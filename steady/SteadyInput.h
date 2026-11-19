/**
 * @file SteadyInput.h
 * @brief Private opt-in fixed-buffer keyboard prototype protocol (control-v2 only).
 */
#ifndef STEADY_INPUT_H
#define STEADY_INPUT_H
#include "SteadyProbe.h"
#define STEADY_INPUT_TYPE 0x5355
#define STEADY_INPUT_BYTES 64
#define STEADY_INPUT_VERSION 1
#define STEADY_INPUT_RESERVE UINT64_C(40000000)
#define STEADY_INPUT_LIFETIME UINT64_C(60000000000)

/** @brief Reliable startup and keyboard record kinds. */
enum steady_input_kind {
  SI_HELLO = 1,
  SI_OFFER = 2,
  SI_COMMIT = 3,
  SI_READY = 4,
  SI_KEY = 5,
  SI_REJECT = 6
};

/** @brief Explicit experimental input opt-in. @return True only for exact value 1. */
static inline int steady_input_opt_in(void) {
#if defined(_WIN32)
  char value[2];
  return GetEnvironmentVariableA("STEADY_INPUT_ENABLE", value, sizeof(value)) == 1 && value[0] == '1';
#else
  return 0;
#endif
}

/** @brief Read the private input SDP capability. @param sdp Complete SDP. @return Version or zero. */
static inline unsigned steady_input_sdp_version(const char *sdp) {
  const char prefix[] = "a=x-steady-input.keyboardVersion:";
  unsigned count = 0;
  if (!sdp) {
    return 0;
  }
  while (*sdp) {
    const char *end = strchr(sdp, '\n');
    size_t n = end ? (size_t) (end - sdp) : strlen(sdp);
    if (n && sdp[n - 1] == '\r') {
      --n;
    }
    if (n >= sizeof(prefix) - 1 && !memcmp(sdp, prefix, sizeof(prefix) - 1)) {
      if (++count != 1 || n != sizeof(prefix) || sdp[sizeof(prefix) - 1] != '1') {
        return 0;
      }
    }
    if (!end) {
      break;
    }
    sdp = end + 1;
  }
  return count == 1 ? STEADY_INPUT_VERSION : 0;
}

/** @brief Validate a fixed-size frame before state changes. @param p Bytes. @param n Length. @return Valid. */
static inline int steady_input_valid(const unsigned char *p, size_t n) {
  unsigned i;
  if (!p || n != STEADY_INPUT_BYTES || memcmp(p, "STK1", 4) || p[4] != 1 || p[5] < SI_HELLO || p[5] > SI_REJECT || p[6] != 0 || p[7] != STEADY_INPUT_BYTES || !steady_probe_get64(p + 8) || steady_probe_get64(p + 40) != STEADY_INPUT_RESERVE || steady_probe_get64(p + 48) != STEADY_INPUT_LIFETIME) {
    return 0;
  }
  for (i = 59; i < 64; ++i) {
    if (p[i]) {
      return 0;
    }
  }
  if (p[5] == SI_KEY) {
    return steady_probe_get64(p + 16) != 0 && p[56] == 0 && p[57] >= 4 && p[58] <= 1;
  }
  return !steady_probe_get64(p + 16) && !p[56] && !p[57] && !p[58];
}

/** @brief Initialize a canonical frame. @param p Output. @param kind Kind. @param generation Session generation. */
static inline void steady_input_frame(unsigned char *p, unsigned kind, uint64_t generation) {
  memset(p, 0, STEADY_INPUT_BYTES);
  memcpy(p, "STK1", 4);
  p[4] = 1;
  p[5] = (unsigned char) kind;
  p[7] = STEADY_INPUT_BYTES;
  steady_probe_put64(p + 8, generation);
  steady_probe_put64(p + 40, STEADY_INPUT_RESERVE);
  steady_probe_put64(p + 48, STEADY_INPUT_LIFETIME);
}

/** @brief Compare an immutable offer. @param a Frame. @param b Offer. @return Same generation, anchors and policy. */
static inline int steady_input_same_offer(const unsigned char *a, const unsigned char *b) {
  return !memcmp(a + 8, b + 8, 8) && !memcmp(a + 24, b + 24, 32);
}
#endif
