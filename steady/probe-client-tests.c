/* Execute the actual client collector with a deterministic clock and transport.
 * This exercises stop/accounting/seal behavior, not OS timing or concurrency. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "SteadyProbe.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
#define CTRL_CHANNEL_KEYBOARD 2
#define ENET_PACKET_FLAG_UNSEQUENCED 2
#define SS_ENC_CONTROL_V2 1
typedef int PLT_MUTEX;
typedef struct { bool interrupted; } PLT_THREAD;
static bool encryptedControlStream = true;
static int EncryptionFeaturesEnabled = SS_ENC_CONTROL_V2;
static uint64_t fakeClock, clockStep, sendCount, failAt, cancelAt;
static unsigned lossModulo;
static bool negativeBracket, duplicateReply, threadFails, clockFails;
static unsigned locks;

static int PltCreateMutex(PLT_MUTEX* mutex) { *mutex = 1; return 0; }
static void PltDeleteMutex(PLT_MUTEX* mutex) { *mutex = 0; }
static void PltLockMutex(PLT_MUTEX* mutex) { (void)mutex; ++locks; }
static void PltUnlockMutex(PLT_MUTEX* mutex) { (void)mutex; --locks; }
static void PltGenerateRandomData(unsigned char* data, size_t size) { memset(data,7,size); }
static bool PltIsThreadInterrupted(PLT_THREAD* thread) { return thread->interrupted; }
static void PltSleepMsInterruptible(PLT_THREAD* thread, unsigned ms) {
    fakeClock += ms == STEADY_PROBE_DRAIN_MS ? (uint64_t)ms*1000000 : clockStep;
    if (sendCount >= cancelAt) thread->interrupted = true;
}
static int PltCreateThread(const char* name, void (*entry)(void*), void* context, PLT_THREAD* thread) {
    (void)name; (void)entry; (void)context; (void)thread; return threadFails ? -1 : 0;
}
static void PltInterruptThread(PLT_THREAD* thread) { thread->interrupted = true; }
static void PltJoinThread(PLT_THREAD* thread) { (void)thread; }
static void Limelog(const char* format, ...) { (void)format; }
static bool fake_now(uint64_t* ns, uint64_t* frequency) {
    if (clockFails) return false;
    *ns = fakeClock; *frequency = 10000000; return true;
}
static bool sendMessageAndForget(unsigned short type, unsigned short size, const void* payload,
    unsigned char channel, unsigned flags, bool reliable);
#define steady_probe_now fake_now
#include "SteadyProbeClient.inc"
#undef steady_probe_now

static bool sendMessageAndForget(unsigned short type, unsigned short size, const void* payload,
    unsigned char channel, unsigned flags, bool reliable) {
    unsigned char reply[STEADY_PROBE_BYTES];
    uint64_t sequence = steady_probe_get64((const unsigned char*)payload+16);
    uint64_t t1 = steady_probe_get64((const unsigned char*)payload+24);
    (void)type; (void)size; (void)channel; (void)flags; (void)reliable;
    ++sendCount;
    if (sendCount == failAt) return false;
    if (lossModulo && sequence % lossModulo == 0) return true;
    if (!steady_probe_reply(reply,payload,STEADY_PROBE_BYTES,1,t1+1000000,
            t1+(negativeBracket ? 10000000 : 1000100),10000000)) return false;
    fakeClock += 2000000;
    steadyProbeReceive(reply,sizeof(reply));
    if (duplicateReply) steadyProbeReceive(reply,sizeof(reply));
    return true;
}

static void reset(void) {
    memset(steadyProbeRows,0,sizeof(steadyProbeRows));
    memset(&steadyProbeThread,0,sizeof(steadyProbeThread));
    steadyProbeEnabled = steadyProbeAcceptReplies = true;
    steadyProbeThreadStarted = false;
    steadyProbeGeneration = 7;
    steadyProbeUsable = steadyProbeInvalid = steadyProbeDuplicates = 0;
    steadyProbeCollectionEnd = 0;
    steadyProbeStopReason = STEADY_PROBE_STOP_THREAD;
    fakeClock = UINT64_C(1000000000); clockStep = 4000000;
    sendCount = 0; failAt = cancelAt = UINT64_MAX;
    lossModulo = 0; negativeBracket = duplicateReply = threadFails = clockFails = false;
    locks = 0;
}

int main(void) {
    unsigned char request[64], reply[64];
    reset(); lossModulo = 100; duplicateReply = true;
    steadyProbeSendThread(NULL);
    CHECK(steadyProbeStopReason == STEADY_PROBE_STOP_TARGET && steadyProbeUsable == 10000);
    CHECK(sendCount > 10000 && sendCount < STEADY_PROBE_SAMPLES);
    CHECK(steadyProbeDuplicates == 10000 && !steadyProbeAcceptReplies && locks == 0);
    CHECK(steadyProbeRows[sendCount][0] == 0 && steadyProbeRows[sendCount][7] == 0);
    CHECK(steady_probe_request(request,7,0,steadyProbeRows[0][0]));
    CHECK(steady_probe_reply(reply,request,64,1,fakeClock+1,fakeClock+2,10000000));
    steadyProbeReceive(reply,64);
    CHECK(steadyProbeUsable == 10000 && steadyProbeRows[0][3] == 0);
    reset(); lossModulo = 1;
    steadyProbeSendThread(NULL);
    CHECK(steadyProbeStopReason == STEADY_PROBE_STOP_CAPACITY && sendCount == STEADY_PROBE_SAMPLES);
    CHECK(steadyProbeUsable == 0 && steadyProbeRows[STEADY_PROBE_SAMPLES-1][7] == 1);
    reset(); negativeBracket = duplicateReply = true;
    steadyProbeSendThread(NULL);
    CHECK(steadyProbeStopReason == STEADY_PROBE_STOP_CAPACITY && steadyProbeUsable == 0);
    CHECK(steadyProbeRows[0][3] != 0 && steadyProbeDuplicates == STEADY_PROBE_SAMPLES);
    reset(); cancelAt = 10;
    steadyProbeSendThread(NULL);
    CHECK(steadyProbeStopReason == STEADY_PROBE_STOP_CANCEL && sendCount == 10 && !steadyProbeAcceptReplies);
    reset(); failAt = 100;
    steadyProbeSendThread(NULL);
    CHECK(steadyProbeStopReason == STEADY_PROBE_STOP_SEND && sendCount == 100);
    CHECK(steadyProbeRows[99][7] == 1 && steadyProbeRows[99][6] == 0 && steadyProbeRows[100][7] == 0);
    reset(); clockStep = UINT64_C(20000000000);
    steadyProbeSendThread(NULL);
    CHECK(steadyProbeStopReason == STEADY_PROBE_STOP_TIMEOUT && sendCount < 10000);
    reset(); clockFails = true;
    steadyProbeSendThread(NULL);
    CHECK(steadyProbeStopReason == STEADY_PROBE_STOP_CLOCK && sendCount == 0);
    reset(); threadFails = true;
    steadyProbeStart();
    CHECK(steadyProbeStopReason == STEADY_PROBE_STOP_THREAD && !steadyProbeAcceptReplies && !steadyProbeThreadStarted);
    /* Compile/lifecycle references for the remaining included entry points. */
    steadyProbeSetPeerVersion(STEADY_PROBE_VERSION);
    steadyProbeStop();
    steadyProbeEnabled = false;
    steadyProbeDestroy();
    steadyProbeInit();
    CHECK(locks == 0);
    puts("PASS: actual v2 client collector loss, duplicates, all-loss bound, invalid-first reply, cancellation, send/clock/thread failure and sealed late replies");
    return 0;
}
