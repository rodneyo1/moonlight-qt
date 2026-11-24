/**
 * @file SteadyInput.h
 * @brief Private opt-in calibrated frozen keyboard protocol, version 2 (control-v2 only).
 *
 * Both forks carry this file and SteadyCalibration.h verbatim. Version 1 peers
 * never match the capability, so they keep the exact legacy path.
 */
#ifndef STEADY_INPUT_H
#define STEADY_INPUT_H
#include "SteadyProbe.h"
#include "SteadyCalibration.h"
#define STEADY_INPUT_TYPE 0x5355
#define STEADY_INPUT_BYTES 64
#define STEADY_INPUT_VERSION 2
#define STEADY_INPUT_CAL_PERIOD_MS 3  ///< Request spacing: at most 334 records/s, and capacity cannot be exhausted before 30 s.
#define STEADY_INPUT_ACK_TIMEOUT_NS UINT64_C(3000000000)  ///< Precommit HELLO answer limit.
#define STEADY_INPUT_OFFER_TIMEOUT_NS UINT64_C(10000000000)  ///< Precommit OFFER limit after sealing.
#define STEADY_INPUT_READY_TIMEOUT_NS UINT64_C(3000000000)  ///< Postcommit READY limit; expiry is terminal.
#define STEADY_INPUT_REPORT_CODES 10u  ///< Round-trip codes per REPORT frame.
#define STEADY_INPUT_REPORT_BLOCKS ((STEADY_CAL_CAPACITY + STEADY_INPUT_REPORT_CODES - 1) / STEADY_INPUT_REPORT_CODES)
/** @brief Planned trace span in original key events: 2 h at 8 keydowns/s, each with one release. */
#define STEADY_INPUT_TRACE_ORIGINALS (2u * 3600u * 8u * 2u)
/** @brief Fixed client original-trace rows; covers STEADY_INPUT_TRACE_ORIGINALS (115200) with headroom. */
#define STEADY_INPUT_CLIENT_TRACE_ROWS 131072u
/** @brief Fixed host trace rows; one receipt and one backend row per original (230400) plus cleanup headroom. */
#define STEADY_INPUT_HOST_TRACE_ROWS 262144u

#if STEADY_CAL_CAPACITY != STEADY_PROBE_SAMPLES
  #error "Calibration inventory must match the probe inventory"
#endif

/**
 * @brief Record kinds. Every frame is 64 bytes: "STK2", version, kind, 0, 64,
 * generation (8..15), then six big-endian 64-bit slots s0..s5 at 16..63.
 */
enum steady_input_kind {
  SI_HELLO = 1,  ///< c->h reliable: s0 reserve, s1 frequency, s2 power epoch, s3/s4 incarnation.
  SI_OFFER = 2,  ///< h->c reliable: s0 rate ppb, s1 client anchor, s2 host anchor, s3 age, s4 host expiry, s5 E0<<32|rate error.
  SI_COMMIT = 3,  ///< c->h reliable: exact OFFER slots.
  SI_READY = 4,  ///< h->c reliable: exact OFFER slots.
  SI_KEY = 5,  ///< c->h reliable: s0 sequence, s1 capture, s2..s4 rate/anchors, bytes 57 usage, 58 down.
  SI_REJECT = 6,  ///< Either direction: s0 reason; terminal after commitment.
  SI_HELLO_ACK = 7,  ///< h->c reliable: s0 reserve, s1 frequency, s2 power epoch, s3/s4 incarnation.
  SI_CAL_REQ = 8,  ///< c->h unsequenced: s0 sequence, s1 t1.
  SI_CAL_REP = 9,  ///< h->c unsequenced: s0 sequence, s1 t1, s2 t2, s3 t3.
  SI_REPORT = 10,  ///< c->h reliable: s0 first sequence, then ten 32-bit round-trip codes.
  SI_SEALED = 11  ///< c->h reliable: s0 attempted, s1 stop reason, s2 seal time, s3 client-usable count.
};

/** @brief Read slot k (0..5). @param p Frame. @param k Slot. @return Value. */
static inline uint64_t steady_input_slot(const unsigned char *p, unsigned k) {
  return steady_probe_get64(p + 16 + 8 * k);
}

/** @brief Write slot k (0..5). @param p Frame. @param k Slot. @param value Value. */
static inline void steady_input_set(unsigned char *p, unsigned k, uint64_t value) {
  steady_probe_put64(p + 16 + 8 * k, value);
}

/** @brief Read one REPORT round-trip code. @param p Frame. @param k Index 0..9. @return Code. */
static inline uint32_t steady_input_code(const unsigned char *p, unsigned k) {
  const unsigned char *q = p + 24 + 4 * k;
  return ((uint32_t) q[0] << 24) | ((uint32_t) q[1] << 16) | ((uint32_t) q[2] << 8) | (uint32_t) q[3];
}

/** @brief Write one REPORT round-trip code. @param p Frame. @param k Index 0..9. @param code Code. */
static inline void steady_input_set_code(unsigned char *p, unsigned k, uint32_t code) {
  unsigned char *q = p + 24 + 4 * k;
  q[0] = (unsigned char) (code >> 24);
  q[1] = (unsigned char) (code >> 16);
  q[2] = (unsigned char) (code >> 8);
  q[3] = (unsigned char) code;
}

/** @brief Explicit experimental input opt-in. @return True only for exact value 1. */
static inline int steady_input_opt_in(void) {
#if defined(_WIN32)
  char value[2];
  return GetEnvironmentVariableA("STEADY_INPUT_ENABLE", value, sizeof(value)) == 1 && value[0] == '1';
#else
  return 0;
#endif
}

/**
 * @brief Declared fixed reserve, chosen before any input. STEADY_INPUT_PROFILE=standard
 * selects 25 ms; unset or "clean" selects 15 ms; anything else disables timed input.
 * @return Reserve in ns, or zero.
 */
static inline uint64_t steady_input_reserve(void) {
#if defined(_WIN32)
  char value[16];
  const unsigned long n = GetEnvironmentVariableA("STEADY_INPUT_PROFILE", value, sizeof(value));
  if (!n) {
    return STEADY_CAL_RESERVE_CLEAN;
  }
  if (n == 5 && !memcmp(value, "clean", 5)) {
    return STEADY_CAL_RESERVE_CLEAN;
  }
  if (n == 8 && !memcmp(value, "standard", 8)) {
    return STEADY_CAL_RESERVE_STANDARD;
  }
#endif
  return 0;
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
      if (++count != 1 || n != sizeof(prefix) || sdp[sizeof(prefix) - 1] != '0' + STEADY_INPUT_VERSION) {
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

/** @brief Validate a fixed-size frame before any state change. @param p Bytes. @param n Length. @return Canonical. */
static inline int steady_input_valid(const unsigned char *p, size_t n) {
  uint64_t s[6];
  unsigned i;
  if (!p || n != STEADY_INPUT_BYTES || memcmp(p, "STK2", 4) || p[4] != STEADY_INPUT_VERSION || p[5] < SI_HELLO || p[5] > SI_SEALED || p[6] != 0 || p[7] != STEADY_INPUT_BYTES || !steady_probe_get64(p + 8)) {
    return 0;
  }
  for (i = 0; i < 6; ++i) {
    s[i] = steady_input_slot(p, i);
  }
  switch (p[5]) {
    case SI_HELLO:
    case SI_HELLO_ACK:
      return (s[0] == STEADY_CAL_RESERVE_CLEAN || s[0] == STEADY_CAL_RESERVE_STANDARD) && s[1] && s[1] <= UINT64_MAX / 1000000000 && s[2] && (s[3] || s[4]) && !s[5];
    case SI_OFFER:
    case SI_COMMIT:
    case SI_READY:
      return s[0] && s[1] && s[2] && s[3] && s[4] > s[2] && (s[5] >> 32) && s[1] <= UINT64_MAX - s[3];
    case SI_KEY:
      if (!s[0] || !s[1] || !s[2] || !s[3] || !s[4] || p[56] || p[57] < 4 || p[58] > 1) {
        return 0;
      }
      for (i = 59; i < 64; ++i) {
        if (p[i]) {
          return 0;
        }
      }
      return 1;
    case SI_REJECT:
      return !s[1] && !s[2] && !s[3] && !s[4] && !s[5];
    case SI_CAL_REQ:
      return s[0] < STEADY_CAL_CAPACITY && s[1] && !s[2] && !s[3] && !s[4] && !s[5];
    case SI_CAL_REP:
      return s[0] < STEADY_CAL_CAPACITY && s[1] && s[2] && s[3] >= s[2] && !s[4] && !s[5];
    case SI_REPORT:
      return s[0] < STEADY_CAL_CAPACITY && s[0] % STEADY_INPUT_REPORT_CODES == 0;
    case SI_SEALED:
      return s[0] && s[0] <= STEADY_CAL_CAPACITY && s[2] && s[3] <= s[0] && !s[4] && !s[5];
    default:
      return 0;
  }
}

/** @brief Initialize a canonical frame. @param p Output. @param kind Kind. @param generation Session generation. */
static inline void steady_input_frame(unsigned char *p, unsigned kind, uint64_t generation) {
  memset(p, 0, STEADY_INPUT_BYTES);
  memcpy(p, "STK2", 4);
  p[4] = STEADY_INPUT_VERSION;
  p[5] = (unsigned char) kind;
  p[7] = STEADY_INPUT_BYTES;
  steady_probe_put64(p + 8, generation);
}

/** @brief Write the immutable agreement slots of an OFFER/COMMIT/READY. @param p Frame. @param r Admitted fit. */
static inline void steady_input_agreement(unsigned char *p, const steady_cal_result *r) {
  steady_input_set(p, 0, r->rate);
  steady_input_set(p, 1, r->client_anchor);
  steady_input_set(p, 2, r->host_anchor);
  steady_input_set(p, 3, r->age);
  steady_input_set(p, 4, r->host_valid_until);
  steady_input_set(p, 5, (r->initial_error << 32) | (r->rate_error & UINT64_C(0xFFFFFFFF)));
}

/** @brief Compare an immutable agreement. @param a Frame. @param b Offer. @return Same generation and all six slots. */
static inline int steady_input_same_offer(const unsigned char *a, const unsigned char *b) {
  return !memcmp(a + 8, b + 8, 8) && !memcmp(a + 16, b + 16, 48);
}

/** @brief Check that a KEY frame binds the committed rate and anchors. @param key KEY frame. @param offer Committed offer. @return Bound. */
static inline int steady_input_key_bound(const unsigned char *key, const unsigned char *offer) {
  return !memcmp(key + 8, offer + 8, 8) && !memcmp(key + 32, offer + 16, 24);
}
#endif
