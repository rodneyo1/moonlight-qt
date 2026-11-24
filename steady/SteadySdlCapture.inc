/* Actual SDL capture/filter lifecycle, included after SdlInputHandler is declared.
 * Kept separate so its real callbacks can be exercised with an SDL queue fixture. */
#ifdef Q_OS_WIN32
namespace {
// STEADY pre-queue keyboard stamps (STEADY_INPUT_ENABLE=1 on Windows only).
// The stamp is QPC when SDL offers the event to its event filter, before SDL
// enqueues it: an SDL enqueue timestamp, not physical/hardware actuation.
// SDL documents that the filter may run on another thread and that
// SDL_PeepEvents() bypasses it; bypassed events carry no token and fail closed.
//
// All state has static storage. A filter call still running on another thread
// while (or after) a handler is destroyed touches only these objects, never a
// handler pointer, so there is no destroyed-handler access.
steady_capture::Ring s_SteadyRing;
enum { SteadyFilterPassThrough, SteadyFilterInstalling, SteadyFilterArmed };
struct SteadyFilterContext {
    SDL_EventFilter previous = nullptr;
    void* userdata = nullptr;
    uint64_t session = 0;
    std::atomic<int> mode{SteadyFilterPassThrough};
};
// Never recycle a callback's context: an in-flight SDL callback may retain it.
// Refuse installation on exhaustion instead of reusing a prior chain identity.
std::array<SteadyFilterContext,4096> s_SteadyContexts;
size_t s_SteadyContextsUsed = 0;
SteadyFilterContext* s_SteadyContext = nullptr; // Main thread only.
bool s_SteadyInstalled;  // Main thread only
bool s_SteadyStranded;   // Main thread only: another filter may still chain to ours

int SDLCALL steadyCaptureFilter(void* userdata, SDL_Event* event)
{
    auto* context = static_cast<SteadyFilterContext*>(userdata);
    // Immutable per-registration function/userdata pair, including while SDL
    // re-filters old events. Separate atomics could mix two registrations.
    if (context->previous && !context->previous(context->userdata, event)) {
        return 0; // Dropped: never enqueued, so it takes no slot
    }
    const int mode = context->mode.load(std::memory_order_acquire);
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        if (mode == SteadyFilterInstalling) {
            event->key.keysym.unused = 0; // Previously queued timestamp is unknown.
        }
        else if (mode == SteadyFilterArmed) {
            event->key.keysym.unused = s_SteadyRing.stamp_for_session(context->session,
                (uint32_t)event->key.keysym.scancode,event->key.state == SDL_PRESSED,steady_capture::qpc_ns());
        }
    }
    return 1;
}
}
#endif

void SdlInputHandler::installSteadyCapture()
{
#ifdef Q_OS_WIN32
    m_SteadyCaptureOptIn = steady_capture::opt_in();
    if (!m_SteadyCaptureOptIn) {
        return;
    }

    // From here on, originals always use LiSendKeyboardEventTimedAt(). If the
    // filter cannot be installed, tokens are missing and capture 0 makes the
    // timed session fail closed rather than substituting handler time.
    if (SDL_GetHint("SDL3_VERSION") != nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "STEADY keyboard capture unavailable: sdl2-compat does not preserve SDL_Keysym.unused");
        return;
    }
    if (s_SteadyInstalled || s_SteadyStranded || s_SteadyContextsUsed == s_SteadyContexts.size()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "STEADY keyboard capture unavailable: previous event filter chain not restored");
        return;
    }

    SDL_EventFilter prev = nullptr;
    void* prevUserdata = nullptr;
    if (!SDL_GetEventFilter(&prev, &prevUserdata)) {
        prev = nullptr;
        prevUserdata = nullptr;
    }
    auto* context = &s_SteadyContexts[s_SteadyContextsUsed++];
    context->previous = prev;
    context->userdata = prevUserdata;
    context->session = s_SteadyRing.begin_session();
    if (!context->session) {
        return;
    }
    context->mode.store(SteadyFilterInstalling,std::memory_order_release);
    SDL_SetEventFilter(steadyCaptureFilter, context);
    context->mode.store(SteadyFilterArmed,std::memory_order_release);
    s_SteadyContext = context;
    s_SteadyInstalled = m_SteadyCaptureInstalled = true;
    // Only now is pre-queue capture real. The handler is constructed before the
    // connection starts, so common-C sees this before ControlInit and keeps it.
    LiSteadyInputSetCaptureAvailable(1);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "STEADY keyboard capture: QPC stamped in SDL event filter before SDL enqueue (not physical actuation)");
#endif
}

void SdlInputHandler::removeSteadyCapture()
{
#ifdef Q_OS_WIN32
    if (!m_SteadyCaptureInstalled) {
        // Disabled or failed installation: still clear any registration, unless
        // another handler's filter is armed and owns it.
        if (!s_SteadyInstalled) {
            LiSteadyInputSetCaptureAvailable(0);
        }
        return;
    }

    // Withdraw before pass-through: from here on originals fail closed.
    LiSteadyInputSetCaptureAvailable(0);

    steady_capture::Counters counters = s_SteadyRing.counters();
    unsigned int live = s_SteadyRing.live();

    // Pass-through first: any in-flight or later call only chains the previous filter.
    auto* context = s_SteadyContext;
    context->mode.store(SteadyFilterPassThrough, std::memory_order_release);

    SDL_EventFilter current = nullptr;
    void* currentUserdata = nullptr;
    if (SDL_GetEventFilter(&current, &currentUserdata) && current == steadyCaptureFilter && currentUserdata == context) {
        SDL_SetEventFilter(context->previous,context->userdata);
    }
    else {
        // Someone replaced our filter and may still chain to it. Keep the
        // pass-through function and previous-filter pointers intact, and refuse
        // later installs, which would otherwise make the chain self-referential.
        s_SteadyStranded = true;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "STEADY keyboard capture: event filter was replaced; left in pass-through mode");
    }

    // Advance the token floor: queued tokens become stale without ever reusing identities.
    s_SteadyRing.end_session();
    s_SteadyInstalled = m_SteadyCaptureInstalled = false;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "STEADY keyboard capture: %llu stamped, %llu taken, %llu failed lookups, %llu overflow, %llu exhausted, %llu clock faults, %u untaken",
                (unsigned long long)counters.stamped, (unsigned long long)counters.taken,
                (unsigned long long)counters.failures, (unsigned long long)counters.overflow,
                (unsigned long long)counters.exhausted, (unsigned long long)counters.clock_faults, live);
#endif
}

