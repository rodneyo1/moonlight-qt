/* Real client adapter and shared fitter under deterministic transport, clock and identity.
 * Synthetic exchanges come from a stated true affine clock with asymmetric delays; the
 * checks assert soundness (truth inside the bounds, map error inside E0 + growth),
 * rejection and lifecycle rules, never particular fitted values. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include "SteadyInput.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while (0)
#define CTRL_CHANNEL_KEYBOARD 2
#define ENET_PACKET_FLAG_RELIABLE 1
#define ENET_PACKET_FLAG_UNSEQUENCED 2
#define SS_ENC_CONTROL_V2 1
#define KEY_ACTION_DOWN 3
#define KEY_ACTION_UP 4
#define MODIFIER_EXTENDED 0x10
#define HOST_OFFSET UINT64_C(5000000000)
#define FWD_NS UINT64_C(150000)
#define HOST_NS UINT64_C(20000)
#define BACK_NS UINT64_C(250000)
static bool encryptedControlStream=true;
static int EncryptionFeaturesEnabled=1;
typedef int PLT_MUTEX;
typedef struct { bool interrupted; } PLT_THREAD;
static uint64_t fakeTime=1000000000,fakeFrequency=10000000,fakeEpoch=1;
static unsigned nowCalls,frequencyChangeAt;
static unsigned totalSent,legacy,terminated,locks,kindSent[16],dropEvery,dropFirst;
static bool sendFails,clockFails,identityFails,fakeNeutral=true,hostAuto=true;
static unsigned char lastFrame[64],lastKindFrame[16][64];
/* Simulated host: retains its own t1/t2/t3 and decodes the client's REPORT frames. */
static steady_cal_exchange hostTable[STEADY_CAL_CAPACITY];
static uint64_t hostAttempted;
static steady_cal_result hostFit;
static int hostStatus=-1;
static int PltCreateMutex(PLT_MUTEX* m) { *m=1; return 0; }
static void PltDeleteMutex(PLT_MUTEX* m) { *m=0; }
static void PltLockMutex(PLT_MUTEX* m) { (void)m; ++locks; }
static void PltUnlockMutex(PLT_MUTEX* m) { (void)m; --locks; }
static void PltGenerateRandomData(unsigned char* p,size_t n) { memset(p,7,n); }
static bool PltIsThreadInterrupted(PLT_THREAD* t) { return t->interrupted; }
static int PltCreateThread(const char* n,void (*f)(void*),void* c,PLT_THREAD* t) { (void)n;(void)f;(void)c;t->interrupted=false;return 0; }
static void PltSleepMsInterruptible(PLT_THREAD* t,unsigned ms) { (void)t;fakeTime+=(uint64_t)ms*1000000; }
static void PltInterruptThread(PLT_THREAD* t) { t->interrupted=true; }
static void PltJoinThread(PLT_THREAD* t) { (void)t; }
static bool fake_now(uint64_t* now,uint64_t* frequency) { if (clockFails) return false;*now=fakeTime;*frequency=fakeFrequency;if (++nowCalls==frequencyChangeAt) ++fakeFrequency;return true; }
static void Limelog(const char* f,...) { (void)f; }
static void terminated_cb(int e) { (void)e; ++terminated; }
static struct { void (*connectionTerminated)(int); } ListenerCallbacks={terminated_cb};
#define STEADY_INPUT_CLIENT_PLATFORM 1
static bool siPlatformIdentity(uint64_t* high,uint64_t* low,uint64_t* epoch) { if (identityFails) return false; *high=11; *low=22; *epoch=fakeEpoch; return true; }
static bool siPlatformNeutral(void) { return fakeNeutral; }
static void steadyInputReceive(const unsigned char* frame,size_t size);
static uint64_t siReserveForHost(void);
static bool sendMessageAndForget(short type,short size,const void* p,uint8_t channel,uint32_t flags,bool more) {
    const unsigned char* f=(const unsigned char*)p;
    if (type!=STEADY_INPUT_TYPE || size!=64 || channel!=2 || more || !steady_input_valid(f,64)) return false;
    if (flags!=(f[5]==SI_CAL_REQ ? (uint32_t)ENET_PACKET_FLAG_UNSEQUENCED : (uint32_t)ENET_PACKET_FLAG_RELIABLE)) return false;
    ++totalSent; ++kindSent[f[5]]; memcpy(lastFrame,p,64); memcpy(lastKindFrame[f[5]],p,64);
    if (sendFails) return false;
    if (hostAuto && f[5]==SI_CAL_REQ) {
        /* Rate-1 host, asymmetric one-way delays; every third reply is slower. */
        uint64_t seq=steady_input_slot(f,0),t1=steady_input_slot(f,1),extra=seq%3==0 ? 400000 : 0;
        unsigned char r[64];
        hostTable[seq].t1=t1; hostTable[seq].t2=t1+HOST_OFFSET+FWD_NS+extra; hostTable[seq].t3=hostTable[seq].t2+HOST_NS;
        if (seq>=hostAttempted) hostAttempted=seq+1;
        fakeTime=t1+FWD_NS+extra+HOST_NS+BACK_NS;
        if ((dropEvery && seq%dropEvery==0) || seq<dropFirst) return true; /* Reply lost in transit. */
        steady_input_frame(r,SI_CAL_REP,steady_probe_get64(f+8));
        steady_input_set(r,0,seq); steady_input_set(r,1,t1); steady_input_set(r,2,hostTable[seq].t2); steady_input_set(r,3,hostTable[seq].t3);
        steadyInputReceive(r,64);
    } else if (hostAuto && f[5]==SI_REPORT) {
        uint64_t base=steady_input_slot(f,0); unsigned k;
        for (k=0;k<STEADY_INPUT_REPORT_CODES;++k) {
            uint32_t code=steady_input_code(f,k);
            if (code && base+k<STEADY_CAL_CAPACITY) hostTable[base+k].t4=hostTable[base+k].t1+code-1;
        }
    } else if (hostAuto && f[5]==SI_SEALED) {
        hostStatus=steady_cal_fit(hostTable,steady_input_slot(f,0),siReserveForHost(),&hostFit);
    }
    return true;
}
int LiSendKeyboardEvent2(short code,char action,char modifiers,char flags) { (void)code;(void)action;(void)modifiers;(void)flags;++legacy;return 17; }
#define steady_probe_now fake_now
#include "SteadyInputClient.inc"
#undef steady_probe_now
static uint64_t siReserveForHost(void) { return siReserve; }

/* Exact true clock: H(c) = h0 + (c-c0)*(1e6+37)/1e6, i.e. rate 1.000037. */
#define TRUE_NUM UINT64_C(1000037)
#define TRUE_DEN UINT64_C(1000000)
static const uint64_t c0=UINT64_C(7000000000),h0=UINT64_C(900000000000);
static steady_cal_exchange ex[STEADY_CAL_CAPACITY];
static uint64_t truth_floor(uint64_t c) { return h0+(c-c0)*TRUE_NUM/TRUE_DEN; }
static uint64_t truth_ceil(uint64_t c) { uint64_t p=(c-c0)*TRUE_NUM; return h0+p/TRUE_DEN+(p%TRUE_DEN!=0); }
static uint64_t inverse_ceil(uint64_t h) { uint64_t p=(h-h0)*TRUE_DEN; return c0+p/TRUE_NUM+(p%TRUE_NUM!=0); }
static uint32_t lcg=12345;
static uint64_t jitter(void) { lcg=lcg*1103515245u+12345u; return (uint64_t)((lcg>>8)%2000000u); }
/* count exchanges spaced by period; most carry up to 2 ms jitter, one in 50 is at the delay floor. */
static void synth(unsigned count,uint64_t period,uint64_t fwd,uint64_t back) {
    unsigned i;
    memset(ex,0,sizeof(ex)); lcg=12345;
    for (i=0;i<count;++i) {
        uint64_t f=fwd+(i%50 ? jitter() : 0),b=back+(i%50 ? jitter() : 0);
        ex[i].t1=c0+1000+(uint64_t)i*period;
        ex[i].t2=truth_ceil(ex[i].t1)+f;
        ex[i].t3=ex[i].t2+HOST_NS;
        ex[i].t4=inverse_ceil(ex[i].t3+b);
    }
}
static int fitter_tests(void) {
    steady_cal_result r,again; uint64_t t,g,cand,lowTruth,highTruth,s,rem; unsigned i;
    synth(12000,3000000,150000,250000);
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_CLEAN,&r)==STEADY_CAL_OK);
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_CLEAN,&again)==STEADY_CAL_OK && !memcmp(&r,&again,sizeof(r)));
    CHECK(r.answered==12000 && !r.unanswered && !r.contradictions && r.attempted==12000);
    /* Soundness: the true rate and H(C) lie inside the outward bounds. */
    CHECK(r.rate_low<=1000037000 && 1000037000<=r.rate_high && r.rate_low<=r.rate && r.rate<=r.rate_high);
    CHECK(r.lower<=truth_floor(r.client_anchor) && truth_ceil(r.client_anchor)<=r.upper);
    /* Frozen map error stays inside E0 + rate_error*age over the whole admitted age. */
    for (i=0;i<=64;++i) {
        t=r.age/64*i;
        CHECK(steady_cal_muldiv(t,r.rate,STEADY_CAL_PPB,&s,&rem));
        cand=r.host_anchor+s+(rem!=0);
        CHECK(steady_cal_scale(t,r.rate_error,1,&g));
        lowTruth=truth_floor(r.client_anchor+t); highTruth=truth_ceil(r.client_anchor+t);
        CHECK(cand<=lowTruth+r.initial_error+g && highTruth<=cand+r.initial_error+g);
    }
    /* Lifetime comes from the bounds: the budget holds at the age and fails one second later. */
    CHECK(steady_cal_scale(r.age,r.rate_error,1,&g) && r.initial_error+g<=STEADY_CAL_ABSOLUTE_CLEAN);
    CHECK(r.age==STEADY_CAL_MAX_AGE_NS || (steady_cal_scale(r.age+1000000000,r.rate_error,1,&g) && r.initial_error+g>STEADY_CAL_ABSOLUTE_CLEAN));
    CHECK(r.age>=STEADY_CAL_MIN_AGE_NS && r.client_valid_until==r.client_anchor+r.age && r.host_valid_until<=r.host_anchor+r.age);
    CHECK(steady_cal_scale(r.age,r.rate_low,0,&s) && r.host_valid_until<=r.lower+s);
    /* Both pair scales use the clock share of the distortion budgets. */
    CHECK(steady_cal_scale(STEADY_CAL_PAIR_LONG_NS,r.rate_error,1,&g) && g+1<=STEADY_CAL_PAIR_LONG_BUDGET);
    CHECK(steady_cal_scale(STEADY_CAL_PAIR_SHORT_NS,r.rate_error,1,&g) && g+1<=STEADY_CAL_PAIR_SHORT_BUDGET);
    /* STANDARD budget is larger, so its validity cannot be shorter. */
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_STANDARD,&again)==STEADY_CAL_OK && again.age>=r.age && again.rate==r.rate);
    /* Undeclared reserves (including the old 40 ms prototype) are rejected. */
    CHECK(steady_cal_fit(ex,12000,40000000,&again)==STEADY_CAL_ARGUMENT);
    /* Unanswered attempts are misses: 11 of 12000 admits, 12 (0.1 %) refuses. */
    for (i=0;i<11;++i) ex[100+i*700].t4=0;
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_CLEAN,&again)==STEADY_CAL_OK && again.unanswered==11);
    ex[9000].t4=0;
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_CLEAN,&again)==STEADY_CAL_ENVELOPE && again.unanswered==12 && !again.age);
    /* Too few answered, too short a span. */
    synth(12000,3000000,150000,250000);
    for (i=0;i<2001;++i) ex[i*5].t4=0;
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_CLEAN,&again)==STEADY_CAL_TOO_FEW);
    synth(12000,2400000,150000,250000);
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_CLEAN,&again)==STEADY_CAL_TOO_SHORT);
    /* A single malformed answered exchange is never trimmed. */
    synth(12000,3000000,150000,250000);
    ex[6000].t3=ex[6000].t2-1;
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_CLEAN,&again)==STEADY_CAL_MALFORMED && again.contradictions==1);
    /* A causally impossible host receive (before the true send) contradicts the affine model. */
    synth(12000,3000000,150000,250000);
    ex[11000].t2=truth_floor(ex[11000].t1)-60000000; ex[11000].t3=ex[11000].t2+HOST_NS;
    i=(unsigned)steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_CLEAN,&again);
    CHECK((i==STEADY_CAL_CONTRADICTION || i==STEADY_CAL_CANDIDATE) && !again.age);
    synth(12000,3000000,150000,250000);
    ex[11999].t2=h0; ex[11999].t3=h0+1; ex[11999].t4=ex[11999].t1+1;
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_CLEAN,&again)==STEADY_CAL_CONTRADICTION && again.contradictions);
    /* Slow forward paths: CLEAN cannot carry 15 ms arrivals plus drift, STANDARD can
     * (its drift allowance is bounded by its 5 ms clock budget). */
    synth(12000,3000000,150000,250000);
    for (i=0;i<20;++i) { ex[i*600+1].t2+=15000000; ex[i*600+1].t3+=15000000; ex[i*600+1].t4=inverse_ceil(ex[i*600+1].t3+250000); }
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_CLEAN,&again)==STEADY_CAL_ENVELOPE && again.late==20);
    CHECK(steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_STANDARD,&again)==STEADY_CAL_OK && !again.late);
    /* Uniformly slow exchanges cannot bound the rate tightly enough: refuse, do not pretend. */
    synth(12000,3000000,9000000,9000000);
    i=(unsigned)steady_cal_fit(ex,12000,STEADY_CAL_RESERVE_STANDARD,&again);
    CHECK(i==STEADY_CAL_PAIR_ERROR || i==STEADY_CAL_INITIAL_ERROR || i==STEADY_CAL_VALIDITY);
    /* Report codes round-trip exactly, and unrepresentable round trips are flagged. */
    synth(12000,3000000,150000,250000);
    CHECK(steady_cal_report_code(&ex[5])==(uint32_t)(ex[5].t4-ex[5].t1+1));
    ex[5].t4=ex[5].t1+UINT64_C(0xFFFFFFFE); CHECK(steady_cal_report_code(&ex[5])==STEADY_CAL_RTT_OVERSIZE);
    ex[5].t4=0; CHECK(!steady_cal_report_code(&ex[5]));
    /* Portable 128-bit arithmetic. */
    CHECK(steady_cal_muldiv(UINT64_MAX,UINT64_MAX,UINT64_MAX,&s,&rem) && s==UINT64_MAX && !rem);
    CHECK(!steady_cal_muldiv(UINT64_MAX,2,1,&s,&rem) && !steady_cal_muldiv(1,1,0,&s,&rem));
    CHECK(steady_cal_muldiv(UINT64_C(180000000000),UINT64_C(1000037000),STEADY_CAL_PPB,&s,&rem) && s==UINT64_C(180006660000) && !rem);
    return 0;
}

static void prepare(void) {
    if (siEnabled) { steadyInputStop(); siEnabled=false; PltDeleteMutex(&siMutex); }
    totalSent=legacy=terminated=locks=0; memset(kindSent,0,sizeof(kindSent)); dropEvery=dropFirst=0;
    sendFails=clockFails=identityFails=false; fakeNeutral=hostAuto=true;
    fakeTime=1000000000; fakeFrequency=10000000; fakeEpoch=1; nowCalls=frequencyChangeAt=0;
    memset(hostTable,0,sizeof(hostTable)); hostAttempted=0; hostStatus=-1;
    SetEnvironmentVariableA("STEADY_INPUT_ENABLE","1"); SetEnvironmentVariableA("STEADY_INPUT_TRACE","unused-client-test.csv");
    SetEnvironmentVariableA("STEADY_INPUT_PROFILE",NULL);
    steadyInputSetPeerVersion(STEADY_INPUT_VERSION); steadyInputInit();
}
static void ack(void) {
    unsigned char p[64];
    steady_input_frame(p,SI_HELLO_ACK,siGeneration);
    steady_input_set(p,0,siReserve); steady_input_set(p,1,10000000); steady_input_set(p,2,3); steady_input_set(p,3,44); steady_input_set(p,4,55);
    steadyInputReceive(p,64);
}
/* Calibrate through the real sender, receive path, seal and report. */
static void calibrate(void) { prepare(); steadyInputStart(); ack(); siSeal(siCalibrate()); }
static void offer(void) {
    unsigned char p[64];
    steady_input_frame(p,SI_OFFER,siGeneration); steady_input_agreement(p,&hostFit);
    steadyInputReceive(p,64);
}
static void ready(void) { unsigned char p[64]; memcpy(p,siOffer,64); p[5]=SI_READY; steadyInputReceive(p,64); }
static void admit(void) { calibrate(); offer(); ready(); }
/* An original carrying its pre-queue capture at the current fake time. */
static int orig(unsigned short usage,char action) { return LiSendKeyboardEventTimedAt(usage,(short)-32703,action,0,0,fakeTime); }

static int adapter_tests(void) {
    unsigned char p[64],copy[64]; unsigned i;
    LiSteadyInputSetCaptureAvailable(1); /* As the input handler does after a real filter install. */
    CHECK(steady_input_sdp_version("a=x-steady-input.keyboardVersion:2\r\n")==2);
    CHECK(!steady_input_sdp_version("a=x-steady-input.keyboardVersion:1\n")); /* Version 1 peers keep legacy. */
    CHECK(!steady_input_sdp_version("a=x-steady-input.keyboardVersion:2\na=x-steady-input.keyboardVersion:2\n"));
    /* Version mismatch and an undeclared profile both keep the exact legacy path. */
    prepare(); siEnabled=false; PltDeleteMutex(&siMutex);
    steadyInputSetPeerVersion(1); steadyInputInit(); CHECK(!siEnabled && LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==17);
    SetEnvironmentVariableA("STEADY_INPUT_PROFILE","fast"); steadyInputSetPeerVersion(2); steadyInputInit(); CHECK(!siEnabled);
    SetEnvironmentVariableA("STEADY_INPUT_PROFILE","standard"); steadyInputInit(); CHECK(siEnabled && siReserve==STEADY_CAL_RESERVE_STANDARD);
    steadyInputStop(); siEnabled=false; PltDeleteMutex(&siMutex);
    /* HELLO declares the reserve and the bracketed client identity before any input. */
    prepare(); steadyInputStart();
    CHECK(siState==SIS_HELLO_SENT && kindSent[SI_HELLO]==1 && steady_input_slot(lastFrame,0)==STEADY_CAL_RESERVE_CLEAN);
    CHECK(steady_input_slot(lastFrame,1)==fakeFrequency && steady_input_slot(lastFrame,2)==1 && steady_input_slot(lastFrame,3)==11 && steady_input_slot(lastFrame,4)==22);
    /* A key before commitment selects legacy for the session; later host frames cannot revive it. */
    CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==17 && siState==SIS_LEGACY && siNotifyReject);
    ack(); CHECK(siState==SIS_LEGACY && !siCalAccept);
    siWatch(); CHECK(kindSent[SI_REJECT]==1 && !terminated);
    /* HELLO_ACK timeout is a precommit fallback. */
    prepare(); steadyInputStart(); siWatch(); CHECK(siState==SIS_LEGACY && !terminated && kindSent[SI_REJECT]==1);
    /* Full calibration: >=10000 answered over >=30 s, all attempts reported, host fits the same agreement. */
    calibrate();
    CHECK(siState==SIS_SEALED_WAIT && siCalStop==STEADY_PROBE_STOP_TARGET && siCalUsable>=STEADY_CAL_MIN_USABLE);
    CHECK(siCalAttempted==hostAttempted && kindSent[SI_CAL_REQ]==siCalAttempted && kindSent[SI_SEALED]==1);
    CHECK(kindSent[SI_REPORT]==(siCalAttempted+STEADY_INPUT_REPORT_CODES-1)/STEADY_INPUT_REPORT_CODES);
    CHECK(siCal[siCalAttempted-1].t1-siCal[0].t1>=STEADY_CAL_MIN_SPAN_NS);
    CHECK(hostStatus==STEADY_CAL_OK && !memcmp(&hostFit,&siFit,sizeof(hostFit)));
    for (i=0;i<siCalAttempted;++i) if (memcmp(&hostTable[i],&siCal[i],sizeof(siCal[i]))) break;
    CHECK(i==siCalAttempted);
    /* Commit happens locally before COMMIT is sent; keys before READY are buffered, never legacy. */
    offer(); CHECK(siState==SIS_COMMITTED && kindSent[SI_COMMIT]==1 && siExpiry==siFit.client_valid_until && siCommitTime==fakeTime);
    fakeTime+=100; CHECK(!orig(4,3) && siPendingCount==1 && !legacy);
    ready(); CHECK(siState==SIS_ACTIVE && kindSent[SI_KEY]==1 && steady_input_slot(lastFrame,1)==fakeTime);
    CHECK(steady_input_key_bound(lastFrame,siOffer));
    /* A synthetic release keeps a provided capture and is classified synthetic. */
    fakeTime+=5000; CHECK(!LiSendKeyboardEventTimedAt(0,(short)-32703,4,0,0,fakeTime-3000));
    CHECK(lastFrame[57]==4 && !lastFrame[58] && steady_input_slot(lastFrame,1)==fakeTime-3000 && siTrace[siTraceUsed-1][7]==1 && siTrace[siTraceUsed-1][8]==1);
    CHECK(!siTrace[0][8] && siTrace[0][7]==1); /* The buffered original is not synthetic. */
    CHECK(steady_probe_get64(lastFrame+16)==2 && !locks && !legacy);
    /* Implausible captures terminate after commitment: future, or before commitment. */
    fakeTime+=100; CHECK(LiSendKeyboardEventTimedAt(5,(short)-32702,3,0,0,fakeTime+1)==-1 && terminated==1 && siState==SIS_TERMINAL && !legacy);
    admit(); CHECK(siState==SIS_ACTIVE && LiSendKeyboardEventTimedAt(5,(short)-32702,3,0,0,siCommitTime-1)==-1 && terminated==1 && siAbandoned==1);
    /* Expiry from the bounds is enforced at its boundary by the key path and by the watcher. */
    admit(); fakeTime=siExpiry-1; CHECK(!orig(4,3));
    fakeTime=siExpiry; CHECK(orig(4,4)==-1 && terminated==1);
    admit(); siWatch(); CHECK(siState==SIS_TERMINAL && terminated==1 && fakeTime>=siExpiry && kindSent[SI_REJECT]==1);
    /* Suspend/resume (power epoch) or a frequency change ends the active generation. */
    admit(); ++fakeEpoch; CHECK(orig(4,3)==-1 && terminated==1 && !legacy);
    admit(); fakeFrequency++; siWatch(); CHECK(siState==SIS_TERMINAL && terminated==1);
    admit(); nowCalls=0;frequencyChangeAt=1;
    { uint64_t output=123,last=siLast;CHECK(!siCheckedNow(&output) && output==123 && siLast==last); }

    admit(); fakeTime-=1; CHECK(orig(4,3)==-1);
    /* An epoch change during calibration stays precommit: legacy, no termination. */
    prepare(); steadyInputStart(); ack(); fakeEpoch=2; CHECK(siCalibrate()==STEADY_PROBE_STOP_CLOCK && siState==SIS_LEGACY && !terminated);
    /* Lost replies beyond 0.1 % refuse admission instead of pretending qualification. */
    prepare(); dropEvery=500; steadyInputStart(); ack(); siSeal(siCalibrate());
    CHECK(siState==SIS_LEGACY && siReason==200u+STEADY_CAL_ENVELOPE && !kindSent[SI_SEALED] && !kindSent[SI_REPORT]);
    siWatch(); CHECK(kindSent[SI_REJECT]==1 && !terminated);
    /* A changed offer, a non-neutral keyboard, or a held client key declines precommit. */
    calibrate(); steady_input_frame(p,SI_OFFER,siGeneration); steady_input_agreement(p,&hostFit); p[40]^=1; steadyInputReceive(p,64);
    CHECK(siState==SIS_LEGACY && !kindSent[SI_COMMIT] && !terminated);
    calibrate(); fakeNeutral=false; offer(); CHECK(siState==SIS_LEGACY && !kindSent[SI_COMMIT]);
    /* A key during calibration cancels it. */
    prepare(); steadyInputStart(); ack(); CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==17 && siCalibrate()==STEADY_PROBE_STOP_CANCEL);
    /* Missing READY and a postcommit REJECT are terminal, never legacy. */
    calibrate(); offer(); siWatch(); CHECK(siState==SIS_TERMINAL && terminated==1 && !legacy);
    calibrate(); offer(); ready(); memcpy(p,siOffer,64); p[5]=SI_REJECT; memset(p+16,0,48); steadyInputReceive(p,64);
    CHECK(siState==SIS_TERMINAL && terminated==1);
    admit(); sendFails=true; CHECK(orig(4,3)==-1 && terminated==1 && siState==SIS_TERMINAL && !legacy);
    CHECK(orig(4,4)==-1 && !legacy);
    admit(); CHECK(orig(300,3)==-1);
    calibrate(); offer(); for (i=0;i<64;++i) { fakeTime+=10; CHECK(!orig(4,3)); }
    fakeTime+=10; CHECK(orig(4,3)==-1 && terminated==1);
    /* Capture registration is process lifetime: ControlInit (init) and stop never reset it. */
    LiSteadyInputSetCaptureAvailable(0); prepare(); CHECK(siEnabled && !siCaptureIsAvailable());
    LiSteadyInputSetCaptureAvailable(1); prepare(); CHECK(siEnabled && siCaptureIsAvailable());
    prepare(); CHECK(siCaptureIsAvailable());
    /* Without registered capture the offer is declined precommit: exact legacy, no COMMIT, no termination. */
    calibrate(); LiSteadyInputSetCaptureAvailable(0); offer();
    CHECK(siState==SIS_LEGACY && siReason==408 && !kindSent[SI_COMMIT] && !terminated);
    CHECK(LiSendKeyboardEventTimedAt(4,(short)-32703,3,0,0,0)==17 && legacy==1 && siState==SIS_LEGACY);
    siWatch(); CHECK(kindSent[SI_REJECT]==1 && !terminated);
    /* Precommit, zero capture without registration also keeps the exact legacy path. */
    prepare(); steadyInputStart(); CHECK(LiSendKeyboardEventTimedAt(4,(short)-32703,3,0,0,0)==17 && legacy==1 && siState==SIS_LEGACY && !terminated);
    LiSteadyInputSetCaptureAvailable(1);
    /* Withdrawal after commitment fails originals closed, buffered before READY or active; never legacy. */
    calibrate(); offer(); LiSteadyInputSetCaptureAvailable(0); fakeTime+=100;
    CHECK(orig(4,3)==-1 && !siPendingCount && siState==SIS_TERMINAL && terminated==1 && !legacy && siAbandoned==1);
    LiSteadyInputSetCaptureAvailable(1);
    admit(); fakeTime+=100; CHECK(!orig(4,3) && siHeld[4]);
    LiSteadyInputSetCaptureAvailable(0);
    /* The synthetic release of a held key may still use callback time and is classified synthetic. */
    fakeTime+=100; CHECK(!LiSendKeyboardEventTimed(0,(short)-32703,4,0,0) && lastFrame[57]==4 && !lastFrame[58] && !siHeld[4]);
    CHECK(steady_input_slot(lastFrame,1)==fakeTime && siTrace[siTraceUsed-1][8]==1 && !siTrace[siTraceUsed-1][7]);
    fakeTime+=100; CHECK(orig(5,3)==-1 && siState==SIS_TERMINAL && terminated==1 && !legacy);
    CHECK(siTrace[siTraceUsed-1][2]==5 && !siTrace[siTraceUsed-1][4] && !siTrace[siTraceUsed-1][8]);
    LiSteadyInputSetCaptureAvailable(1);
    /* Originals with zero capture are refused after commitment (down, up, old wrapper): no callback time. */
    admit(); fakeTime+=100; CHECK(LiSendKeyboardEventTimedAt(4,(short)-32703,3,0,0,0)==-1 && siState==SIS_TERMINAL && terminated==1 && !legacy);
    CHECK(!kindSent[SI_KEY] && !siTrace[siTraceUsed-1][1] && !siTrace[siTraceUsed-1][4] && !siTrace[siTraceUsed-1][7] && !siTrace[siTraceUsed-1][8]);
    admit(); fakeTime+=100; CHECK(!orig(4,3)); fakeTime+=100; CHECK(LiSendKeyboardEventTimedAt(4,(short)-32703,4,0,0,0)==-1 && terminated==1 && kindSent[SI_KEY]==1);
    admit(); fakeTime+=100; CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==-1 && terminated==1 && !legacy);
    /* Strict frame syntax. */
    steady_input_frame(p,SI_KEY,7); steady_input_set(p,0,1); steady_input_set(p,1,9); steady_input_set(p,2,1); steady_input_set(p,3,1); steady_input_set(p,4,1); p[57]=4; p[58]=1;
    CHECK(steady_input_valid(p,64)); memcpy(copy,p,64);
    for (i=59;i<64;++i) { p[i]=1; CHECK(!steady_input_valid(p,64)); p[i]=0; }
    CHECK(!steady_input_valid(p,63) && !steady_input_valid(NULL,64)); p[58]=2; CHECK(!steady_input_valid(p,64));
    memcpy(p,copy,64); p[57]=0; CHECK(!steady_input_valid(p,64));
    memcpy(p,copy,64); p[3]='1'; CHECK(!steady_input_valid(p,64)); /* STK1 prototype frames are refused. */
    steady_input_frame(p,SI_CAL_REQ,7); steady_input_set(p,0,STEADY_CAL_CAPACITY); steady_input_set(p,1,1); CHECK(!steady_input_valid(p,64));
    steady_input_frame(p,SI_HELLO,7); steady_input_set(p,0,40000000); steady_input_set(p,1,1); steady_input_set(p,2,1); steady_input_set(p,3,1); CHECK(!steady_input_valid(p,64));
    /* Trace capacity: deterministic fill under the fake clock, not a real-time soak or live qualification. */
    CHECK(STEADY_INPUT_TRACE_ORIGINALS==115200 && STEADY_INPUT_CLIENT_TRACE_ROWS>=STEADY_INPUT_TRACE_ORIGINALS);
    CHECK(sizeof(siTrace)/sizeof(siTrace[0])==STEADY_INPUT_CLIENT_TRACE_ROWS);
    admit();
    for (i=0;i<STEADY_INPUT_CLIENT_TRACE_ROWS;++i) { fakeTime+=100; CHECK(!orig(4,(i&1)?4:3)); }
    CHECK(siTraceUsed==STEADY_INPUT_CLIENT_TRACE_ROWS && !siTraceOverflow && siState==SIS_ACTIVE && !terminated && !locks);
    fakeTime+=100; CHECK(orig(4,3)==-1);
    CHECK(siTraceUsed==STEADY_INPUT_CLIENT_TRACE_ROWS && siTraceOverflow==1 && siState==SIS_TERMINAL && terminated==1 && !legacy);
    prepare(); steadyInputStop(); siEnabled=false; CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==17);
    CHECK(!locks);
    return 0;
}
int main(void) {
    if (fitter_tests() || adapter_tests()) return 1;
    puts("PASS: causal fitter soundness/rejections/expiry derivation, v2 activation, calibration inventory cross-fit, precommit fallback, capture registration (survives init, required at COMMIT, withdrawal fails originals), zero-capture originals refused with synthetic releases classified, terminal expiry/identity/capture faults, bounded ingress, exact trace last slot/overflow and strict frame/SDP checks");
    return 0;
}
