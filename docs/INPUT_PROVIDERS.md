# External input providers

This fork keeps OFXR's OpenXR interception, pacing, owned GPU snapshots, frame generators and presentation. Game-specific input capture can live in a separate DLL. The existing V1/V2 exports remain available.

## Read the change

Start at `include/xrfg/input_provider_api.hpp` for the versioned POD boundary, then `src/layer/input_providers.cpp` for its translation into the existing guide transport. `ngx_guide_capture.cpp` drives provider polling and queue configuration; `native_ingress.cpp` reports actual command-list submission and retirement. `openxr_layer.cpp` reports projection-image receipts.

The fork also carries generic AMD frame-generation fixes developed with the prototype: SDK 2.4 ML-provider selection, asynchronous context preparation, exact release-bound guide matching, two/four-view handling, bounded snapshot retention, and foveated guide composition. Those are separate from the plugin interface; this is not an interface-only patch.

## Package and lifecycle

Place `NAME.ofxr-provider.dll` in `providers/` beside the layer. OFXR initializes sorted providers before forwarding `xrCreateInstance`, outside the loader lock. An initializer must decline unsupported processes/ABI versions before installing hooks. Modules remain loaded until process exit because installed detours can outlive a session. Disable/stop callbacks disarm capture; polling continues to support a later session.

The callback table contains only fixed-width scalars, arrays, borrowed D3D12 pointers and function pointers. No STL objects, allocator ownership or exceptions cross the boundary. Providers must be thread-safe: render, submission and monitor callbacks can overlap. The host table remains valid for the process lifetime.

## Input ownership and matching

Publish while the producer command list is still recording and after its upscale dispatch. The host records its owned depth/motion snapshots on that list. The game's resources must remain alive through execution; publication does not prove execution. OFXR retains its snapshots and uses its existing execution/fence tracking.

Attach a late camera to the exact returned stream/publication receipt before consumer admission. Do not substitute the latest camera, another eye, or a tracking-space headset pose. Stream IDs must be unique across providers. Invalid or ambiguous data must invalidate history; missing input does not authorize generation.

## Companion tools

`providers/tools/OFXRProviderTools.exe` is optional. F10 can launch it, including `--collect`. The external process owns diagnostic collection; the shared diagnostics mailbox carries status only. Provider directories are staged with the runtime. A product can set `XRFG_PROFILE_DIRECTORY` at build time to isolate its settings.

## Validation limits

Windows/WARP tests cover API and GPU plumbing. Headset timing, hardware ML-provider behavior and game updates still require hardware validation. The interface is experimental ABI version 1, not an upstream compatibility promise.
