/* Real client adapter under deterministic control transport and clock. */
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
#define SS_ENC_CONTROL_V2 1
#define KEY_ACTION_DOWN 3
#define KEY_ACTION_UP 4
#define MODIFIER_EXTENDED 0x10
static bool encryptedControlStream=true;
static int EncryptionFeaturesEnabled=1;
typedef int PLT_MUTEX;
typedef struct { bool interrupted; } PLT_THREAD;
static uint64_t fakeTime=1000000000,fakeFrequency=10000000;
static unsigned totalSent,legacy,terminated,locks;
static bool sendFails,threadFails,clockFails;
static unsigned char lastFrame[64];
static int PltCreateMutex(PLT_MUTEX* m) { *m=1; return 0; }
static void PltDeleteMutex(PLT_MUTEX* m) { *m=0; }
static void PltLockMutex(PLT_MUTEX* m) { (void)m; ++locks; }
static void PltUnlockMutex(PLT_MUTEX* m) { (void)m; --locks; }
static void PltGenerateRandomData(unsigned char* p,size_t n) { memset(p,7,n); }
static bool PltIsThreadInterrupted(PLT_THREAD* t) { return t->interrupted; }
static int PltCreateThread(const char* n,void (*f)(void*),void* c,PLT_THREAD* t) { (void)n;(void)f;(void)c;t->interrupted=false;return threadFails?-1:0; }
static void PltSleepMsInterruptible(PLT_THREAD* t,unsigned ms) { (void)t;(void)ms;fakeTime+=STEADY_INPUT_LIFETIME; }
static void PltInterruptThread(PLT_THREAD* t) { t->interrupted=true; }
static void PltJoinThread(PLT_THREAD* t) { (void)t; }
static bool fake_now(uint64_t* now,uint64_t* frequency) { if (clockFails) return false;*now=fakeTime;*frequency=fakeFrequency;return true; }
static void Limelog(const char* f,...) { (void)f; }
static void terminated_cb(int e) { (void)e; ++terminated; }
static struct { void (*connectionTerminated)(int); } ListenerCallbacks={terminated_cb};
static bool sendMessageAndForget(short type,short size,const void* p,uint8_t channel,uint32_t flags,bool more) {
    if (type!=STEADY_INPUT_TYPE || size!=64 || channel!=2 || flags!=1 || more) return false;
    ++totalSent;memcpy(lastFrame,p,64);return !sendFails;
}
int LiSendKeyboardEvent2(short code,char action,char modifiers,char flags) { (void)code;(void)action;(void)modifiers;(void)flags;++legacy;return 17; }
#define steady_probe_now fake_now
#include "SteadyInputClient.inc"
#undef steady_probe_now
static void prepare(void) {
    if (siEnabled) { steadyInputStop(); siEnabled=false; PltDeleteMutex(&siMutex); }
    totalSent=legacy=terminated=locks=0;sendFails=threadFails=clockFails=false;fakeTime=1000000000;fakeFrequency=10000000;
    SetEnvironmentVariableA("STEADY_INPUT_ENABLE","1");SetEnvironmentVariableA("STEADY_INPUT_TRACE","unused-client-test.csv");
    steadyInputSetPeerVersion(1);steadyInputInit();
}
static void reset(void) { prepare();steadyInputStart(); }
static void offer(void) {
    unsigned char p[64];memcpy(p,lastFrame,64);p[5]=SI_OFFER;steady_probe_put64(p+32,9000000000);
    steadyInputReceive(p,64);
}
static void ready(void) { unsigned char p[64];memcpy(p,siOffer,64);p[5]=SI_READY;steadyInputReceive(p,64); }
int main(void) {
    unsigned char p[64],copy[64];unsigned i;
    CHECK(steady_input_sdp_version("a=x-steady-input.keyboardVersion:1\r\n")==1);
    CHECK(!steady_input_sdp_version("a=x-steady-input.keyboardVersion:1\na=x-steady-input.keyboardVersion:1\n"));
    CHECK(!steady_input_sdp_version("a=x-steady-input.keyboardVersion:10\n"));
    prepare();fakeTime+=600000000;steadyInputStart();
    CHECK(siState==1 && totalSent==1 && steady_probe_get64(lastFrame+24)==fakeTime && siStart==fakeTime && siLast==fakeTime);
    offer();ready();fakeTime+=STEADY_INPUT_LIFETIME-1;CHECK(!LiSendKeyboardEventTimed(4,(short)-32703,3,0,0));
    ++fakeTime;CHECK(LiSendKeyboardEventTimed(4,(short)-32703,4,0,0)==-1 && terminated==1);
    prepare();clockFails=true;steadyInputStart();CHECK(siState==0 && !totalSent && !siThreadStarted);
    prepare();fakeFrequency++;steadyInputStart();CHECK(siState==0 && !totalSent && !siThreadStarted);
    prepare();fakeTime--;steadyInputStart();CHECK(siState==0 && !totalSent && !siThreadStarted);
    reset();CHECK(siEnabled && siState==1 && totalSent==1 && steady_input_valid(lastFrame,64));
    CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==17);offer();CHECK(siState==0 && legacy==1 && totalSent==1);
    reset();offer();CHECK(siState==2 && totalSent==2 && lastFrame[5]==SI_COMMIT);
    fakeTime+=100;CHECK(!LiSendKeyboardEventTimed(4,(short)-32703,3,0,0));CHECK(totalSent==2 && siPendingCount==1);
    ready();CHECK(siState==3 && totalSent==3 && lastFrame[5]==SI_KEY && steady_probe_get64(lastFrame+24)==fakeTime);
    fakeTime+=100;CHECK(!LiSendKeyboardEventTimed(0,(short)-32703,4,0,0));CHECK(lastFrame[57]==4 && !lastFrame[58] && legacy==0);
    CHECK(steady_probe_get64(lastFrame+16)==2 && !locks);
    reset();offer();ready();sendFails=true;CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==-1);CHECK(terminated==1 && siState==4 && !legacy);
    CHECK(LiSendKeyboardEventTimed(4,(short)-32703,4,0,0)==-1 && !legacy);
    reset();offer();ready();fakeFrequency++;CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==-1 && terminated==1);
    reset();offer();ready();fakeTime--;CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==-1);
    reset();offer();ready();CHECK(LiSendKeyboardEventTimed(300,(short)-32703,3,0,0)==-1);
    reset();offer();for(i=0;i<64;++i) CHECK(!LiSendKeyboardEventTimed(4,(short)-32703,3,0,0));
    CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==-1 && terminated==1);
    reset();offer();ready();steadyInputWatch(NULL);CHECK(siState==4 && terminated==1 && !legacy);
    reset();memcpy(p,lastFrame,64);p[5]=SI_OFFER;steady_probe_put64(p+32,9);p[59]=1;steadyInputReceive(p,64);CHECK(siState==1);
    p[59]=0;steady_probe_put64(p+8,99);steadyInputReceive(p,64);CHECK(siState==1);
    reset();memcpy(p,lastFrame,64);p[5]=SI_REJECT;steadyInputReceive(p,64);CHECK(siState==0 && !terminated);
    reset();offer();memcpy(p,siOffer,64);p[5]=SI_REJECT;steadyInputReceive(p,64);CHECK(siState==4 && terminated==1);
    steady_input_frame(p,SI_KEY,7);steady_probe_put64(p+16,1);p[57]=4;p[58]=1;
    CHECK(steady_input_valid(p,64));memcpy(copy,p,64);
    for(i=59;i<64;++i) {p[i]=1;CHECK(!steady_input_valid(p,64));p[i]=0;}
    CHECK(!steady_input_valid(p,63));CHECK(!steady_input_valid(NULL,64));p[58]=2;CHECK(!steady_input_valid(p,64));
    memcpy(p,copy,64);p[57]=0;CHECK(!steady_input_valid(p,64));
    /* Trace capacity: deterministic fill under the fake clock, not a real-time soak or live qualification. */
    CHECK(STEADY_INPUT_TRACE_ORIGINALS==115200 && STEADY_INPUT_CLIENT_TRACE_ROWS>=STEADY_INPUT_TRACE_ORIGINALS);
    CHECK(sizeof(siTrace)/sizeof(siTrace[0])==STEADY_INPUT_CLIENT_TRACE_ROWS);
    reset();offer();ready();
    for(i=0;i<STEADY_INPUT_CLIENT_TRACE_ROWS;++i) {fakeTime+=100;CHECK(!LiSendKeyboardEventTimed(4,(short)-32703,(i&1)?4:3,0,0));}
    CHECK(siTraceUsed==STEADY_INPUT_CLIENT_TRACE_ROWS && !siTraceOverflow && siState==3 && !terminated && !locks);
    CHECK(siTrace[STEADY_INPUT_CLIENT_TRACE_ROWS-1][0]==STEADY_INPUT_CLIENT_TRACE_ROWS && siTrace[STEADY_INPUT_CLIENT_TRACE_ROWS-1][1]==fakeTime);
    CHECK(siTrace[STEADY_INPUT_CLIENT_TRACE_ROWS-1][2]==4 && !siTrace[STEADY_INPUT_CLIENT_TRACE_ROWS-1][3] && siTrace[STEADY_INPUT_CLIENT_TRACE_ROWS-1][4]==1 && siTrace[STEADY_INPUT_CLIENT_TRACE_ROWS-1][5]==3);
    fakeTime+=100;CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==-1);
    CHECK(siTraceUsed==STEADY_INPUT_CLIENT_TRACE_ROWS && siTraceOverflow==1 && siState==4 && terminated==1 && !legacy);
    fakeTime+=100;CHECK(LiSendKeyboardEventTimed(4,(short)-32703,4,0,0)==-1 && siTraceOverflow==1 && terminated==1 && !legacy);
    reset();steadyInputStop();siEnabled=false;CHECK(LiSendKeyboardEventTimed(4,(short)-32703,3,0,0)==17);
    CHECK(!locks);puts("PASS: client activation, precommit fallback, buffered commit, synthetic releases, terminal faults, bounded ingress, exact trace last slot/overflow and strict frame/SDP checks");return 0;
}
