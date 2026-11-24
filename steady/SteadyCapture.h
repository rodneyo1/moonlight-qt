/**
 * @file SteadyCapture.h
 * @brief STEADY keyboard capture stamps taken in the SDL event filter, before SDL enqueues the event.
 *
 * What the stamp means: QPC (in nanoseconds, the same conversion as steady_probe_now) read when
 * SDL offers a keyboard event to the application's event filter, immediately before SDL appends it
 * to its queue. It is an SDL enqueue timestamp. It is NOT physical actuation, USB/HID report,
 * Windows raw-input or window-message time; OS and SDL delivery delay before the filter remains
 * unmeasured and must be reported as such.
 *
 * Carrying the stamp: the filter stores the stamp in a fixed slot and writes a 32-bit token into
 * SDL_Keysym.unused. The main-thread handler takes the slot by token and checks the stored source
 * identity (scancode, down). Any failure yields capture 0, which the timed adapter rejects after
 * commitment; handler time is never substituted.
 *
 * Tokens increase for the entire process lifetime and are never reused across sessions.
 * UINT32_MAX is an "unstamped" marker: the filter saw the event but could not record it
 * (all slots live, sequence exhausted, clock failure). Token 0 means the filter never saw the event
 * (SDL_PeepEvents bypass, filter not installed, event queued before installation).
 *
 * Storage is fixed; no operation allocates. Live slots are never overwritten. Every method is
 * serialized by one mutex because SDL documents that the filter may run on another thread.
 * Standalone: no Qt or SDL dependency, so tools/core/input-capture-tests.cpp can compile it alone.
 */
#ifndef STEADY_CAPTURE_H
#define STEADY_CAPTURE_H
#include <cstdint>
#include <mutex>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace steady_capture {

constexpr uint32_t kSlots = 256;  // At most this many stamped events may wait in the SDL queue.
static_assert((kSlots & (kSlots - 1)) == 0, "slot count must be a power of two");
constexpr uint32_t kSeqMask = UINT32_MAX;
constexpr uint32_t kUnstampedSeq = kSeqMask;      // Reserved marker, never an issued sequence.
constexpr uint32_t kLastSeq = kUnstampedSeq - 1;

enum class Result : uint8_t {
  Ok,         // Original stamp returned and consumed.
  Missing,    // Token 0: the filter never saw this event.
  Unstamped,  // Filter saw it but had no free slot, sequence or clock.
  Stale,      // Token issued before this session, or no session is armed.
  Forged,     // Sequence never issued in this epoch.
  Replayed,   // Token already consumed (slot possibly reused since).
  Mismatch    // Token live but scancode/down differs; slot is consumed anyway.
};

inline const char *name(Result r) {
  switch (r) {
    case Result::Ok: return "ok";
    case Result::Missing: return "missing";
    case Result::Unstamped: return "unstamped";
    case Result::Stale: return "stale";
    case Result::Forged: return "forged";
    case Result::Replayed: return "replayed";
    case Result::Mismatch: return "mismatch";
  }
  return "unknown";
}

struct Lookup {
  Result result;
  uint64_t capture_ns;  // Nonzero only for Result::Ok.
};

struct Counters {
  uint64_t stamped, taken, overflow, exhausted, clock_faults, failures;
};

/** @brief Same arithmetic and limits as steady_probe_now. @return Nonzero ns, or 0 on failure. */
inline uint64_t ticks_to_ns(uint64_t ticks, uint64_t frequency) {
  if (!frequency || !ticks || frequency > UINT64_MAX / 1000000000u) {
    return 0;
  }
  const uint64_t whole = ticks / frequency;
  const uint64_t fraction = (ticks % frequency) * 1000000000u / frequency;
  if (whole > (UINT64_MAX - fraction) / 1000000000u) {
    return 0;
  }
  return whole * 1000000000u + fraction;
}

#if defined(_WIN32)
/** @brief Current QPC in ns, or 0 on failure. Safe on any thread; does not allocate. */
inline uint64_t qpc_ns() {
  LARGE_INTEGER stamp, freq;
  if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0 || !QueryPerformanceCounter(&stamp) || stamp.QuadPart <= 0) {
    return 0;
  }
  return ticks_to_ns((uint64_t) stamp.QuadPart, (uint64_t) freq.QuadPart);
}

/** @brief Exact opt-in, identical to steady_input_opt_in: STEADY_INPUT_ENABLE must be "1". */
inline bool opt_in() {
  char value[2];
  return GetEnvironmentVariableA("STEADY_INPUT_ENABLE", value, sizeof(value)) == 1 && value[0] == '1';
}
#endif

class Ring {
public:
  explicit Ring(uint32_t last_token = kLastSeq) : last_token_(last_token < kUnstampedSeq ? last_token : kLastSeq) {}
  Ring(const Ring &) = delete;
  Ring &operator=(const Ring &) = delete;

  /** @brief Start a session with a new token floor, all slots free, stamping enabled. */
  uint64_t begin_session() {
    std::lock_guard<std::mutex> lock(m_);
    reset_locked();
    if (session_ == UINT64_MAX) {
      armed_ = false;
      return 0;
    }
    ++session_;
    armed_ = true;
    return session_;
  }

  /** @brief End a session: stamping disabled and every outstanding token becomes stale. */
  void end_session() {
    std::lock_guard<std::mutex> lock(m_);
    reset_locked();
    armed_ = false;
  }

  /** @brief Producer (SDL filter). @return Token for SDL_Keysym.unused; 0 only when not armed. */
  uint32_t stamp(uint32_t source, bool down, uint64_t capture_ns) {
    std::lock_guard<std::mutex> lock(m_);
    return stamp_locked(source, down, capture_ns);
  }

  /** @brief Reject an in-flight producer belonging to a removed filter/session. */
  uint32_t stamp_for_session(uint64_t expected, uint32_t source, bool down, uint64_t capture_ns) {
    std::lock_guard<std::mutex> lock(m_);
    if (expected != session_) {
      return 0;
    }
    return stamp_locked(source, down, capture_ns);
  }

private:
  uint32_t stamp_locked(uint32_t source, bool down, uint64_t capture_ns) {
    if (!armed_) {
      return 0;
    }
    const uint32_t unstamped = kUnstampedSeq;
    if (!capture_ns) {
      ++counters_.clock_faults;
      return unstamped;
    }
    if (next_ > last_token_) {
      ++counters_.exhausted;
      return unstamped;
    }
    Slot &slot = slots_[next_ & (kSlots - 1)];
    if (slot.live) {
      ++counters_.overflow;  // Refuse rather than overwrite an unconsumed original.
      return unstamped;
    }
    const uint32_t token = static_cast<uint32_t>(next_++);
    slot = Slot {token, source, capture_ns, down, true};
    ++counters_.stamped;
    return token;
  }

public:
  /** @brief Consumer (event handler). Consumes any live slot the token names, even on mismatch. */
  Lookup take(uint32_t token, uint32_t source, bool down) {
    std::lock_guard<std::mutex> lock(m_);
    uint64_t capture_ns = 0;
    const Result r = take_locked(token, source, down, capture_ns);
    if (r != Result::Ok) {
      ++counters_.failures;
      return {r, 0};
    }
    ++counters_.taken;
    return {r, capture_ns};
  }

  Counters counters() {
    std::lock_guard<std::mutex> lock(m_);
    return counters_;
  }

  /** @brief Slots stamped but not yet taken (still in, or lost from, the SDL queue). */
  uint32_t live() {
    std::lock_guard<std::mutex> lock(m_);
    uint32_t n = 0;
    for (const Slot &slot : slots_) {
      n += slot.live ? 1u : 0u;
    }
    return n;
  }

private:
  struct Slot {
    uint32_t token;
    uint32_t source;
    uint64_t capture_ns;
    bool down;
    bool live;
  };

  void reset_locked() {
    session_first_ = next_;
    for (Slot &slot : slots_) {
      slot = Slot {};
    }
    counters_ = Counters {};
  }

  Result take_locked(uint32_t token, uint32_t source, bool down, uint64_t &capture_ns) {
    if (!token) {
      return Result::Missing;
    }
    if (token == kUnstampedSeq) {
      return Result::Unstamped;
    }
    if (!armed_ || token < session_first_) {
      return Result::Stale;
    }
    if (token >= next_) {
      return Result::Forged;
    }
    Slot &slot = slots_[token & (kSlots - 1)];
    if (!slot.live || slot.token != token) {
      return Result::Replayed;
    }
    slot.live = false;
    if (slot.source != source || slot.down != down) {
      return Result::Mismatch;
    }
    capture_ns = slot.capture_ns;
    return Result::Ok;
  }

  std::mutex m_;
  Slot slots_[kSlots] {};
  Counters counters_ {};
  const uint32_t last_token_;
  uint64_t next_ = 1, session_first_ = 1;
  uint64_t session_ = 0;
  bool armed_ = false;
};

}  // namespace steady_capture
#endif
