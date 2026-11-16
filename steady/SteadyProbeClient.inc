/* Included inside ControlStream.c after sendMessageAndForget. This path sends
 * diagnostic probes only; it never selects timed keyboard ownership. */
#include "SteadyProbe.h"
#include <stdio.h>
#include <inttypes.h>

static PLT_MUTEX steadyProbeMutex;
static PLT_THREAD steadyProbeThread;
static bool steadyProbeEnabled, steadyProbeThreadStarted;
static uint32_t steadyProbePeerVersion;
static uint64_t steadyProbeGeneration;
static uint64_t steadyProbeRows[STEADY_PROBE_SAMPLES][8];
static uint64_t steadyProbeDuplicates, steadyProbeInvalid, steadyProbeUsable;
static bool steadyProbeAcceptReplies;
static enum steady_probe_stop steadyProbeStopReason;
static uint64_t steadyProbeCollectionEnd;
static char steadyProbeTracePath[32768];

void steadyProbeSetPeerVersion(uint32_t version) {
    steadyProbePeerVersion = version;
}

static void steadyProbeInit(void) {
    steadyProbeEnabled = false;
    steadyProbeThreadStarted = false;
    steadyProbeDuplicates = steadyProbeInvalid = steadyProbeUsable = 0;
    steadyProbeAcceptReplies = false;
    steadyProbeStopReason = STEADY_PROBE_STOP_THREAD;
    steadyProbeCollectionEnd = 0;
    memset(steadyProbeRows,0,sizeof(steadyProbeRows));
#if defined(_WIN32)
    DWORD trace_size = GetEnvironmentVariableA("STEADY_PROBE_TRACE",steadyProbeTracePath,sizeof(steadyProbeTracePath));
    if (steady_probe_opt_in() && trace_size && trace_size < sizeof(steadyProbeTracePath) && steadyProbePeerVersion == STEADY_PROBE_VERSION &&
        encryptedControlStream && (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2) &&
        PltCreateMutex(&steadyProbeMutex) == 0) {
        steadyProbeGeneration = 0;
        PltGenerateRandomData((unsigned char*)&steadyProbeGeneration,sizeof(steadyProbeGeneration));
        if (steadyProbeGeneration) steadyProbeEnabled = true;
        else PltDeleteMutex(&steadyProbeMutex);
    }
#endif
}
static void steadyProbeReceive(const unsigned char* payload, size_t size) {
#if defined(_WIN32)
    uint64_t t4, frequency, sequence;
    if (!steadyProbeEnabled || !steady_probe_now(&t4,&frequency)) return;
    if (!steady_probe_valid(payload,size,2) || steady_probe_get64(payload+8) != steadyProbeGeneration) {
        ++steadyProbeInvalid; return;
    }
    sequence = steady_probe_get64(payload+16);
    PltLockMutex(&steadyProbeMutex);
    if (!steadyProbeAcceptReplies) { PltUnlockMutex(&steadyProbeMutex); return; }
    if (!steadyProbeRows[sequence][0] || steadyProbeRows[sequence][0] != steady_probe_get64(payload+24)) ++steadyProbeInvalid;
    else if (steadyProbeRows[sequence][3]) ++steadyProbeDuplicates;
    else {
        steadyProbeRows[sequence][1] = steady_probe_get64(payload+32);
        steadyProbeRows[sequence][2] = steady_probe_get64(payload+40);
        steadyProbeRows[sequence][3] = t4;
        steadyProbeRows[sequence][5] = steady_probe_get64(payload+48);
        /* Count each identity once, retaining model-invalid replies in the trace. */
        if (t4 >= steadyProbeRows[sequence][0] && frequency == steadyProbeRows[sequence][4] &&
                steadyProbeRows[sequence][2] - steadyProbeRows[sequence][1] <= t4 - steadyProbeRows[sequence][0])
            ++steadyProbeUsable;
    }
    PltUnlockMutex(&steadyProbeMutex);
#else
    (void)payload; (void)size;
#endif
}
static void steadyProbeSendThread(void* context) {
    (void)context;
#if defined(_WIN32)
    uint64_t sequence, t1, frequency, started = 0, usable;
    unsigned char request[STEADY_PROBE_BYTES];
    enum steady_probe_stop reason = STEADY_PROBE_STOP_CANCEL;
    for (sequence = 0; sequence < STEADY_PROBE_SAMPLES && !PltIsThreadInterrupted(&steadyProbeThread); ++sequence) {
        bool sent;
        if (!steady_probe_now(&t1,&frequency) || !steady_probe_request(request,steadyProbeGeneration,sequence,t1)) {
            reason = STEADY_PROBE_STOP_CLOCK; break;
        }
        if (!started) started = t1;
        if (t1 < started) { reason = STEADY_PROBE_STOP_CLOCK; break; }
        PltLockMutex(&steadyProbeMutex);
        usable = steadyProbeUsable;
        PltUnlockMutex(&steadyProbeMutex);
        reason = steady_probe_collection_stop(sequence,usable,t1-started);
        if (reason != STEADY_PROBE_RUNNING) break;
        PltLockMutex(&steadyProbeMutex);
        steadyProbeRows[sequence][0] = t1; steadyProbeRows[sequence][4] = frequency;
        PltUnlockMutex(&steadyProbeMutex);
        sent = sendMessageAndForget(STEADY_PROBE_TYPE,sizeof(request),request,CTRL_CHANNEL_KEYBOARD,ENET_PACKET_FLAG_UNSEQUENCED,false);
        PltLockMutex(&steadyProbeMutex);
        steadyProbeRows[sequence][6] = sent ? 1 : 0;
        steadyProbeRows[sequence][7] = 1;
        PltUnlockMutex(&steadyProbeMutex);
        if (!sent) { reason = STEADY_PROBE_STOP_SEND; break; }
        reason = STEADY_PROBE_STOP_CANCEL;
        PltSleepMsInterruptible(&steadyProbeThread,4);
    }
    if (sequence == STEADY_PROBE_SAMPLES) reason = STEADY_PROBE_STOP_CAPACITY;
    /* One fixed grace window, then seal under the receive lock. Export timing
       cannot convert an unanswered attempt into a reply after this boundary. */
    PltSleepMsInterruptible(&steadyProbeThread,STEADY_PROBE_DRAIN_MS);
    if (PltIsThreadInterrupted(&steadyProbeThread) &&
            (reason == STEADY_PROBE_STOP_TARGET || reason == STEADY_PROBE_STOP_CAPACITY || reason == STEADY_PROBE_STOP_TIMEOUT))
        reason = STEADY_PROBE_STOP_CANCEL;
    PltLockMutex(&steadyProbeMutex);
    steadyProbeAcceptReplies = false;
    steadyProbeStopReason = reason;
    if (!steady_probe_now(&steadyProbeCollectionEnd,&frequency))
        steadyProbeStopReason = STEADY_PROBE_STOP_CLOCK;
    PltUnlockMutex(&steadyProbeMutex);
#endif
}
static void steadyProbeStart(void) {
    if (steadyProbeEnabled) {
        PltLockMutex(&steadyProbeMutex);
        steadyProbeAcceptReplies = true;
        PltUnlockMutex(&steadyProbeMutex);
        if (PltCreateThread("SteadyProbe",steadyProbeSendThread,NULL,&steadyProbeThread) == 0)
            steadyProbeThreadStarted = true;
        else {
            PltLockMutex(&steadyProbeMutex);
            steadyProbeAcceptReplies = false;
            PltUnlockMutex(&steadyProbeMutex);
        }
    }
}
static void steadyProbeStop(void) {
    if (steadyProbeThreadStarted) {
        PltInterruptThread(&steadyProbeThread); PltJoinThread(&steadyProbeThread);
        steadyProbeThreadStarted = false;
    }
}
static void steadyProbeDestroy(void) {
    FILE* file;
    unsigned i;
    if (!steadyProbeEnabled) return;
    /* Called only after sender and control receive threads are joined. */
#if defined(_MSC_VER)
    file = NULL;
    fopen_s(&file,steadyProbeTracePath,"wx");
#else
    file = fopen(steadyProbeTracePath,"wx");
#endif
    if (file) {
        fprintf(file,"sequence,t1_ns,t2_ns,t3_ns,t4_ns,client_frequency,host_frequency,send_ok,attempted,generation,probe_version,collection_stop,collection_end_ns,collected_usable\n");
        for (i = 0; i < STEADY_PROBE_SAMPLES; ++i) {
            const uint64_t* r = steadyProbeRows[i];
            fprintf(file,"%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%u,%" PRIu64 ",%" PRIu64 "\n",
                    i,r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7],steadyProbeGeneration,
                    (unsigned)STEADY_PROBE_VERSION,(unsigned)steadyProbeStopReason,steadyProbeCollectionEnd,steadyProbeUsable);
        }
        int io_error = ferror(file);
        if (fclose(file)) io_error = 1;
        if (io_error) Limelog("STEADY probe trace write failed\n");
        else Limelog("STEADY probe trace saved; duplicate replies=%" PRIu64 ", invalid replies=%" PRIu64 "\n",steadyProbeDuplicates,steadyProbeInvalid);
    } else Limelog("STEADY probe trace not written: use a new writable output filename\n");
    PltDeleteMutex(&steadyProbeMutex);
    steadyProbeEnabled = false;
}
