# Preserve usable display slots after preparation

MSFS captures at approximately 89.91 Hz showed recurring missed slots even at
Medium/Low graphics settings. In 47 of 52 recorded physical-slot gaps across two
15-second windows, the image copy and native Begin/Release were complete
0.24–1.05 ms before the skipped slot's intended handoff. The former eligibility
check nevertheless required another 1.1 ms after Begin. That reserve was already
used to schedule native entry early, so small wakeup/clock variations could
exclude a usable slot and add a whole refresh period.

The physical director now claims a prepared slot after all those prerequisites
complete. It retains the 100 us final CPU preparation guard, the physical display
grid, the existing interval floor, and all image/fence/FIFO ownership. Native
Wait lookahead and stale-prediction renewal remain intact. The fallback schedule
used before a physical clock is available is unchanged.

`xrfg_private_output_distributor_tests` replays five captured cases, including
physical-clock resampling. It demonstrates the old policy's missed slot, verifies
the prepared packet uses that slot, and rejects unfinished/late preparation,
early delivery, slot reuse and catch-up bursts. This is a deterministic scheduling
regression; Windows runtime tests and headset measurements are separate checks.

This fixes one avoidable cadence loss. A source frame, GPU job or native call
that actually misses its deadline still requires a later slot. It does not make
90 FPS possible under arbitrary graphics load or eliminate other game stalls.
