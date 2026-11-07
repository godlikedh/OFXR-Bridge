# Native FSR producer overlap

The private runtime already owns the physical presentation clock. Previously,
the upper FSR EndFrame path also waited until both queued outputs were accepted
by that endpoint. Its consumer waits for synthesis before acceptance, so this
made the next game frame wait for the current pair's GPU work.

Native D3D12 FSR with deferred direct publication now returns after enqueue.
The consumer retains the exact output images, metadata, publication objects
and readiness fences. The next application EndFrame still waits for upper
queue admission before recording another pair. Private image acquisition,
publication busy flags, GPU dependencies and endpoint capacity are unchanged.
The game can render its next frame during that interval; outputs cannot be
overwritten or handed to the native runtime before their fences complete.

This changes CPU admission, not AMD's internal async-workload setting. It adds
no GPU queue, buffers or generated frames. Other generation and interop routes
retain their existing release policy. Physical cadence still comes from the
private runtime, and an overloaded GPU can still miss display slots.

`xrfg_private_fsr_overlap` runs the shipping layer with a controlled synthesis
queue fence. EndFrame must return while the fence is withheld, the next pair
must remain blocked at admission, and the downstream runtime must see no
unfinished output. After release, the usual generated-output, pixel, metadata
and lifecycle checks apply. The FSR live-toggle fixture covers draining.

Diagnostic collection temporarily enables the existing timing recorder even
when F9 is off. It does not edit the user's logging preference; ending the
collection restores the current preference. The collector takes its final
flight-log snapshot after observation, including newly-created logger epochs.
