# Phase 0 Spike

Status: Phase 0 complete, NO-GO for the current capture/overlay architecture (2026-10-30). No capture strategy is selected for implementation.

## Experiments

1. Apply [focus-appid.patch](focus-appid.patch) to a temporary copy of the existing PipeWire capture helper. It selects the YouTube TV shortcut (`AppID 3609786786`); with a moving video playing, switch back to Steam Library and verify frame buffers continue arriving.
2. Build `x11-overlay-probe` with `make -C spike`, then run `./out/x11-overlay-probe` inside the Gamescope Xwayland session. It creates a fullscreen ARGB window with the `GAMESCOPE_EXTERNAL_OVERLAY` CARDINAL property and an empty input shape. The rectangle it draws only checks visible alpha composition. Test visibility over a game and Steam UI, including controller input and overlay/QAM/notification interactions.
3. Repeat with the HyperHDR PipeWire reader active and record whether both consumers receive usable frames.
4. If PipeWire focus capture is unsuitable, build `xcomposite-dmabuf-probe` with `make -C spike` and run it with an Xwayland window ID (for example `./out/xcomposite-dmabuf-probe 0x3000002`). It names the window pixmap, exports it through DRI3, imports the DMA-BUF through GBM EGL, and samples a 32x32 grid ten times for changing pixels. Test with a browser and a WSI Vulkan app; this finite sample does not measure sustained rendering performance.
5. Confirm an unfocused app continues audio/video playback and determine whether its PipeWire audio stream can be controlled as the plugin user.
6. Verify drain-before-disconnect shutdown behavior for every capture path.

## Overlay Probe Smoke Test

- 2026-10-30, HTPC host `htpc`: active Gamescope process detected; probe compiled with `-Wall -Wextra -Werror` against the installed X11/Xext/Xrender libraries.
- Ran on `DISPLAY=:0` with a 20-second timeout. The client created and mapped X11 window `0x3200002`, then remained alive until timeout cleanup.
- User observed the red probe shape at the top-left of the screen, confirming visible composition in the tested session.
- User confirmed controller input still reached the game and Steam/QAM UI and notifications remained visible over the probe.
- When the user enabled MangoHud/performance overlay through QAM, the Floater probe disappeared; the user confirmed it reappeared when MangoHud was disabled. The two external overlays do not coexist in the tested Gamescope session and need an explicit design decision.

## PipeWire Focus-AppID Smoke Test

- 2026-10-30, HTPC host `htpc`: resolved YouTube TV AppID `3609786786` from Steam's active `shortcuts.vdf`; Gamescope reported focus AppID `769` (Steam) during the run.
- Built a temporary copy of the HyperHDR PipeWire reader with `focus_appid=3609786786`. With the existing `Gamescope Hue capture` stream active on target `gamescope`, the probe negotiated BGRx at 512x288 but received zero buffers in five seconds.
- With user approval, stopped `hyperhdr.service` and repeated the same test while Steam remained focused (AppID `769`). The probe received 300 buffers in five seconds; FFmpeg sampled four frames with distinct hashes. This confirms moving frames from the non-focused YouTube TV app are available to a single PipeWire consumer.
- SIGINT stopped the isolated helper through its 500 ms drain timer (`Capture received 300 buffers`); Gamescope stayed running without an assertion. Restarted `hyperhdr.service`; both the supervisor and `gamescope-hyperhdr.service` returned active, and the Hue connection re-established.
- The paired result is consistent with a second-consumer conflict on this Gamescope PipeWire source, but needs another controlled repeat before being treated as definitive. Do not select or reject the capture strategy from one comparison alone.

## XComposite/DRI3 DMA-BUF Import Smoke Test

- 2026-10-30, HTPC host `htpc`: ran against the YouTube TV playback window (`0x3000002`, X11 class `mpvk`, 3840x2160).
- `XCompositeNameWindowPixmap` succeeded; DRI3 exported one XRGB8888 plane with a modifier; GBM EGL successfully created an `EGLImage` from the DMA-BUF.
- After returning to Firefox, found its Xwayland display was `:1` (Steam/Gamescope control properties were on `:0`). Ran against the live YouTube TV browser window (`0x600003`, X11 class `firefox`, 3840x2160); DRI3 exported one ARGB8888 plane with a modifier and GBM EGL successfully imported it.
- With the user confirming moving video, ten GLES2 samples from one imported EGLImage had a single repeated hash; this does not prove a changing image because that imported pixmap may be a snapshot.
- A follow-up local variant re-named/re-exported/re-imported the pixmap before each sample. On the HTPC, EGLImage creation succeeded, but `glEGLImageTargetTexture2DOES` returned `GL_INVALID_VALUE` (`0x501`) for both 2D and external-texture targets. The direct 2D FBO was incomplete (`0x8cd6`); no pixel hashes are valid evidence of updates. This GBM EGL import path is not yet GLES-sampleable for the tested modifier.
- These checks confirm XComposite-to-DRI3 export and GBM EGL DMA-BUF import for both mpv's WSI Vulkan window and Firefox. GBM EGL has no pbuffer config, so the ten-sample sampler uses its surfaceless/no-config extensions with an FBO. Dynamic pixels and sustained performance remain unverified.

## Audio Mapping Smoke Test

- 2026-10-30, HTPC host `htpc`: PipeWire audio node `73` maps to client `74`, process `mpv` PID `697144`, whose parent is the Firefox process PID `598619` for the YouTube TV shortcut.
- As the normal HTPC user, `wpctl set-volume 73 1.00` succeeded and read back `Volume: 1.00`; this verified control permission without changing the level. Mute behavior and mapping across other apps remain untested.

## Decision Gate

Test platform: HTPC with Intel Arc B580, Gamescope 3.16.25, and Xwayland displays `:0`/`:1`.

Decision: NO-GO for Phase 1 under the current architecture. The prototype does not meet the requirements for a public plugin that must coexist with HyperHDR and MangoHud, and neither tested capture route currently supplies a confirmed sampleable background frame while preserving the other constraints.

Evidence:

- PipeWire `focus_appid` produced changing buffers from the non-focused YouTube TV AppID when used alone. With the existing HyperHDR reader active on the same Gamescope target, the second stream received zero buffers in the one controlled comparison.
- The `GAMESCOPE_EXTERNAL_OVERLAY` window was visible, passed controller input, and allowed Steam/QAM UI and notifications above it. Enabling MangoHud made the probe disappear; disabling MangoHud restored it.
- XComposite plus DRI3 exported DMA-BUFs from Firefox and mpv WSI windows, and GBM EGL created EGLImages. GLES `glEGLImageTargetTexture2DOES` returned `GL_INVALID_VALUE` (`0x501`) for both 2D and external texture targets; the direct 2D FBO was incomplete. Dynamic pixel sampling is not established.
- The PipeWire reader drained on SIGINT, reported 300 buffers, and did not crash Gamescope. Audio node/client/process mapping was resolved; `wpctl set-volume` succeeded at the unchanged 1.00 level.

Re-entry criteria: demonstrate a capture path that returns changing frames while HyperHDR remains active; demonstrate coexistence with MangoHud; and confirm sustained sampling performance, browser and WSI Vulkan behavior, and safe teardown. The tested audio mapping and volume permission can be reused. No plugin backend or UI implementation should start until a viable capture strategy is demonstrated.
