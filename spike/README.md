# Phase 0 Spike

Status: Phase 0 complete, NO-GO for the current capture/overlay architecture (2026-10-30). No capture strategy is selected for implementation.

## Experiments

1. Apply [focus-appid.patch](focus-appid.patch) to a temporary copy of the existing PipeWire capture helper. It selects the YouTube TV shortcut (`AppID 3609786786`); with a moving video playing, switch back to Steam Library and verify frame buffers continue arriving.
2. Build `x11-overlay-probe` with `make -C spike`, then run `./out/x11-overlay-probe` inside the Gamescope Xwayland session. It creates a fullscreen ARGB window with the `GAMESCOPE_EXTERNAL_OVERLAY` CARDINAL property and an empty input shape. The rectangle it draws only checks visible alpha composition. Test visibility over a game and Steam UI, including controller input and overlay/QAM/notification interactions.
3. Repeat with the HyperHDR PipeWire reader active and record whether both consumers receive usable frames.
4. If PipeWire focus capture is unsuitable, build `xcomposite-dmabuf-probe` with `make -C spike` and run it with an Xwayland window ID (for example `./out/xcomposite-dmabuf-probe 0x3000002`). It names the window pixmap, exports it through DRI3, imports the DMA-BUF through GBM EGL, and samples a 32x32 grid ten times for changing pixels. Test with a browser and a WSI Vulkan app; this finite sample does not measure sustained rendering performance.
5. Confirm an unfocused app continues audio/video playback and determine whether its PipeWire audio stream can be controlled as the plugin user.
6. Verify drain-before-disconnect shutdown behavior for every capture path.
7. Build `mangoapp-mirror-probe` with `make -C spike` and run `DISPLAY=:0 ./out/mangoapp-mirror-probe [seconds] [center]`. Gamescope 3.16.25 paints only one `GAMESCOPE_EXTERNAL_OVERLAY` window (topmost, highest opacity), so the probe keeps itself above mangoapp, copies mangoapp's damaged pixels into its own overlay with XRender, and draws a 640x360 Picture in Picture placeholder bottom right (or centred). It reports damage, copied pixels, re-raises, source/copy hash changes, and CPU every 5 s.
8. Build `pip-prototype` with `make -C spike` and run `DISPLAY=:0 ./out/pip-prototype APPID [SECONDS] [WIDTH HEIGHT]` with HyperHDR stopped. It connects to the Gamescope PipeWire node with `focus_appid` and `requested_size` (default 640x360), uploads each BGRx frame through MIT-SHM into a pixmap, and composites it bottom right in the external overlay alongside the mangoapp mirror. It hides the box while `GAMESCOPE_FOCUSED_APP` equals the AppID and drains the stream for 500 ms on SIGINT/SIGTERM or after SECONDS. Diagnostic switches: `FLOATER_MANGO_CLIP=WxH` limits mangoapp copies to a top-left area, `FLOATER_NO_CAPTURE=1` draws a static box without PipeWire, and `FLOATER_NO_OVERLAY=1` captures without mapping or uploading. `FLOATER_MAX_FPS=N` holds every dequeued buffer and returns one per 1/N s.

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

## MangoHud Mirror Smoke Test

- 2026-11-01, HTPC, Gamescope 3.16.25, HyperHDR active (the probe does not use PipeWire). mangoapp is a GLFW/GLX window titled `mangoapp overlay window` on `:0`, sized 3840x2160 at 0,0. It sets `GAMESCOPE_EXTERNAL_OVERLAY` to 1 and maps when shown, and sets it to 0 and unmaps when hidden.
- A pixmap named once with `XCompositeNameWindowPixmap` never changed (`0/5` hash changes despite ~100 damage events/s), consistent with Present flips replacing the window pixmap. An XRender picture on the mangoapp window itself follows the current buffer: source and copy hashes changed on every 1 s sample.
- The user confirmed MangoHud and the placeholder box were visible together with MangoHud updating. Across three off/on toggles and a level change, MangoHud returned each time, the probe re-raised above the newly mapped mangoapp window, the box stayed visible, mirroring and CPU dropped to zero while MangoHud was off, and there were no X errors. SIGINT shutdown was clean.
- Cost: probe CPU 1.0-1.4% while mirroring. mangoapp damages its full 4K window every frame, so the probe copied roughly 0.7-1.0 Gpixel/s on the GPU; restricting copies to the HUD bounds is an optimisation for the prototype. The box sits under the QAM, which is acceptable. Game-running and HDR behavior remain untested.

## End-to-End Prototype Smoke Test

- 2026-11-01, HTPC, Gamescope 3.16.25 (PID unchanged across all runs), HyperHDR stopped during runs and restored afterwards. Picture in Picture source: YouTube TV (`3609786786`) playing video via mpv.
- Gamescope first negotiated the previous reader's 512x288, then renegotiated to the requested 640x360 within the first second.
- Desktop apps in front (Steam Library, Media Dashboard): steady 60 buffers/s, ~0.4 ms per frame for copy + MIT-SHM upload + composite, 0.4-0.6% CPU (about 1% with the mangoapp mirror). The user confirmed smooth video in the box and MangoHud visible and updating alongside it. The box hid and reappeared correctly on every switch to and from YouTube TV. Moonlight in front reduced buffers to about 4/s with gaps up to 3 s (not investigated).
- Cyberpunk 2077 (`1091500`), same spot, MangoHud on, YouTube TV playing in the background:

  | Configuration | Game FPS |
  | --- | --- |
  | No Picture in Picture | 47 |
  | Full prototype | 42 |
  | Full prototype, mangoapp copies clipped to 1400x1200 | 43 |
  | Overlay and mirror only, no capture (`FLOATER_NO_CAPTURE`) | 46 |
  | Capture only, overlay unmapped (`FLOATER_NO_OVERLAY`) | 10 |
  | HyperHDR reader alone (512x288, no `focus_appid`) | 46 |

- With the overlay shown, the cost is about 4-5 FPS (~10%), mostly from capture rather than the overlay or mangoapp mirror. Picture in Picture video stayed smooth at ~60 buffers/s over the game.
- Anomaly: with `focus_appid` capture active and the overlay unmapped, the game dropped to 10 FPS even though the prototype spent only ~0.08 ms per frame (no X work). `paint_pipewire()` waits synchronously (`vulkan_wait`) for the capture render on the compositor thread; why that becomes so expensive only without our overlay is not established. The real product always shows the overlay while capturing, but this must be understood before shipping.
- SIGINT and duration stops drained cleanly every time; no Gamescope crash.

## Capture Cost Investigation

- Source (3.16.25 and current master): `paint_pipewire()` runs on the compositor thread at vblank, renders the target window with `vulkan_screenshot()` into the stream buffer, then blocks in `vulkan_wait()` until the GPU finishes. There is no asynchronous capture path upstream. Gamescope also offers linear single-plane DMA-BUF buffers, but that only avoids a ~0.9 MB CPU copy that already costs well under 1 ms; it does not remove the compositor wait. Gamescope only renders a capture when it can dequeue a free stream buffer and only when the target's commit changed (no frames while a video is paused).
- 10 FPS anomaly, 2026-11-02, Cyberpunk 2077 with `FLOATER_NO_OVERLAY`: still 10 FPS with `gamescopectl composite_force 1` (not a scanout/composition issue). Gamescope threads used ~1-2% CPU, so it is waiting rather than working. With MangoHud turned off the game was smooth again. The drop requires mangoapp to be the painted external overlay while a `focus_appid` capture of a background app runs; HyperHDR (no `focus_appid`) with MangoHud is unaffected. In the real design our overlay replaces mangoapp in the painted slot, so the combination does not occur. The exact Gamescope mechanism (mangoapp frame-time nudges are tied to focus-window commits) was not established.
- Throttling, same spot, MangoHud mirrored and clipped to 1400x1200: holding one buffer did not throttle (Gamescope allocates several); holding all dequeued buffers and returning one per interval did (exactly 30.0 buffers/s).

  | Picture in Picture rate | Game FPS | User verdict on PiP video |
  | --- | --- | --- |
  | Off | 47 | - |
  | 60 (uncapped) | 42-43 | smooth |
  | 30 | 45-46 | acceptable, slightly choppy with action; make it a setting |
  | 15 | 45 | too choppy |

- Conclusion: capture cost scales with captured frames per second; at 30 FPS the total cost is ~1-2 game FPS, matching the overlay-only cost (46 FPS). Default to 30 FPS with a user setting for 60.

## Controller Input Leak

- 2026-11-02: pressing A sometimes paused the YouTube TV video while it was the Picture in Picture source. Both its Firefox gamepad bridge and its mpv (`input-gamepad=yes`) hold `/dev/input/event22`, Steam's single shared virtual pad `Microsoft X-Box 360 pad 0`, and act on it regardless of window focus.
- An event logger on that device (A pressed in each place): Steam Library and QAM sent nothing to the pad (Steam consumes input); with Cyberpunk or YouTube TV in front every press reached the pad. The user saw the video pause only in those two cases. So any background app that reads the virtual pad receives the focused game's controller input.
- Grabbing the device (`EVIOCGRAB`) would also starve the focused game, which reads the same pad. There is no general non-root way to route the pad by focus; apps must ignore the pad when they are not focused (e.g. check `GAMESCOPE_FOCUSED_APP`), which is fixable for the HTPC's own YouTube TV/Plezy wrappers but not for arbitrary apps.

## Steam Game Recording

- Steam Game Recording is another consumer of the same Gamescope PipeWire node (`CDesktopCapturePipeWire`, node `steam`). On its own it sets `focus_appid` to the game and negotiates 3840x2160 NV12 MemFd (no DMA-BUF), then encodes with VAAPI H264; the user saw a heavy FPS hit from recording alone.
- With the prototype running first (30 FPS throttle, HyperHDR stopped), a manual recording joined the already negotiated 640x360 BGRx YouTube TV format instead of the game, and the held buffers starved it. After 33 s Steam logged `Driver deadlock in hardware accelerated desktop capture, aborting` and `Fatal assert; application exiting`; the Steam UI died and Cyberpunk exited. Gamescope survived; killing the stale Steam main process restored the session. Evidence: HTPC `~/Downloads/floater-prototype-2026-11-02/steam-recording-crash.journal`.
- Consequences: Picture in Picture and Steam recording are mutually exclusive. Holding buffers starves every consumer of the node, so throttling is only safe as the sole consumer. The plugin must refuse to start while another consumer is linked to the `gamescope` node (covers "Record in background", HyperHDR, Remote Play) and stop immediately when one appears.

## Decision Gate

Test platform: HTPC with Intel Arc B580, Gamescope 3.16.25, and Xwayland displays `:0`/`:1`.

Decision: NO-GO for Phase 1 under the current architecture. The prototype does not meet the requirements for a public plugin that must coexist with HyperHDR and MangoHud, and neither tested capture route currently supplies a confirmed sampleable background frame while preserving the other constraints.

Evidence:

- PipeWire `focus_appid` produced changing buffers from the non-focused YouTube TV AppID when used alone. With the existing HyperHDR reader active on the same Gamescope target, the second stream received zero buffers in the one controlled comparison.
- The `GAMESCOPE_EXTERNAL_OVERLAY` window was visible, passed controller input, and allowed Steam/QAM UI and notifications above it. Enabling MangoHud made the probe disappear; disabling MangoHud restored it.
- XComposite plus DRI3 exported DMA-BUFs from Firefox and mpv WSI windows, and GBM EGL created EGLImages. GLES `glEGLImageTargetTexture2DOES` returned `GL_INVALID_VALUE` (`0x501`) for both 2D and external texture targets; the direct 2D FBO was incomplete. Dynamic pixel sampling is not established.
- The PipeWire reader drained on SIGINT, reported 300 buffers, and did not crash Gamescope. Audio node/client/process mapping was resolved; `wpctl set-volume` succeeded at the unchanged 1.00 level.

Re-entry criteria: demonstrate a capture path that returns changing frames while HyperHDR remains active; demonstrate coexistence with MangoHud; and confirm sustained sampling performance, browser and WSI Vulkan behavior, and safe teardown. The tested audio mapping and volume permission can be reused. No plugin backend or UI implementation should start until a viable capture strategy is demonstrated.

Update 2026-11-01: HyperHDR coexistence is no longer required; Picture in Picture may own the Gamescope PipeWire stream while active. MangoHud coexistence is demonstrated by the mirror probe above. Remaining before Phase 1: an end-to-end PipeWire `focus_appid` to overlay prototype with sustained performance, focus swaps, and safe teardown.

Update 2026-11-01 (prototype): the end-to-end prototype works on the HTPC with smooth video, correct focus handling, MangoHud coexistence, and clean teardown, at a cost of about 10% game FPS in Cyberpunk 2077. Open items: the 10 FPS anomaly is narrowed to mangoapp being painted during a `focus_appid` capture (does not occur with our overlay shown; see Capture Cost Investigation); default capture to 30 FPS. Remaining: the Moonlight frame-rate drop, HyperHDR handoff automation, Steam Game Recording interaction, and audio control.
