/**
 * @file SteadyProbe.h
 * @brief Private, non-injecting authenticated-control calibration frame helpers.
 */
#ifndef STEADY_PROBE_H
#define STEADY_PROBE_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STEADY_PROBE_TYPE 0x5354
#define STEADY_PROBE_BYTES 64
#define STEADY_PROBE_VERSION 2
#define STEADY_PROBE_SAMPLES 12000
#define STEADY_PROBE_TARGET 10000
#define STEADY_PROBE_MIN_NS UINT64_C(30000000000)
#define STEADY_PROBE_MAX_NS UINT64_C(180000000000)
#define STEADY_PROBE_DRAIN_MS 1000

/** @brief Explicit terminal states for the bounded diagnostic collector. */
enum steady_probe_stop {
  STEADY_PROBE_RUNNING = 0,
  STEADY_PROBE_STOP_TARGET = 1,
  STEADY_PROBE_STOP_CAPACITY = 2,
  STEADY_PROBE_STOP_TIMEOUT = 3,
  STEADY_PROBE_STOP_SEND = 4,
  STEADY_PROBE_STOP_CLOCK = 5,
  STEADY_PROBE_STOP_CANCEL = 6,
  STEADY_PROBE_STOP_THREAD = 7
};

/**
 * @brief Decide whether a bounded collection needs another independent attempt.
 * @param attempted Number of attempted unique probes.
 * @param usable First replies passing the local exchange time checks.
 * @param elapsed_ns Time since the first attempt on the client clock.
 * @return Running, target met, capacity exhausted, or timeout reached.
 */
static inline enum steady_probe_stop steady_probe_collection_stop(uint64_t attempted, uint64_t usable, uint64_t elapsed_ns) {
  if (usable >= STEADY_PROBE_TARGET && elapsed_ns >= STEADY_PROBE_MIN_NS) {
    return STEADY_PROBE_STOP_TARGET;
  }
  if (attempted >= STEADY_PROBE_SAMPLES) {
    return STEADY_PROBE_STOP_CAPACITY;
  }
  if (elapsed_ns >= STEADY_PROBE_MAX_NS) {
    return STEADY_PROBE_STOP_TIMEOUT;
  }
  return STEADY_PROBE_RUNNING;
}

/** @brief Bounded per-session diagnostic replay state, owned by control thread. */
typedef struct steady_probe_session {
  uint64_t generation;  ///< First admitted generation, zero before admission.
  uint64_t seen[(STEADY_PROBE_SAMPLES + 63) / 64];  ///< Receipt bitmap for this finite probe run.
} steady_probe_session;

/** @brief Read explicit diagnostic opt-in. @return Nonzero only for exact value 1. */
static inline int steady_probe_opt_in(void) {
#if defined(_WIN32)
  char value[2];
  return GetEnvironmentVariableA("STEADY_PROBE_ENABLE", value, sizeof(value)) == 1 && value[0] == '1';
#else
  return 0;
#endif
}

/**
 * @brief Accept exactly one complete supported SDP attribute line.
 * @param sdp NUL-terminated SDP already bounded by the RTSP parser.
 * @return Supported version for exactly one canonical attribute, otherwise zero.
 */
static inline unsigned steady_probe_sdp_version(const char *sdp) {
  const char prefix[] = "a=x-steady-input.probeVersion:";
  const char *line = sdp;
  unsigned matches = 0;
  if (!line) {
    return 0;
  }
  while (*line) {
    const char *end = strchr(line, '\n');
    size_t length = end ? (size_t) (end - line) : strlen(line);
    if (length && line[length - 1] == '\r') {
      --length;
    }
    if (length >= sizeof(prefix) - 1 && memcmp(line, prefix, sizeof(prefix) - 1) == 0) {
      if (++matches != 1 || length != sizeof(prefix) || line[sizeof(prefix) - 1] != '0' + STEADY_PROBE_VERSION) {
        return 0;
      }
    }
    if (!end) {
      break;
    }
    line = end + 1;
  }
  return matches ? STEADY_PROBE_VERSION : 0;
}

/** @brief Decode one big-endian 64-bit field. @param p Field bytes. @return Value. */
static inline uint64_t steady_probe_get64(const unsigned char *p) {
  uint64_t value = 0;
  unsigned i;
  for (i = 0; i < 8; ++i) {
    value = (value << 8) | p[i];
  }
  return value;
}

/** @brief Encode one big-endian field. @param p Output bytes. @param value Value. */
static inline void steady_probe_put64(unsigned char *p, uint64_t value) {
  unsigned i;
  for (i = 8; i > 0; --i) {
    p[i - 1] = (unsigned char) value;
    value >>= 8;
  }
}

/**
 * @brief Validate a complete private request/reply without mutating any state.
 * @param p Payload bytes, after existing control authentication.
 * @param size Exact payload length.
 * @param kind Expected request (1) or reply (2).
 * @return Nonzero for canonical syntax; this does not authenticate a packet.
 */
static inline int steady_probe_valid(const unsigned char *p, size_t size, unsigned kind) {
  if (!p || size != STEADY_PROBE_BYTES || (kind != 1 && kind != 2)) {
    return 0;
  }
  if (p[0] != STEADY_PROBE_VERSION || p[1] != kind || p[2] != 0 || p[3] != STEADY_PROBE_BYTES || p[4] || p[5] || p[6] || p[7] || steady_probe_get64(p + 56)) {
    return 0;
  }
  if (!steady_probe_get64(p + 8) || steady_probe_get64(p + 16) >= STEADY_PROBE_SAMPLES || !steady_probe_get64(p + 24)) {
    return 0;
  }
  if (kind == 1) {
    return !steady_probe_get64(p + 32) && !steady_probe_get64(p + 40) && !steady_probe_get64(p + 48);
  }
  return steady_probe_get64(p + 32) && steady_probe_get64(p + 40) >= steady_probe_get64(p + 32) &&
         steady_probe_get64(p + 48);
}

/**
 * @brief Construct a canonical request; invalid values leave output unchanged.
 * @param output Destination frame.
 * @param generation Fresh nonzero per-session generation.
 * @param sequence Probe identity below the fixed inventory capacity.
 * @param t1 Client pre-send QPC nanoseconds.
 * @return Nonzero on success.
 */
static inline int steady_probe_request(unsigned char *output, uint64_t generation, uint64_t sequence, uint64_t t1) {
  unsigned char frame[STEADY_PROBE_BYTES] = {0};
  if (!output || !generation || sequence >= STEADY_PROBE_SAMPLES || !t1) {
    return 0;
  }
  frame[0] = STEADY_PROBE_VERSION;
  frame[1] = 1;
  frame[3] = STEADY_PROBE_BYTES;
  steady_probe_put64(frame + 8, generation);
  steady_probe_put64(frame + 16, sequence);
  steady_probe_put64(frame + 24, t1);
  memcpy(output, frame, sizeof(frame));
  return 1;
}

/**
 * @brief Construct a reply only after caller-confirmed control-v2 authentication.
 * @param output Destination, unchanged on rejection; may alias request.
 * @param request Request payload.
 * @param size Request length.
 * @param authenticated Nonzero only after existing control-v2 authentication.
 * @param t2 Host post-authentication receive QPC nanoseconds.
 * @param t3 Host pre-encryption send QPC nanoseconds.
 * @param frequency Host QPC frequency.
 * @return Nonzero on success.
 */
static inline int steady_probe_reply(unsigned char *output, const unsigned char *request, size_t size, int authenticated, uint64_t t2, uint64_t t3, uint64_t frequency) {
  unsigned char frame[STEADY_PROBE_BYTES];
  if (!output || !authenticated || !steady_probe_valid(request, size, 1) || !t2 || t3 < t2 || !frequency) {
    return 0;
  }
  memcpy(frame, request, sizeof(frame));
  frame[1] = 2;
  steady_probe_put64(frame + 32, t2);
  steady_probe_put64(frame + 40, t3);
  steady_probe_put64(frame + 48, frequency);
  memcpy(output, frame, sizeof(frame));
  return 1;
}

/**
 * @brief Admit one authenticated request once per fixed diagnostic generation.
 * @param state Session state owned by the control thread.
 * @param output Destination reply, unchanged on rejection.
 * @param request Request frame.
 * @param size Exact request size.
 * @param authenticated Caller-confirmed control-v2 authentication.
 * @param t2 Host receive time in QPC nanoseconds.
 * @param t3 Host pre-encryption send time in QPC nanoseconds.
 * @param frequency Host QPC frequency.
 * @return Nonzero on admission; duplicates and other generations do not mutate state.
 */
static inline int steady_probe_reply_once(steady_probe_session *state, unsigned char *output, const unsigned char *request, size_t size, int authenticated, uint64_t t2, uint64_t t3, uint64_t frequency) {
  unsigned char frame[STEADY_PROBE_BYTES];
  uint64_t sequence, generation, mask;
  if (!state || !output || !steady_probe_reply(frame, request, size, authenticated, t2, t3, frequency)) {
    return 0;
  }
  sequence = steady_probe_get64(request + 16);
  generation = steady_probe_get64(request + 8);
  mask = UINT64_C(1) << (sequence % 64);
  if ((state->generation && state->generation != generation) || (state->seen[sequence / 64] & mask)) {
    return 0;
  }
  state->generation = generation;
  state->seen[sequence / 64] |= mask;
  memcpy(output, frame, sizeof(frame));
  return 1;
}

#if defined(_WIN32)
/**
 * @brief Convert QPC to nanoseconds using checked integer arithmetic.
 * @param ns Output timestamp, unchanged on failure.
 * @param frequency Output QPC frequency, unchanged on failure.
 * @return Nonzero on success; requires Windows declarations before this header.
 */
static inline int steady_probe_now(uint64_t *ns, uint64_t *frequency) {
  LARGE_INTEGER stamp, freq;
  uint64_t whole, fraction, f, ticks;
  if (!ns || !frequency || !QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0 || !QueryPerformanceCounter(&stamp) || stamp.QuadPart <= 0) {
    return 0;
  }
  f = (uint64_t) freq.QuadPart;
  ticks = (uint64_t) stamp.QuadPart;
  if (f > UINT64_MAX / 1000000000) {
    return 0;
  }
  whole = ticks / f;
  fraction = (ticks % f) * 1000000000 / f;
  if (whole > (UINT64_MAX - fraction) / 1000000000) {
    return 0;
  }
  *ns = whole * 1000000000 + fraction;
  *frequency = f;
  return 1;
}
#endif
#endif
