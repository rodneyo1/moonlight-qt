#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "SteadyProbe.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

int main(void) {
    unsigned char request[64], reply[64], unchanged[64], bad[64];
    steady_probe_session session = {0};
    uint64_t now, frequency;
    unsigned i;
    CHECK(steady_probe_request(request,UINT64_MAX,0,123));
    CHECK(steady_probe_valid(request,64,1));
    CHECK(request[0] == 1 && request[1] == 1 && request[3] == 64 && request[31] == 123);
    for (i = 8; i < 16; ++i) CHECK(request[i] == 255);
    CHECK(steady_probe_reply(reply,request,64,1,456,789,10000000));
    CHECK(steady_probe_valid(reply,64,2) && steady_probe_get64(reply+32) == 456 && steady_probe_get64(reply+40) == 789);
    memset(unchanged,0xA5,64);
    for (i = 0; i < 64; ++i) {
        memcpy(reply,unchanged,64);
        CHECK(!steady_probe_reply(reply,request,i,1,456,789,10000000));
        CHECK(memcmp(reply,unchanged,64) == 0);
    }
    memcpy(reply,unchanged,64);
    CHECK(!steady_probe_reply(reply,request,64,0,456,789,10000000));
    CHECK(!steady_probe_reply(reply,request,64,1,0,789,10000000));
    CHECK(!steady_probe_reply(reply,request,64,1,456,455,10000000));
    CHECK(!steady_probe_reply(reply,request,64,1,456,789,0));
    CHECK(memcmp(reply,unchanged,64) == 0);
    for (i = 0; i < 64; ++i) {
        memcpy(bad,request,64); bad[i] ^= 0x80;
        /* Mutating generation, sequence or t1 can be valid syntax; every
           canonical accepted request must preserve its bytes in the reply. */
        if (steady_probe_valid(bad,64,1)) {
            CHECK(steady_probe_reply(reply,bad,64,1,456,789,10000000));
            CHECK(memcmp(reply+8,bad+8,24) == 0);
        } else CHECK(!steady_probe_reply(reply,bad,64,1,456,789,10000000));
    }
    for (i = 0; i < STEADY_PROBE_SAMPLES; ++i) {
        CHECK(steady_probe_request(request,7,i,123));
        CHECK(steady_probe_reply_once(&session,reply,request,64,1,456,789,10000000));
        CHECK(!steady_probe_reply_once(&session,reply,request,64,1,456,789,10000000));
    }
    CHECK(!steady_probe_request(request,7,STEADY_PROBE_SAMPLES,123));
    CHECK(steady_probe_request(request,8,0,123));
    CHECK(!steady_probe_reply_once(&session,reply,request,64,1,456,789,10000000));
    CHECK(session.generation == 7);
    CHECK(!steady_probe_valid(NULL,64,1));
    CHECK(!steady_probe_request(NULL,7,0,123));
    CHECK(!steady_probe_request(reply,0,0,123));
    CHECK(!steady_probe_request(reply,7,0,0));
    CHECK(!steady_probe_reply_once(NULL,reply,request,64,1,456,789,10000000));
    CHECK(!steady_probe_sdp_version(NULL));
    CHECK(!steady_probe_sdp_version(""));
    CHECK(steady_probe_sdp_version("v=0\r\na=x-steady-input.probeVersion:1\r\n"));
    CHECK(steady_probe_sdp_version("a=x-steady-input.probeVersion:1"));
    CHECK(!steady_probe_sdp_version("a=x-steady-input.probeVersion:11\r\n"));
    CHECK(!steady_probe_sdp_version("a=x-steady-input.probeVersion:1junk\n"));
    CHECK(!steady_probe_sdp_version("a=x-steady-input.probeVersion:1\na=x-steady-input.probeVersion:1\n"));
    CHECK(!steady_probe_sdp_version("x-a=x-steady-input.probeVersion:1\n"));
    CHECK(!steady_probe_sdp_version("a=x-steady-input.probeVersion:2\n"));
    CHECK(!steady_probe_sdp_version("a=x-steady-input.probeVersion:\n"));
    CHECK(steady_probe_now(&now,&frequency) && now && frequency);
    CHECK(!steady_probe_now(NULL,&frequency) && !steady_probe_now(&now,NULL));
    printf("PASS: C calibration codec, exact SDP, authentication seam, 10000 unique probes/replays, generation and QPC guards\n");
    return 0;
}
