/* Calibrated frozen keyboard, protocol version 2. Included after the authenticated
 * control sender. Sequence per connection (precommit failures keep exact legacy):
 *   HELLO(declared reserve + client identity) -> HELLO_ACK(host identity)
 *   >=10000 answered unsequenced CAL exchanges over >=30 s -> drain -> seal
 *   REPORT(all round trips) + SEALED -> host fits independently -> OFFER
 *   client compares OFFER with its own fit, checks identity/expiry/neutral keys,
 *   commits locally, COMMIT -> READY -> timed originals until the finite expiry.
 * After commitment every failure is terminal: no legacy replay, no re-anchoring.
 * Timestamps: originals (caller-supplied usage) must carry their pre-queue capture;
 * zero is refused after commitment. Only a synthetic release (usage zero, key up,
 * resolved to a held key) may use this API's callback time, and the trace marks it.
 * Capture availability: registered by the input handler once its pre-queue filter
 * is actually installed and cleared at its teardown. It is required at COMMIT;
 * originals fail once it is cleared. It survives ControlInit (steadyInputInit). */
#include "SteadyInput.h"
#if defined(_WIN32)
/* Written by the UI thread (handler), read by the receive and input threads.
 * Process lifetime: never reset by init, stop or destroy. */
static volatile LONG siCaptureAvailable;
void LiSteadyInputSetCaptureAvailable(int available) { InterlockedExchange(&siCaptureAvailable,available ? 1 : 0); }
static bool siCaptureIsAvailable(void) { return InterlockedCompareExchange(&siCaptureAvailable,0,0)!=0; }
enum {
    SIS_LEGACY = 0,       /* Precommit fallback or disabled; exact legacy path. */
    SIS_HELLO_SENT = 1,   /* Precommit. */
    SIS_CALIBRATING = 2,  /* Precommit. */
    SIS_SEALED_WAIT = 3,  /* Precommit: reports sent, awaiting OFFER. */
    SIS_COMMITTED = 4,    /* Postcommit: COMMIT sent, keys buffered until READY. */
    SIS_ACTIVE = 5,       /* Postcommit: timed originals. */
    SIS_TERMINAL = 6      /* Postcommit failure or teardown. */
};
#ifndef STEADY_INPUT_CLIENT_PLATFORM
/* Process-lifetime client continuity source mirroring steady::WindowsPowerEpoch:
 * random incarnation plus a suspend/resume counter, never per connection. The
 * registration and its parameters remain valid until process exit. */
typedef struct { ULONG (CALLBACK *Callback)(PVOID,ULONG,PVOID); PVOID Context; } siPowerParameters;
static volatile LONG64 siPowerEpoch = 1;
static uint64_t siIncarnation[2];
static bool siPowerTried, siPowerRegistered;
static siPowerParameters siPowerParams;
static ULONG CALLBACK siPowerCallback(PVOID context, ULONG type, PVOID setting) {
    (void)context; (void)setting;
    if (type == PBT_APMSUSPEND || type == PBT_APMRESUMESUSPEND || type == PBT_APMRESUMEAUTOMATIC) InterlockedIncrement64(&siPowerEpoch);
    return 0;
}
static bool siPlatformIdentity(uint64_t* high, uint64_t* low, uint64_t* epoch) {
    if (!siPowerTried) {
        typedef PVOID (WINAPI *siRegisterFn)(HANDLE,DWORD);
        HMODULE user32 = GetModuleHandleA("user32.dll");
        siRegisterFn reg = user32 ? (siRegisterFn)GetProcAddress(user32,"RegisterSuspendResumeNotification") : NULL;
        siPowerTried = true;
        PltGenerateRandomData((unsigned char*)siIncarnation,sizeof(siIncarnation));
        siPowerParams.Callback = siPowerCallback; siPowerParams.Context = NULL;
        /* 2 is DEVICE_NOTIFY_CALLBACK; the SDK declaration needs Windows 8 headers. */
        if (reg && (siIncarnation[0] || siIncarnation[1])) siPowerRegistered = reg((HANDLE)&siPowerParams,2) != NULL;
    }
    if (!siPowerRegistered) return false;
    *high = siIncarnation[0]; *low = siIncarnation[1];
    *epoch = (uint64_t)InterlockedCompareExchange64(&siPowerEpoch,0,0);
    return *epoch != 0;
}
/* Physical-neutral client keyboard at commitment: no key reported down. */
static bool siPlatformNeutral(void) {
    int vk;
    for (vk = 0x08; vk <= 0xFE; ++vk) if (GetAsyncKeyState(vk) & 0x8000) return false;
    return true;
}
#endif
static PLT_MUTEX siMutex;
static PLT_THREAD siThread;
static bool siEnabled,siThreadStarted,siCalAccept,siNotifyReject;
static uint32_t siPeerVersion;
static unsigned siState,siReason;
static unsigned char siOffer[STEADY_INPUT_BYTES],siExpected[STEADY_INPUT_BYTES],siPending[64][STEADY_INPUT_BYTES];
static unsigned siPendingCount;
static uint64_t siGeneration,siReserve,siSequence,siFrequency,siLast,siStateTime,siCommitTime,siExpiry;
static uint64_t siIncHigh,siIncLow,siEpoch,siHostFrequency,siHostEpoch,siHostIncHigh,siHostIncLow;
static steady_cal_exchange siCal[STEADY_CAL_CAPACITY]; /* Fixed inventory; every attempt retained. */
static uint64_t siCalAttempted,siCalUsable,siCalDuplicates,siCalInvalid,siCalSealedAt;
static unsigned siCalStop;
static steady_cal_result siFit;
static unsigned short siCodes[256];
static unsigned char siFlags[256],siModifiers[256],siHeld[256];
static uint64_t siTrace[STEADY_INPUT_CLIENT_TRACE_ROWS][9]; /* Static storage: no allocation on the input path. */
static unsigned siTraceUsed,siTraceOverflow;
static uint64_t siAbandoned;
static char siTracePath[32768];
static bool steadyInputSend(const unsigned char* frame,bool reliable) {
    return sendMessageAndForget(STEADY_INPUT_TYPE,STEADY_INPUT_BYTES,frame,CTRL_CHANNEL_KEYBOARD,
                                reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED,false);
}
/* Caller holds siMutex. Brackets two QPC/frequency reads with identity reads; any change of
 * incarnation, power epoch or frequency, or a local regression, fails. */
static bool siCheckedNow(uint64_t* now) {
    uint64_t h1,l1,e1,h2,l2,e2,t,f,t2,f2;
    if (!siPlatformIdentity(&h1,&l1,&e1) || !steady_probe_now(&t,&f) || !steady_probe_now(&t2,&f2) || !siPlatformIdentity(&h2,&l2,&e2)) return false;
    if (h1!=h2 || l1!=l2 || e1!=e2 || h1!=siIncHigh || l1!=siIncLow || e1!=siEpoch || f!=siFrequency || f2!=f || t<siLast || t2<t) return false;
    siLast=t2; *now=t2; return true;
}
static void siLegacyLocked(unsigned reason) {
    if (siState>=SIS_HELLO_SENT && siState<=SIS_SEALED_WAIT) { siState=SIS_LEGACY; siReason=reason; siNotifyReject=true; siCalAccept=false; }
}
static void siTerminalLocked(unsigned reason) {
    if (siState!=SIS_TERMINAL) { siState=SIS_TERMINAL; siReason=reason; siNotifyReject=true; siCalAccept=false; }
}
static void steadyInputInit(void) {
    DWORD n;
    siEnabled=false; siThreadStarted=false; siState=SIS_LEGACY; siReason=0; siPendingCount=0; siSequence=0;
    siTraceUsed=siTraceOverflow=0; siAbandoned=0; siCalAccept=siNotifyReject=false;
    siCalAttempted=siCalUsable=siCalDuplicates=siCalInvalid=siCalSealedAt=0; siCalStop=STEADY_PROBE_STOP_THREAD;
    memset(siHeld,0,sizeof(siHeld)); memset(siCal,0,sizeof(siCal)); memset(&siFit,0,sizeof(siFit));
    memset(siOffer,0,sizeof(siOffer)); memset(siExpected,0,sizeof(siExpected));
    siHostFrequency=siHostEpoch=siHostIncHigh=siHostIncLow=0; siExpiry=siCommitTime=0;
    n=GetEnvironmentVariableA("STEADY_INPUT_TRACE",siTracePath,sizeof(siTracePath));
    siReserve=steady_input_reserve();
    if (!steady_input_opt_in() || siPeerVersion!=STEADY_INPUT_VERSION || !siReserve || !n || n>=sizeof(siTracePath)-32 ||
        !encryptedControlStream || !(EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2)) return;
    if (!siPlatformIdentity(&siIncHigh,&siIncLow,&siEpoch) || !steady_probe_now(&siLast,&siFrequency) || PltCreateMutex(&siMutex)) return;
    siGeneration=0; PltGenerateRandomData((unsigned char*)&siGeneration,sizeof(siGeneration));
    if (!siGeneration) { PltDeleteMutex(&siMutex); return; }
    siEnabled=true;
}
static enum steady_probe_stop siCalibrate(void) {
    uint64_t seq,t1=0,started=0;
    unsigned char frame[STEADY_INPUT_BYTES];
    enum steady_probe_stop reason=STEADY_PROBE_STOP_CANCEL;
    for (seq=0; seq<STEADY_CAL_CAPACITY && !PltIsThreadInterrupted(&siThread); ++seq) {
        PltLockMutex(&siMutex);
        if (siState!=SIS_CALIBRATING) { PltUnlockMutex(&siMutex); return STEADY_PROBE_STOP_CANCEL; }
        if (!siCheckedNow(&t1)) { siLegacyLocked(STEADY_PROBE_STOP_CLOCK); PltUnlockMutex(&siMutex); return STEADY_PROBE_STOP_CLOCK; }
        if (!started) started=t1;
        reason=steady_probe_collection_stop(seq,siCalUsable,t1-started);
        if (reason!=STEADY_PROBE_RUNNING) { PltUnlockMutex(&siMutex); return reason; }
        siCal[seq].t1=t1; siCalAttempted=seq+1;
        PltUnlockMutex(&siMutex);
        steady_input_frame(frame,SI_CAL_REQ,siGeneration); steady_input_set(frame,0,seq); steady_input_set(frame,1,t1);
        if (!steadyInputSend(frame,false)) return STEADY_PROBE_STOP_SEND;
        reason=STEADY_PROBE_STOP_CANCEL;
        PltSleepMsInterruptible(&siThread,STEADY_INPUT_CAL_PERIOD_MS);
    }
    return seq==STEADY_CAL_CAPACITY ? STEADY_PROBE_STOP_CAPACITY : reason;
}
/* One fixed drain, then seal under the receive lock: a late reply cannot join
 * the inventory afterwards. The client fits the exact table it reports. */
static void siSeal(enum steady_probe_stop reason) {
    unsigned char frame[STEADY_INPUT_BYTES];
    uint64_t now=0,base,attempted=0,usable=0,i;
    bool send=false;
    PltSleepMsInterruptible(&siThread,STEADY_PROBE_DRAIN_MS);
    if (PltIsThreadInterrupted(&siThread)) reason=STEADY_PROBE_STOP_CANCEL;
    PltLockMutex(&siMutex);
    siCalAccept=false; siCalStop=(unsigned)reason;
    if (siState==SIS_CALIBRATING) {
        if (reason!=STEADY_PROBE_STOP_TARGET || !siCheckedNow(&now)) siLegacyLocked(100u+(unsigned)reason);
        else {
            int status;
            siCalSealedAt=now;
            for (i=0;i<siCalAttempted;++i) if (steady_cal_report_code(&siCal[i])==STEADY_CAL_RTT_OVERSIZE) break;
            status=i<siCalAttempted ? STEADY_CAL_MALFORMED : steady_cal_fit(siCal,siCalAttempted,siReserve,&siFit);
            if (status!=STEADY_CAL_OK) siLegacyLocked(200u+(unsigned)status);
            else {
                steady_input_frame(siExpected,SI_OFFER,siGeneration); steady_input_agreement(siExpected,&siFit);
                siState=SIS_SEALED_WAIT; siStateTime=now; attempted=siCalAttempted; usable=siCalUsable; send=true;
            }
        }
    }
    PltUnlockMutex(&siMutex);
    for (base=0; send && base<attempted; base+=STEADY_INPUT_REPORT_CODES) {
        unsigned k;
        steady_input_frame(frame,SI_REPORT,siGeneration); steady_input_set(frame,0,base);
        for (k=0;k<STEADY_INPUT_REPORT_CODES;++k) if (base+k<attempted) steady_input_set_code(frame,k,steady_cal_report_code(&siCal[base+k]));
        send=steadyInputSend(frame,true);
    }
    if (attempted) {
        if (send) {
            steady_input_frame(frame,SI_SEALED,siGeneration); steady_input_set(frame,0,attempted);
            steady_input_set(frame,1,(uint64_t)reason); steady_input_set(frame,2,now); steady_input_set(frame,3,usable);
            send=steadyInputSend(frame,true);
        }
        if (!send) { PltLockMutex(&siMutex); siLegacyLocked(300); PltUnlockMutex(&siMutex); }
    }
}
static void siSendReject(unsigned reason) {
    unsigned char frame[STEADY_INPUT_BYTES];
    steady_input_frame(frame,SI_REJECT,siGeneration); steady_input_set(frame,0,reason);
    (void)steadyInputSend(frame,true);
}
/* Precommit timeouts select legacy; postcommit expiry, identity change or a
 * missing READY is terminal. The connection is closed, never re-anchored. */
static void siWatch(void) {
    for (;;) {
        uint64_t now=0;
        bool terminate=false,reject,done=false;
        unsigned rejectionReason=0,sleep=STEADY_INPUT_CAL_PERIOD_MS;
        if (PltIsThreadInterrupted(&siThread)) return;
        PltLockMutex(&siMutex);
        switch (siState) {
        case SIS_HELLO_SENT:
            if (!siCheckedNow(&now) || now-siStateTime>=STEADY_INPUT_ACK_TIMEOUT_NS) siLegacyLocked(400);
            break;
        case SIS_SEALED_WAIT:
            if (!siCheckedNow(&now) || now-siStateTime>=STEADY_INPUT_OFFER_TIMEOUT_NS) siLegacyLocked(401);
            break;
        case SIS_COMMITTED:
            if (!siCheckedNow(&now) || now-siStateTime>=STEADY_INPUT_READY_TIMEOUT_NS || now>=siExpiry) { siTerminalLocked(500); terminate=true; }
            break;
        case SIS_ACTIVE:
            sleep=100;
            if (!siCheckedNow(&now) || now>=siExpiry) { siTerminalLocked(501); terminate=true; }
            break;
        case SIS_CALIBRATING:
            done=true; /* The caller continues calibration. */
            break;
        default:
            done=true;
        }
        reject=siNotifyReject; rejectionReason=siReason; siNotifyReject=false;
        PltUnlockMutex(&siMutex);
        if (reject) siSendReject(rejectionReason);
        if (terminate) { Limelog("STEADY input agreement ended (expiry or clock discontinuity); reconnect for fresh calibration\n"); ListenerCallbacks.connectionTerminated(-1); return; }
        if (done) return;
        PltSleepMsInterruptible(&siThread,sleep);
    }
}
static void steadyInputThreadMain(void* context) {
    (void)context;
    siWatch(); /* HELLO_ACK wait; returns once calibration may start or legacy was selected. */
    PltLockMutex(&siMutex);
    if (siState==SIS_CALIBRATING) {
        PltUnlockMutex(&siMutex);
        siSeal(siCalibrate());
    } else PltUnlockMutex(&siMutex);
    siWatch();
}
static void steadyInputStart(void) {
    uint64_t now=0;
    unsigned char hello[STEADY_INPUT_BYTES];
    if (!siEnabled) return;
    PltLockMutex(&siMutex);
    if (!siCheckedNow(&now)) { PltUnlockMutex(&siMutex); return; }
    steady_input_frame(hello,SI_HELLO,siGeneration);
    steady_input_set(hello,0,siReserve); steady_input_set(hello,1,siFrequency); steady_input_set(hello,2,siEpoch);
    steady_input_set(hello,3,siIncHigh); steady_input_set(hello,4,siIncLow);
    siState=SIS_HELLO_SENT; siStateTime=now;
    PltUnlockMutex(&siMutex);
    if (PltCreateThread("SteadyInput",steadyInputThreadMain,NULL,&siThread)) { PltLockMutex(&siMutex); siState=SIS_LEGACY; PltUnlockMutex(&siMutex); return; }
    siThreadStarted=true;
    if (!steadyInputSend(hello,true)) { PltLockMutex(&siMutex); siLegacyLocked(301); siNotifyReject=false; PltUnlockMutex(&siMutex); }
}
static bool siAnyHeld(void) {
    unsigned i;
    for (i=0;i<256;++i) if (siHeld[i]) return true;
    return false;
}
static void steadyInputReceive(const unsigned char* frame,size_t size) {
    bool failed=false;
    uint64_t now=0,seq;
    if (!siEnabled || !steady_input_valid(frame,size) || steady_probe_get64(frame+8)!=siGeneration) return;
    PltLockMutex(&siMutex);
    switch (frame[5]) {
    case SI_HELLO_ACK:
        if (siState!=SIS_HELLO_SENT) break;
        if (steady_input_slot(frame,0)!=siReserve || !siCheckedNow(&now)) { siLegacyLocked(402); break; }
        siHostFrequency=steady_input_slot(frame,1); siHostEpoch=steady_input_slot(frame,2);
        siHostIncHigh=steady_input_slot(frame,3); siHostIncLow=steady_input_slot(frame,4);
        siState=SIS_CALIBRATING; siStateTime=now; siCalAccept=true;
        break;
    case SI_CAL_REP:
        if (!siCalAccept || siState!=SIS_CALIBRATING) break;
        seq=steady_input_slot(frame,0);
        if (seq>=siCalAttempted || siCal[seq].t1!=steady_input_slot(frame,1)) { ++siCalInvalid; break; }
        if (siCal[seq].t4) { ++siCalDuplicates; break; }
        if (!siCheckedNow(&now)) { siLegacyLocked(403); break; }
        siCal[seq].t2=steady_input_slot(frame,2); siCal[seq].t3=steady_input_slot(frame,3); siCal[seq].t4=now;
        if (now>=siCal[seq].t1 && siCal[seq].t3-siCal[seq].t2<=now-siCal[seq].t1) ++siCalUsable;
        break;
    case SI_OFFER:
        if (siState!=SIS_SEALED_WAIT) break;
        /* Independent validation: the host's agreement must equal this client's fit
         * of the same reported inventory, bit for bit. */
        if (!steady_input_same_offer(frame,siExpected)) { siLegacyLocked(404); break; }
        if (!siCheckedNow(&now) || now<siFit.client_anchor || now>=siFit.client_valid_until) { siLegacyLocked(405); break; }
        if (siAnyHeld() || !siPlatformNeutral()) { siLegacyLocked(406); break; }
        /* No installed pre-queue capture (unsupported SDL/filter profile, or handler
         * gone): decline while still precommit, so the session stays exact legacy. */
        if (!siCaptureIsAvailable()) { siLegacyLocked(408); break; }
        memcpy(siOffer,frame,STEADY_INPUT_BYTES); siOffer[5]=SI_COMMIT;
        siState=SIS_COMMITTED; /* From here onward, never replay through the legacy path. */
        siStateTime=siCommitTime=now; siExpiry=siFit.client_valid_until;
        if (!steadyInputSend(siOffer,true)) { siTerminalLocked(502); failed=true; }
        break;
    case SI_READY:
        if (siState==SIS_COMMITTED && steady_input_same_offer(frame,siOffer)) {
            unsigned i;
            siState=SIS_ACTIVE;
            for (i=0;i<siPendingCount;++i) if (!steadyInputSend(siPending[i],true)) { siTerminalLocked(503); failed=true; break; }
            siPendingCount=0;
            if (!failed) Limelog("STEADY keyboard active: calibrated frozen map, %" PRIu64 " ms reserve, %" PRIu64 " s validity\n",siReserve/1000000,siFit.age/1000000000);
        }
        break;
    case SI_REJECT:
        if (siState>=SIS_HELLO_SENT && siState<=SIS_SEALED_WAIT) { siLegacyLocked(407); siNotifyReject=false; }
        else if (siState==SIS_COMMITTED || siState==SIS_ACTIVE) { siTerminalLocked(504); siNotifyReject=false; failed=true; }
        break;
    default:
        break;
    }
    PltUnlockMutex(&siMutex);
    if (failed) ListenerCallbacks.connectionTerminated(-1);
}
/* Caller supplies USB usage, or zero for a synthetic release of an already held key.
 * captureNs is the original pre-queue client QPC time in ns. Zero is refused for
 * originals after commitment; only the synthetic release may fall back to now. */
int LiSendKeyboardEventTimedAt(unsigned short usage,short keyCode,char action,char modifiers,char flags,uint64_t captureNs) {
    uint64_t now=0,capture=0; unsigned char frame[STEADY_INPUT_BYTES]; bool failed=false,sent=true,traced=false; unsigned i;
    const bool synthetic=!usage && action==KEY_ACTION_UP;
    if (!siEnabled) return LiSendKeyboardEvent2(keyCode,action,modifiers,flags);
    PltLockMutex(&siMutex);
    if (siState<=SIS_SEALED_WAIT) {
        siLegacyLocked(600); /* A key before commitment keeps this entire session legacy. */
        PltUnlockMutex(&siMutex); return LiSendKeyboardEvent2(keyCode,action,modifiers,flags);
    }
    if (siState==SIS_TERMINAL) { ++siAbandoned; PltUnlockMutex(&siMutex); return -1; }
    if (synthetic) {
        for (i=4;i<256;++i) if (siHeld[i] && siCodes[i]==(unsigned short)keyCode && siFlags[i]==(unsigned char)flags &&
            (siModifiers[i]&MODIFIER_EXTENDED)==(modifiers&MODIFIER_EXTENDED)) { usage=(unsigned short)i; break; }
        if (!usage) { PltUnlockMutex(&siMutex); return 0; }
    }
    if (usage<4 || usage>=256 || siSequence==UINT64_MAX || !siCheckedNow(&now) || now>=siExpiry) failed=true;
    else if (!synthetic && !siCaptureIsAvailable()) failed=true; /* Capture withdrawn after commitment. */
    else if (!synthetic && !captureNs) failed=true; /* Original without capture: never substitute callback time. */
    else {
        /* Plausibility: not in the future, not before local commitment (that would
         * be a precommit original), and inside the admitted mapping validity. */
        capture=captureNs ? captureNs : now;
        if (capture>now || capture<siCommitTime || capture<siFit.client_anchor || capture>=siExpiry) failed=true;
    }
    if (!failed) {
        steady_input_frame(frame,SI_KEY,siGeneration);
        steady_input_set(frame,0,++siSequence); steady_input_set(frame,1,capture);
        memcpy(frame+32,siOffer+16,24); frame[57]=(unsigned char)usage; frame[58]=action==KEY_ACTION_DOWN;
        siHeld[usage]=frame[58]; siCodes[usage]=(unsigned short)keyCode; siFlags[usage]=(unsigned char)flags; siModifiers[usage]=(unsigned char)modifiers;
        if (siState==SIS_COMMITTED) {
            if (siPendingCount==64) failed=true;
            else memcpy(siPending[siPendingCount++],frame,sizeof(frame));
        } else if (!(sent=steadyInputSend(frame,true))) failed=true;
        traced=true;
    }
    if (traced || usage>=4) {
        /* Every original is retained, including the one that ended the session. */
        if (siTraceUsed<STEADY_INPUT_CLIENT_TRACE_ROWS) {
            uint64_t* row=siTrace[siTraceUsed++];
            row[0]=traced?siSequence:0; row[1]=capture; row[2]=usage; row[3]=action==KEY_ACTION_DOWN; row[4]=sent&&!failed; row[5]=siState; row[6]=now; row[7]=captureNs!=0; row[8]=synthetic;
        } else { ++siTraceOverflow; failed=true; }
    }
    if (failed) { ++siAbandoned; siTerminalLocked(601); }
    PltUnlockMutex(&siMutex);
    if (failed) ListenerCallbacks.connectionTerminated(-1);
    return failed?-1:0;
}
int LiSendKeyboardEventTimed(unsigned short usage,short keyCode,char action,char modifiers,char flags) {
    return LiSendKeyboardEventTimedAt(usage,keyCode,action,modifiers,flags,0);
}
static void steadyInputStop(void) {
    if (!siEnabled) return;
    PltLockMutex(&siMutex); siTerminalLocked(700); siNotifyReject=false; PltUnlockMutex(&siMutex);
    if (siThreadStarted) { PltInterruptThread(&siThread); PltJoinThread(&siThread); siThreadStarted=false; }
}
static void siWriteCalibration(void) {
    static char path[sizeof(siTracePath)+20];
    FILE* file=NULL; uint64_t i;
    const steady_cal_result* r=&siFit;
    if (!siCalAttempted) return;
    snprintf(path,sizeof(path),"%s.calibration.csv",siTracePath);
#if defined(_MSC_VER)
    fopen_s(&file,path,"wx");
#else
    file=fopen(path,"wx");
#endif
    if (!file) { Limelog("STEADY calibration evidence not written: choose a new writable filename\n"); return; }
    fprintf(file,"sequence,t1_ns,t2_ns,t3_ns,t4_ns,report_code,generation,reserve_ns,stop_reason,fit_status,attempted,answered,unanswered,late,contradictions,duplicates,invalid,"
                 "rate_ppb,rate_low_ppb,rate_high_ppb,rate_error_ppb,lower_ns,upper_ns,client_anchor_ns,host_anchor_ns,initial_error_ns,age_ns,client_valid_until_ns,host_valid_until_ns,"
                 "client_frequency,client_epoch,client_incarnation_high,client_incarnation_low,host_frequency,host_epoch,host_incarnation_high,host_incarnation_low,reason\n");
    for (i=0;i<siCalAttempted;++i) {
        const steady_cal_exchange* e=&siCal[i];
        fprintf(file,"%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
                     "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
                     "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u\n",
                i,e->t1,e->t2,e->t3,e->t4,(unsigned)steady_cal_report_code(e),siGeneration,siReserve,siCalStop,r->status,r->attempted,r->answered,r->unanswered,r->late,r->contradictions,siCalDuplicates,siCalInvalid,
                r->rate,r->rate_low,r->rate_high,r->rate_error,r->lower,r->upper,r->client_anchor,r->host_anchor,r->initial_error,r->age,r->client_valid_until,r->host_valid_until,
                siFrequency,siEpoch,siIncHigh,siIncLow,siHostFrequency,siHostEpoch,siHostIncHigh,siHostIncLow,siReason);
    }
    if (ferror(file)) Limelog("STEADY calibration evidence write failed\n");
    if (fclose(file)) Limelog("STEADY calibration evidence close failed\n");
}
static void steadyInputDestroy(void) {
    FILE* file=NULL; unsigned i;
    if (!siEnabled) return;
    siWriteCalibration();
#if defined(_MSC_VER)
    fopen_s(&file,siTracePath,"wx");
#else
    file=fopen(siTracePath,"wx");
#endif
    if (file) {
        /* Original columns first; callback time, original-capture flag and (last)
         * synthetic-release class appended. Synthetic rows are not originals. */
        fprintf(file,"sequence,capture_ns,usage,down,queued_ok,state,generation,overflow,callback_ns,original_capture,abandoned,reason,synthetic\n");
        for (i=0;i<siTraceUsed;++i) fprintf(file,"%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64 "\n",
                                            siTrace[i][0],siTrace[i][1],siTrace[i][2],siTrace[i][3],siTrace[i][4],siTrace[i][5],siGeneration,siTraceOverflow,siTrace[i][6],siTrace[i][7],siAbandoned,siReason,siTrace[i][8]);
        if (ferror(file)) Limelog("STEADY input trace write failed\n");
        if (fclose(file)) Limelog("STEADY input trace close failed\n");
    } else Limelog("STEADY input trace not written: choose a new writable filename\n");
    siEnabled=false; PltDeleteMutex(&siMutex);
}
#else
/* Non-Windows: timed keyboard is never enabled; registration is accepted and ignored. */
void LiSteadyInputSetCaptureAvailable(int available) { (void)available; }
static uint32_t siPeerVersion;
static void steadyInputInit(void) {}
static void steadyInputStart(void) {}
static void steadyInputStop(void) {}
static void steadyInputDestroy(void) {}
static void steadyInputReceive(const unsigned char* p,size_t n) { (void)p; (void)n; }
int LiSendKeyboardEventTimedAt(unsigned short usage,short code,char action,char mods,char flags,uint64_t captureNs) { (void)usage; (void)captureNs; return LiSendKeyboardEvent2(code,action,mods,flags); }
int LiSendKeyboardEventTimed(unsigned short usage,short code,char action,char mods,char flags) { (void)usage; return LiSendKeyboardEvent2(code,action,mods,flags); }
#endif
void steadyInputSetPeerVersion(uint32_t version) { siPeerVersion=version; }
