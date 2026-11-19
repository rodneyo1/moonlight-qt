/* Experimental fixed-buffer input, included after the authenticated control sender.
 * Timestamp point is this API callback, not physical actuation or SDL creation.
 * Reliable delivery is deliberate for the first connected prototype. */
#include "SteadyInput.h"
#if defined(_WIN32)
static PLT_MUTEX siMutex;
static PLT_THREAD siThread;
static bool siEnabled,siThreadStarted;
static uint32_t siPeerVersion;
static unsigned siState; /* 0 legacy, 1 HELLO, 2 committed, 3 ready, 4 terminal */
static unsigned char siOffer[STEADY_INPUT_BYTES],siPending[64][STEADY_INPUT_BYTES];
static unsigned siPendingCount;
static uint64_t siGeneration,siSequence,siStart,siFrequency,siLast;
static unsigned short siCodes[256];
static unsigned char siFlags[256],siModifiers[256],siHeld[256];
static uint64_t siTrace[32768][6];
static unsigned siTraceUsed,siTraceOverflow;
static char siTracePath[32768];
static bool steadyInputSend(const unsigned char* frame) {
    return sendMessageAndForget(STEADY_INPUT_TYPE,STEADY_INPUT_BYTES,frame,CTRL_CHANNEL_KEYBOARD,ENET_PACKET_FLAG_RELIABLE,false);
}
static void steadyInputInit(void) {
    DWORD n;
    siEnabled=false; siThreadStarted=false; siState=0; siPendingCount=0; siSequence=0; siTraceUsed=siTraceOverflow=0;
    memset(siHeld,0,sizeof(siHeld));
    n=GetEnvironmentVariableA("STEADY_INPUT_TRACE",siTracePath,sizeof(siTracePath));
    if (!steady_input_opt_in() || siPeerVersion!=1 || !n || n>=sizeof(siTracePath) ||
        !encryptedControlStream || !(EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2) || PltCreateMutex(&siMutex)) return;
    siGeneration=0; PltGenerateRandomData((unsigned char*)&siGeneration,sizeof(siGeneration));
    if (!siGeneration || !steady_probe_now(&siStart,&siFrequency)) { PltDeleteMutex(&siMutex); return; }
    siLast=siStart; siEnabled=true;
}
static void steadyInputWatch(void* context) {
    bool expired=false;
    (void)context;
    while (!PltIsThreadInterrupted(&siThread)) {
        uint64_t now,freq;
        PltSleepMsInterruptible(&siThread,100);
        PltLockMutex(&siMutex);
        if (siState>=1 && siState<=3 && (!steady_probe_now(&now,&freq) || freq!=siFrequency || now<siLast ||
            now<siStart || now-siStart>=STEADY_INPUT_LIFETIME || (siState<=2 && now-siStart>=UINT64_C(3000000000)))) {
            expired=siState>=2; siState=expired?4:0;
        }
        PltUnlockMutex(&siMutex);
        if (expired) { Limelog("STEADY input prototype expired; reconnect for a fresh fixed mapping\n"); ListenerCallbacks.connectionTerminated(-1); return; }
    }
}
static void steadyInputStart(void) {
    if (!siEnabled) return;
    steady_input_frame(siOffer,SI_HELLO,siGeneration); steady_probe_put64(siOffer+24,siStart);
    siState=1;
    if (PltCreateThread("SteadyInput",steadyInputWatch,NULL,&siThread)) { siState=0; return; }
    siThreadStarted=true;
    if (!steadyInputSend(siOffer)) { PltLockMutex(&siMutex); siState=0; PltUnlockMutex(&siMutex); }
}
static void steadyInputReceive(const unsigned char* frame,size_t size) {
    bool failed=false;
    if (!siEnabled || !steady_input_valid(frame,size) || steady_probe_get64(frame+8)!=siGeneration) return;
    PltLockMutex(&siMutex);
    if (frame[5]==SI_REJECT && siState==1) siState=0;
    else if (frame[5]==SI_REJECT && siState>=2 && siState<=3) { siState=4; failed=true; }
    else if (frame[5]==SI_OFFER && siState==1 && steady_probe_get64(frame+24)==siStart && steady_probe_get64(frame+32)) {
        memcpy(siOffer,frame,STEADY_INPUT_BYTES); siOffer[5]=SI_COMMIT;
        siState=2; /* From here onward, never replay through the legacy path. */
        if (!steadyInputSend(siOffer)) { siState=4; failed=true; }
    } else if (frame[5]==SI_READY && siState==2 && steady_input_same_offer(frame,siOffer)) {
        unsigned i; siState=3;
        for (i=0;i<siPendingCount;++i) if (!steadyInputSend(siPending[i])) { failed=true; siState=4; break; }
        siPendingCount=0;
        Limelog("STEADY experimental keyboard active: frozen unit-rate mapping, 40 ms reserve, 60 s maximum; callback timestamps\n");
    }
    PltUnlockMutex(&siMutex);
    if (failed) ListenerCallbacks.connectionTerminated(-1);
}
/* Caller supplies USB usage, or zero for a synthetic release of an already held key. */
int LiSendKeyboardEventTimed(unsigned short usage,short keyCode,char action,char modifiers,char flags) {
    uint64_t now,freq; unsigned char frame[STEADY_INPUT_BYTES]; bool failed=false,sent=true; unsigned i;
    if (!siEnabled) return LiSendKeyboardEvent2(keyCode,action,modifiers,flags);
    PltLockMutex(&siMutex);
    if (siState<=1) {
        siState=0; /* A key before commitment keeps this entire session legacy. */
        PltUnlockMutex(&siMutex); return LiSendKeyboardEvent2(keyCode,action,modifiers,flags);
    }
    if (siState==4) { PltUnlockMutex(&siMutex); return -1; }
    if (!usage && action==KEY_ACTION_UP) {
        for (i=4;i<256;++i) if (siHeld[i] && siCodes[i]==(unsigned short)keyCode && siFlags[i]==(unsigned char)flags &&
            (siModifiers[i]&MODIFIER_EXTENDED)==(modifiers&MODIFIER_EXTENDED)) { usage=(unsigned short)i; break; }
        if (!usage) { PltUnlockMutex(&siMutex); return 0; }
    }
    if (usage<4 || usage>=256 || siSequence==UINT64_MAX || !steady_probe_now(&now,&freq) || freq!=siFrequency || now<siLast || now<siStart || now-siStart>=STEADY_INPUT_LIFETIME) failed=true;
    else {
        siLast=now; steady_input_frame(frame,SI_KEY,siGeneration);
        steady_probe_put64(frame+16,++siSequence); steady_probe_put64(frame+24,now);
        memcpy(frame+32,siOffer+32,24); frame[57]=(unsigned char)usage; frame[58]=action==KEY_ACTION_DOWN;
        siHeld[usage]=frame[58]; siCodes[usage]=(unsigned short)keyCode; siFlags[usage]=(unsigned char)flags; siModifiers[usage]=(unsigned char)modifiers;
        if (siState==2) {
            if (siPendingCount==64) failed=true;
            else memcpy(siPending[siPendingCount++],frame,sizeof(frame));
        } else if (!(sent=steadyInputSend(frame))) failed=true;
        if (siTraceUsed<32768) {
            uint64_t* row=siTrace[siTraceUsed++]; row[0]=siSequence; row[1]=now; row[2]=usage; row[3]=frame[58]; row[4]=sent&&!failed; row[5]=siState;
        } else { ++siTraceOverflow; failed=true; }
    }
    if (failed) siState=4;
    PltUnlockMutex(&siMutex);
    if (failed) ListenerCallbacks.connectionTerminated(-1);
    return failed?-1:0;
}
static void steadyInputStop(void) {
    if (!siEnabled) return;
    PltLockMutex(&siMutex); siState=4; PltUnlockMutex(&siMutex);
    if (siThreadStarted) { PltInterruptThread(&siThread); PltJoinThread(&siThread); siThreadStarted=false; }
}
static void steadyInputDestroy(void) {
    FILE* file=NULL; unsigned i;
    if (!siEnabled) return;
#if defined(_MSC_VER)
    fopen_s(&file,siTracePath,"wx");
#else
    file=fopen(siTracePath,"wx");
#endif
    if (file) {
        fprintf(file,"sequence,capture_ns,usage,down,queued_ok,state,generation,overflow\n");
        for (i=0;i<siTraceUsed;++i) fprintf(file,"%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u\n",siTrace[i][0],siTrace[i][1],siTrace[i][2],siTrace[i][3],siTrace[i][4],siTrace[i][5],siGeneration,siTraceOverflow);
        if (ferror(file)) Limelog("STEADY input trace write failed\n");
        if (fclose(file)) Limelog("STEADY input trace close failed\n");
    } else Limelog("STEADY input trace not written: choose a new writable filename\n");
    siEnabled=false; PltDeleteMutex(&siMutex);
}
#else
static uint32_t siPeerVersion;
static void steadyInputInit(void) {}
static void steadyInputStart(void) {}
static void steadyInputStop(void) {}
static void steadyInputDestroy(void) {}
static void steadyInputReceive(const unsigned char* p,size_t n) { (void)p; (void)n; }
int LiSendKeyboardEventTimed(unsigned short usage,short code,char action,char mods,char flags) { (void)usage; return LiSendKeyboardEvent2(code,action,mods,flags); }
#endif
void steadyInputSetPeerVersion(uint32_t version) { siPeerVersion=version; }
