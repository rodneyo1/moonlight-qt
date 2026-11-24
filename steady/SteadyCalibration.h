/**
 * @file SteadyCalibration.h
 * @brief Deterministic causal clock-agreement fitter, shared verbatim by both endpoints.
 *
 * Model, stated as an explicit condition rather than a measured fact: the host
 * clock is an affine function of the client clock, H(c) = H(C) + a*(c - C) with
 * a > 0, over the calibration interval and its admitted continuation. Every
 * answered exchange (client t1 send, host t2 receive, host t3 send, client t4
 * receive) imposes H(t1) <= t2 and H(t4) >= t3. No symmetric-delay, minimum-delay
 * or nominal-rate prior is used, and no answered exchange is ever trimmed: one
 * contradiction rejects admission. The bounds are a sound outer relaxation of the
 * exact feasible set used by tools/windows/clock_envelope.py: rate bounds come
 * from early/late exchange pairs and the anchor bound from every exchange.
 *
 * All arithmetic is checked unsigned 64-bit integer arithmetic with explicit
 * outward rounding, so both peers compute bit-identical agreements from the same
 * exchange table on any conforming C or C++ compiler (MSVC and MinGW included).
 * The caller owns the storage; nothing here allocates.
 */
#ifndef STEADY_CALIBRATION_H
#define STEADY_CALIBRATION_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STEADY_CAL_PPB UINT64_C(1000000000)  ///< Rate denominator: rates are parts per billion of host ns per client ns.
#define STEADY_CAL_CAPACITY 12000u  ///< Fixed exchange inventory; equal to STEADY_PROBE_SAMPLES.
#define STEADY_CAL_MIN_USABLE 10000u  ///< Minimum answered exchanges before admission.
#define STEADY_CAL_MIN_SPAN_NS UINT64_C(30000000000)  ///< Minimum first-to-last answered send span, client clock.
#define STEADY_CAL_WINDOW 64u  ///< Lowest-round-trip exchanges retained per early/late window for rate bounds.
#define STEADY_CAL_RTT_OVERSIZE UINT32_C(0xFFFFFFFF)  ///< Report code: answered, but round trip not representable.
#define STEADY_CAL_RESERVE_CLEAN UINT64_C(15000000)  ///< Declared CLEAN fixed reserve (A6a target).
#define STEADY_CAL_RESERVE_STANDARD UINT64_C(25000000)  ///< Declared STANDARD fixed reserve (A6b target).
#define STEADY_CAL_ABSOLUTE_CLEAN UINT64_C(3000000)  ///< Absolute clock-mapping error budget for CLEAN.
#define STEADY_CAL_ABSOLUTE_STANDARD UINT64_C(5000000)  ///< Absolute clock-mapping error budget for STANDARD.
#define STEADY_CAL_PAIR_SHORT_NS UINT64_C(150000000)  ///< Short supported pair scale.
#define STEADY_CAL_PAIR_SHORT_BUDGET UINT64_C(20000)  ///< Clock share of the short pair distortion budget.
#define STEADY_CAL_PAIR_LONG_NS UINT64_C(2000000000)  ///< Contract ceiling pair scale.
#define STEADY_CAL_MAPPING_SHORT_NS UINT64_C(200000000)  ///< A1a supported pair ceiling.
#define STEADY_CAL_MAPPING_SHORT_BUDGET UINT64_C(200000)  ///< A1a mapping distortion ceiling.
#define STEADY_CAL_MAPPING_LONG_BUDGET UINT64_C(1000000)  ///< A1b mapping distortion ceiling.
#define STEADY_CAL_PAIR_LONG_BUDGET UINT64_C(200000)  ///< Clock share of the 2000 ms pair distortion budget.
#define STEADY_CAL_MAX_AGE_NS UINT64_C(7200000000000)  ///< Resource ceiling: fixed trace storage covers two hours.
#define STEADY_CAL_MIN_AGE_NS UINT64_C(30000000000)  ///< Admission is refused when bounds support less validity.

/** @brief One calibration exchange in client/host QPC nanoseconds. t4 == 0 means unanswered. */
typedef struct steady_cal_exchange {
  uint64_t t1;  ///< Client pre-send time.
  uint64_t t2;  ///< Host post-authentication receive time.
  uint64_t t3;  ///< Host pre-encryption send time.
  uint64_t t4;  ///< Client receive time, zero when no reply was accepted.
} steady_cal_exchange;

/** @brief Fit outcome. Anything but OK refuses timed admission. */
enum steady_cal_status {
  STEADY_CAL_OK = 0,
  STEADY_CAL_ARGUMENT = 1,
  STEADY_CAL_MALFORMED = 2,
  STEADY_CAL_TOO_FEW = 3,
  STEADY_CAL_TOO_SHORT = 4,
  STEADY_CAL_RATE_UNBOUNDED = 5,
  STEADY_CAL_CONTRADICTION = 6,
  STEADY_CAL_CANDIDATE = 7,
  STEADY_CAL_INITIAL_ERROR = 8,
  STEADY_CAL_PAIR_ERROR = 9,
  STEADY_CAL_VALIDITY = 10,
  STEADY_CAL_ENVELOPE = 11,
  STEADY_CAL_ARITHMETIC = 12
};

/** @brief Complete evidence and immutable agreement. Agreement fields are zero unless status is OK. */
typedef struct steady_cal_result {
  uint64_t status;  ///< steady_cal_status.
  uint64_t attempted, answered, unanswered;  ///< All attempts are retained and counted.
  uint64_t late;  ///< Answered exchanges that would miss the reserve at the admitted age.
  uint64_t contradictions;  ///< Malformed exchanges or exchange pairs implying a non-positive rate.
  uint64_t first_send, last_send;  ///< First/last answered client send times.
  uint64_t reserve, absolute_budget;  ///< Declared fixed profile.
  uint64_t rate_low, rate_high;  ///< Outward-rounded feasible rate bounds, ppb.
  uint64_t rate, rate_error;  ///< Immutable candidate rate N/1e9 and its worst deviation from the bounds, ppb.
  uint64_t lower, upper;  ///< Outward-rounded bounds on true H(client_anchor).
  uint64_t client_anchor, host_anchor;  ///< Integer anchors of the frozen map.
  uint64_t initial_error;  ///< Bound on |H(C) - host_anchor|, including the integer deadline rounding.
  uint64_t age;  ///< Admitted client-clock validity from the anchor.
  uint64_t client_valid_until, host_valid_until;  ///< Local expiries; host side uses the lower rate bound.
  uint64_t envelope_growth;  ///< Rate-error drift applied to the arrival-envelope check.
} steady_cal_result;

/** @brief Map a declared reserve to its clock budget. @param reserve Reserve ns. @param absolute Output. @return Known profile. */
static inline int steady_cal_profile(uint64_t reserve, uint64_t *absolute) {
  if (reserve == STEADY_CAL_RESERVE_CLEAN) {
    *absolute = STEADY_CAL_ABSOLUTE_CLEAN;
    return 1;
  }
  if (reserve == STEADY_CAL_RESERVE_STANDARD) {
    *absolute = STEADY_CAL_ABSOLUTE_STANDARD;
    return 1;
  }
  return 0;
}

/** @brief Exact 128-bit product. @param a Factor. @param b Factor. @param hi High half. @param lo Low half. */
static inline void steady_cal_mul(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
  const uint64_t mask = UINT64_C(0xFFFFFFFF);
  const uint64_t a0 = a & mask, a1 = a >> 32, b0 = b & mask, b1 = b >> 32;
  const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  const uint64_t mid = (p00 >> 32) + (p01 & mask) + (p10 & mask);
  *lo = (p00 & mask) | (mid << 32);
  *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/**
 * @brief Exact floor(a*b/d) with remainder, without compiler-specific 128-bit types.
 * @param a Factor. @param b Factor. @param d Divisor. @param q Quotient. @param r Remainder.
 * @return Zero for a zero divisor or a quotient wider than 64 bits.
 */
static inline int steady_cal_muldiv(uint64_t a, uint64_t b, uint64_t d, uint64_t *q, uint64_t *r) {
  uint64_t hi, lo, rem, quo = 0;
  unsigned i;
  if (!d) {
    return 0;
  }
  steady_cal_mul(a, b, &hi, &lo);
  if (hi >= d) {
    return 0;
  }
  rem = hi;
  for (i = 0; i < 64; ++i) {
    const uint64_t top = rem >> 63;
    rem = (rem << 1) | (lo >> 63);
    lo <<= 1;
    quo <<= 1;
    if (top || rem >= d) {
      rem -= d;  // Exact modulo 2^64: the true value is below 2*d.
      quo |= 1;
    }
  }
  *q = quo;
  *r = rem;
  return 1;
}

/** @brief Rounded value*ppb/1e9. @param value Duration. @param ppb Rate. @param up Ceil when nonzero. @param out Result. @return Success. */
static inline int steady_cal_scale(uint64_t value, uint64_t ppb, int up, uint64_t *out) {
  uint64_t q, r;
  if (!steady_cal_muldiv(value, ppb, STEADY_CAL_PPB, &q, &r)) {
    return 0;
  }
  if (up && r) {
    if (q == UINT64_MAX) {
      return 0;
    }
    ++q;
  }
  *out = q;
  return 1;
}

/** @brief Rounded num*1e9/den. @param num Numerator. @param den Denominator. @param up Ceil when nonzero. @param out Result. @return Success. */
static inline int steady_cal_ratio(uint64_t num, uint64_t den, int up, uint64_t *out) {
  uint64_t q, r;
  if (!steady_cal_muldiv(num, STEADY_CAL_PPB, den, &q, &r)) {
    return 0;
  }
  if (up && r) {
    if (q == UINT64_MAX) {
      return 0;
    }
    ++q;
  }
  *out = q;
  return 1;
}

/** @brief Checked sum. @param a Addend. @param b Addend. @param out Result. @return Success. */
static inline int steady_cal_add(uint64_t a, uint64_t b, uint64_t *out) {
  if (a > UINT64_MAX - b) {
    return 0;
  }
  *out = a + b;
  return 1;
}

/**
 * @brief Keep the K lowest round trips in a window, scanning in sequence order.
 * Ties keep the earliest exchanges (early window) or the latest (late window, later != 0),
 * so equal-delay data spans the widest interval. Any subset yields sound bounds.
 */
static inline void steady_cal_keep(uint32_t *index, uint64_t *rtt, unsigned *count, uint32_t i, uint64_t value, int later) {
  unsigned at = *count;
  if (at == STEADY_CAL_WINDOW) {
    if (later ? value > rtt[STEADY_CAL_WINDOW - 1] : value >= rtt[STEADY_CAL_WINDOW - 1]) {
      return;
    }
    at = STEADY_CAL_WINDOW - 1;
  } else {
    ++*count;
  }
  while (at && (later ? value <= rtt[at - 1] : value < rtt[at - 1])) {
    index[at] = index[at - 1];
    rtt[at] = rtt[at - 1];
    --at;
  }
  index[at] = i;
  rtt[at] = value;
}

/** @brief Fail with a status, keeping the counted evidence. @param out Result. @param status Reason. @return Status. */
static inline int steady_cal_fail(steady_cal_result *out, enum steady_cal_status status) {
  out->status = (uint64_t) status;
  out->rate = out->rate_error = out->client_anchor = out->host_anchor = out->initial_error = 0;
  out->age = out->client_valid_until = out->host_valid_until = 0;
  return (int) status;
}

/**
 * @brief Fit and qualify one immutable frozen agreement from a complete exchange inventory.
 *
 * Unanswered attempts count as arrival misses. A miss rate at or above 0.1 %
 * (A4a target), any malformed answered exchange, any contradiction, an
 * unbounded rate, or bounds that cannot support the declared reserve profile,
 * pair budgets and the minimum validity refuse admission.
 *
 * @param x Exchange inventory indexed by sequence; entries at or beyond attempted are ignored.
 * @param attempted Attempted sequences, at most STEADY_CAL_CAPACITY.
 * @param reserve Declared CLEAN or STANDARD fixed reserve.
 * @param out Evidence and agreement.
 * @return steady_cal_status; STEADY_CAL_OK only when the agreement is admissible.
 */
static inline int steady_cal_fit(const steady_cal_exchange *x, uint64_t attempted, uint64_t reserve, steady_cal_result *out) {
  uint32_t early[STEADY_CAL_WINDOW], late[STEADY_CAL_WINDOW];
  uint64_t early_rtt[STEADY_CAL_WINDOW], late_rtt[STEADY_CAL_WINDOW];
  unsigned early_n = 0, late_n = 0, a, b;
  uint64_t i, third, lo = 0, hi = UINT64_MAX, n, err, anchor = 0, hhi = UINT64_MAX, hlo = 0, upper = UINT64_MAX, lower = 0;
  uint64_t h, mid, e0, growth = 0, allowance, age, host_a = 0, host_b = 0, misses, envelope = 0, s = 0, v = 0;
  int have_hi = 0;
  if (!out) {
    return STEADY_CAL_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  out->attempted = attempted;
  out->reserve = reserve;
  if (!x || !attempted || attempted > STEADY_CAL_CAPACITY || !steady_cal_profile(reserve, &out->absolute_budget)) {
    return steady_cal_fail(out, STEADY_CAL_ARGUMENT);
  }
  out->first_send = UINT64_MAX;
  for (i = 0; i < attempted; ++i) {
    const steady_cal_exchange *e = x + i;
    if (!e->t4) {
      ++out->unanswered;
      continue;
    }
    if (!e->t1 || !e->t2 || e->t3 < e->t2 || e->t4 < e->t1) {
      ++out->contradictions;
      continue;
    }
    ++out->answered;
    out->first_send = e->t1 < out->first_send ? e->t1 : out->first_send;
    out->last_send = e->t1 > out->last_send ? e->t1 : out->last_send;
    anchor = e->t4 > anchor ? e->t4 : anchor;
  }
  if (out->contradictions) {
    return steady_cal_fail(out, STEADY_CAL_MALFORMED);
  }
  if (out->answered < STEADY_CAL_MIN_USABLE) {
    return steady_cal_fail(out, STEADY_CAL_TOO_FEW);
  }
  if (out->last_send - out->first_send < STEADY_CAL_MIN_SPAN_NS) {
    return steady_cal_fail(out, STEADY_CAL_TOO_SHORT);
  }
  third = (out->last_send - out->first_send) / 3;
  for (i = 0; i < attempted; ++i) {
    const steady_cal_exchange *e = x + i;
    if (!e->t4) {
      continue;
    }
    if (e->t1 <= out->first_send + third) {
      steady_cal_keep(early, early_rtt, &early_n, (uint32_t) i, e->t4 - e->t1, 0);
    }
    if (e->t1 >= out->last_send - third) {
      steady_cal_keep(late, late_rtt, &late_n, (uint32_t) i, e->t4 - e->t1, 1);
    }
  }
  // Necessary pair bounds: H(t4_j) - H(t1_i) >= t3_j - t2_i and H(t1_j) - H(t4_i) <= t2_j - t3_i.
  for (a = 0; a < early_n; ++a) {
    const steady_cal_exchange *p = x + early[a];
    for (b = 0; b < late_n; ++b) {
      const steady_cal_exchange *q = x + late[b];
      if (q->t3 > p->t2 && q->t4 > p->t1) {
        if (!steady_cal_ratio(q->t3 - p->t2, q->t4 - p->t1, 0, &v)) {
          return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
        }
        lo = v > lo ? v : lo;
      }
      if (q->t1 > p->t4) {
        if (q->t2 <= p->t3) {
          ++out->contradictions;  // Would require a non-positive clock rate.
          continue;
        }
        if (!steady_cal_ratio(q->t2 - p->t3, q->t1 - p->t4, 1, &v)) {
          return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
        }
        hi = v < hi ? v : hi;
        have_hi = 1;
      }
    }
  }
  if (out->contradictions) {
    return steady_cal_fail(out, STEADY_CAL_CONTRADICTION);
  }
  out->rate_low = lo;
  out->rate_high = have_hi ? hi : 0;
  if (!have_hi || !lo) {
    return steady_cal_fail(out, STEADY_CAL_RATE_UNBOUNDED);
  }
  if (lo > hi) {
    ++out->contradictions;
    return steady_cal_fail(out, STEADY_CAL_CONTRADICTION);
  }
  // Fixed candidate rule: midpoint of the outward bounds. No unit-rate preference.
  n = lo + (hi - lo) / 2;
  err = n - lo > hi - n ? n - lo : hi - n;
  for (i = 0; i < attempted; ++i) {
    const steady_cal_exchange *e = x + i;
    if (!e->t4) {
      continue;
    }
    if (!steady_cal_scale(anchor - e->t1, n, 0, &s) || !steady_cal_add(e->t2, s, &v)) {
      return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
    }
    hhi = v < hhi ? v : hhi;
    if (!steady_cal_scale(anchor - e->t4, n, 1, &s) || !steady_cal_add(e->t3, s, &v)) {
      return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
    }
    hlo = v > hlo ? v : hlo;
    if (!steady_cal_scale(anchor - e->t1, hi, 1, &s) || !steady_cal_add(e->t2, s, &v)) {
      return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
    }
    upper = v < upper ? v : upper;
    if (!steady_cal_scale(anchor - e->t4, lo, 0, &s) || !steady_cal_add(e->t3, s, &v)) {
      return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
    }
    lower = v > lower ? v : lower;
  }
  out->lower = lower;
  out->upper = upper;
  if (lower > upper) {
    ++out->contradictions;
    return steady_cal_fail(out, STEADY_CAL_CONTRADICTION);
  }
  if (hlo > hhi || !hlo) {
    return steady_cal_fail(out, STEADY_CAL_CANDIDATE);
  }
  mid = lower + (upper - lower) / 2;
  h = mid < hlo ? hlo : (mid > hhi ? hhi : mid);
  e0 = (h - lower > upper - h ? h - lower : upper - h) + 1;  // +1: ceil of the integer affine deadline.
  if (e0 > out->absolute_budget) {
    return steady_cal_fail(out, STEADY_CAL_INITIAL_ERROR);
  }
  if (!steady_cal_scale(STEADY_CAL_PAIR_SHORT_NS, err, 1, &growth) || growth + 1 > STEADY_CAL_PAIR_SHORT_BUDGET ||
      !steady_cal_scale(STEADY_CAL_PAIR_LONG_NS, err, 1, &growth) || growth + 1 > STEADY_CAL_PAIR_LONG_BUDGET) {
    return steady_cal_fail(out, STEADY_CAL_PAIR_ERROR);
  }
  // A precise estimate can still distort intervals. Admission must constrain the
  // chosen mapping itself, independently of uncertainty about the true clock.
  {
    const uint64_t distortion = n > STEADY_CAL_PPB ? n - STEADY_CAL_PPB : STEADY_CAL_PPB - n;
    if (!steady_cal_scale(STEADY_CAL_MAPPING_SHORT_NS, distortion, 1, &growth) || growth >= STEADY_CAL_MAPPING_SHORT_BUDGET ||
        !steady_cal_scale(STEADY_CAL_PAIR_LONG_NS, distortion, 1, &growth) || growth >= STEADY_CAL_MAPPING_LONG_BUDGET) {
      return steady_cal_fail(out, STEADY_CAL_PAIR_ERROR);
    }
  }
  // Same rule as steady::make_clock_agreement: the budget bounds E0 + rate_error*age.
  allowance = out->absolute_budget - e0;
  age = STEADY_CAL_MAX_AGE_NS;
  if (err) {
    if (!steady_cal_scale(age, err, 1, &growth)) {
      return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
    }
    if (growth > allowance) {
      uint64_t r;
      if (!steady_cal_muldiv(allowance, STEADY_CAL_PPB, err, &age, &r)) {
        return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
      }
    }
  }
  if (age < STEADY_CAL_MIN_AGE_NS) {
    return steady_cal_fail(out, STEADY_CAL_VALIDITY);
  }
  // Arrival envelope: drift between any calibration send and the last admitted capture.
  if (!steady_cal_add(age, anchor - out->first_send, &v) || !steady_cal_scale(v, err, 1, &envelope)) {
    return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
  }
  misses = out->unanswered;
  for (i = 0; i < attempted; ++i) {
    const steady_cal_exchange *e = x + i;
    uint64_t base, arrival;
    if (!e->t4) {
      continue;
    }
    if (!steady_cal_scale(anchor - e->t1, n, 0, &s) || !steady_cal_add(h, reserve, &base) || !steady_cal_add(e->t2, envelope, &arrival)) {
      return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
    }
    if (s > base || arrival > base - s) {
      ++out->late;
      ++misses;
    }
  }
  out->envelope_growth = envelope;
  if (misses * 1000 >= attempted) {
    return steady_cal_fail(out, STEADY_CAL_ENVELOPE);
  }
  if (!steady_cal_add(anchor, age, &out->client_valid_until) || !steady_cal_add(h, age, &host_a) ||
      !steady_cal_scale(age, lo, 0, &s) || !steady_cal_add(lower, s, &host_b)) {
    return steady_cal_fail(out, STEADY_CAL_ARITHMETIC);
  }
  out->host_valid_until = host_a < host_b ? host_a : host_b;
  if (out->host_valid_until <= h) {
    return steady_cal_fail(out, STEADY_CAL_VALIDITY);
  }
  out->rate = n;
  out->rate_error = err;
  out->client_anchor = anchor;
  out->host_anchor = h;
  out->initial_error = e0;
  out->age = age;
  out->status = STEADY_CAL_OK;
  return STEADY_CAL_OK;
}

/**
 * @brief Encode the round-trip report code a client sends for one sequence.
 * @param e Client exchange. @return 0 unanswered, rtt+1, or STEADY_CAL_RTT_OVERSIZE.
 */
static inline uint32_t steady_cal_report_code(const steady_cal_exchange *e) {
  uint64_t rtt;
  if (!e->t4) {
    return 0;
  }
  if (e->t4 < e->t1) {
    return STEADY_CAL_RTT_OVERSIZE;
  }
  rtt = e->t4 - e->t1;
  return rtt >= UINT64_C(0xFFFFFFFE) ? STEADY_CAL_RTT_OVERSIZE : (uint32_t) (rtt + 1);
}
#endif
