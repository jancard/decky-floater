# Phase 0 Spike

Status: not run. Record commands, observations, and evidence here; do not treat an unverified capture path as selected.

## Experiments

1. Verify Gamescope `focus_appid` on a copy of the existing PipeWire capture helper, using a non-focused app and moving video.
2. Test an ARGB X11 window tagged `GAMESCOPE_EXTERNAL_OVERLAY=1` with an empty input shape over a game and Steam UI, including controller input and overlay/QAM/notification interactions.
3. Repeat with the HyperHDR PipeWire reader active and record whether both consumers receive usable frames.
4. If PipeWire focus capture is unsuitable, test XComposite plus DRI3 capture with a browser and a WSI Vulkan app.
5. Confirm an unfocused app continues audio/video playback and determine whether its PipeWire audio stream can be controlled as the plugin user.
6. Verify drain-before-disconnect shutdown behavior for every capture path.

## Decision Gate

Record a go/no-go decision and the selected capture strategy only after the experiments above. Include hardware, Gamescope version, commands, observed behavior, and relevant logs without copying secrets or unrelated machine state.
