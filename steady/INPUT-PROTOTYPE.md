# Experimental fixed-buffer keyboard path

This opt-in Windows prototype connects Moonlight keyboard callbacks to Sunshine's bounded Windows scheduler and its selected libvirtualhid keyboard backend. Ordinary input remains the default. It is not the qualified STEADY release.

- Private control type `0x5355`, capability `x-steady-input.keyboardVersion:1`; both peers must opt in, advertise support and use authenticated control-v2. No new socket or driver.
- HELLO / OFFER / COMMIT / READY activation; a key before commitment selects legacy for that session. After commitment, failures terminate the session and release held keys rather than replaying through legacy.
- Reliable ordered keyboard-channel transport for this first connected experiment. Application-level ACK/retry/snapshots from the reference core are not yet bound to this transport. Reliable head-of-line blocking remains a limitation to measure.
- Unit-rate frozen mapping anchored from HELLO creation to host receipt, plus a fixed 40 ms reserve. Startup network delay contributes a constant offset. This does not certify absolute one-way latency or the clock-rate/interval target. No slewing or rebuffering.
- Maximum 60-second experimental session; expiry closes the stream and cleans up keys. Reconnect for another short comparison. This time cap is an experiment limit, not a budget-derived accuracy guarantee.
- Timestamp point is the client send API callback, after SDL's event queue; it is not physical actuation or event creation. Pre-queue capture and measured rate agreement remain release work.
- Physical USB keyboard profile; custom host key remaps and Right-Alt-to-Win configurations decline activation. Host repeat synthesis and Sunshine host shortcuts are bypassed while timed ownership is active; held-key repeat behavior is not qualified. Unicode text injection is excluded during timed ownership. Mouse/video/audio continue through their ordinary paths.
- Fixed ingress, pending, replay and trace capacities. No growing event queue. Backend failure, overflow, expiry or host clock continuity failure closes the owner and attempts key release. A failed native cleanup remains a reported failure.

Launch both apps with process environment `STEADY_INPUT_ENABLE=1` and `STEADY_INPUT_TRACE` set to a new writable CSV filename. The client must read the host's matching capability; stock peers retain legacy behavior. Trace files are created at teardown, never overwritten. Use disposable input such as a blank editor for the first short comparison.

Client CSV records source identity, callback time, USB usage and queue outcome. Host CSV records receipts and native backend attempts with host QPC timestamps. Match backend rows to admitted transitions; raw traces and all original counts must be retained. These logs are implementation evidence, not an automatic timing PASS.

The host vendors the seven receiver/waiter/clock headers from the workspace's tested `tools/core/`. Its `SteadyInputHost.inc` is exercised directly by the local binding harness as well as the full Sunshine fake-backend regression suite. The client's common-c changes remain a reproducible patch against pinned commit `04053843c90c4268583f2496b997d54d59f03957`.
