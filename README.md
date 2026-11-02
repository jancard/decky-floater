# Floater

Picture in Picture for Steam Game Mode. Keep a video or app playing in a corner of the screen while you use another app or play a game.

Floater is a [Decky Loader](https://github.com/SteamDeckHomebrew/decky-loader) plugin.

## Features

- Shows any running Steam app (including non-Steam shortcuts) in a corner while another app is in front.
- Switch the Picture in Picture app from the Quick Access Menu without closing it first.
- Hides the picture automatically while its own app is in front, and shows it again when you switch away.
- Picture in Picture audio controls: volume (0–130%) and mute, without changing the app's own settings.
- Layout settings: corner, size (10–50% of the screen), opacity, border, and an optional 60 FPS mode.
- Works alongside MangoHud's performance overlay.
- No system changes: no root access, no Gamescope or Steam patches.

## Requirements

- Decky Loader 3.x.
- Steam Game Mode (Gamescope with its PipeWire capture stream), as on SteamOS 3.x and similar Game Mode sessions.
- Tested with Gamescope 3.16.25 on desktop Game Mode. Steam Deck testing is in progress.

## Installation

### Decky Plugin Store

Not yet available. Once listed, open the Quick Access Menu, go to Decky → Store, and install **Floater**.

### Manual install

1. Download `floater-vX.Y.Z.zip` from the releases page.
2. In Game Mode, open Decky's settings, turn on **Developer mode**, then go to **Developer → Install Plugin from ZIP** and choose the file.

## Usage

1. Start the app you want to watch, for example a video player or a streaming app.
2. Open the Quick Access Menu (`…` button) and select **Floater**.
3. Choose the app in the **App** list and select **Start Picture in Picture**.
4. If that app is in front, Floater switches to the Library so the picture becomes visible. Start another app or game and the picture stays in its corner.

While Picture in Picture is running, choose a different app in the list to switch to it, or select **Close Picture in Picture** to stop. Closing restores the app's audio if Floater muted it.

## Settings

| Setting | Description |
| --- | --- |
| Volume / Mute | Changes only the Picture in Picture app's audio. Mute is undone when Picture in Picture closes. |
| Corner | Which corner the picture sits in. |
| Size | Height of the picture as a share of the screen. |
| Opacity | Transparency of the picture. |
| Border | Thin frame around the picture, in pixels at 1080p. |
| Smooth video (60 FPS) | Updates the picture at 60 instead of 30 frames per second. Uses more GPU time while a game runs. |

## Limitations

- **One capture at a time.** Gamescope has a single capture stream. Steam Game Recording (including *Record in background*) and Steam Remote Play can use the same stream, so Floater won't start while they are active, and stops if one starts.
- **Performance.** Gamescope renders the capture on its compositor thread, so demanding games can lose some frames per second while Picture in Picture is running. The default 30 FPS keeps the cost low.
- **Controller input.** Steam sends controller input to the app in front. Apps that read the controller themselves may still react in the background; that depends on the app.
- **Steam apps only.** The app must be started through Steam, as a game or a non-Steam shortcut.

## Troubleshooting

- *"Another app is using Gamescope's capture stream"*: turn off Steam Game Recording's background recording, or end any Remote Play session.
- *"The Picture in Picture app closed"*: the app exited or Steam no longer lists it as running.
- Logs are in `~/homebrew/logs/Floater/`.

## How it works

The Python backend (`main.py`) runs a small native helper (`backend/src`). The helper asks Gamescope's PipeWire stream for frames of a chosen app (`focus_appid`) at a reduced size and draws them in an X11 overlay window that Gamescope composites as an external overlay. Gamescope draws only one external overlay, so the helper also mirrors MangoHud's overlay. Audio controls use WirePlumber's `wpctl` on the app's PipeWire streams.

## Development

Requirements: Node.js 20+, pnpm 9, Python 3, a C compiler, `pkg-config`, and PipeWire and X11 development headers.

```sh
pnpm install
pnpm test           # Python and helper unit tests
pnpm run package    # builds dist/, the helper, and release/floater-vX.Y.Z.zip
```

The Decky Plugin Store builds the helper with `backend/Dockerfile` in the SteamOS `holo-base` image. CI does the same (`scripts/ci.sh`).

### Continuous integration

Forgejo Actions workflows live in `.forgejo/workflows/` and need a runner with the `docker` label:

- **CI**: tests, builds and packages every push and pull request, and keeps the ZIP as an artifact.
- **Release**: on a `vX.Y.Z` tag that matches `package.json`, builds the ZIP and its SHA-256 and attaches them to a draft release. The draft is published by hand after device testing. It uses the job token, or a `RELEASE_TOKEN` secret if the token can't create releases.
- **Upstream compatibility**: every Monday, checks the newest Gamescope and MangoHud tags for the interfaces Floater depends on (`scripts/compat.json`), and rebuilds against the current SteamOS image and the newest Decky libraries.

### Maintenance

Floater relies on Gamescope and Steam behaviour that isn't a public API. After updates, check:

| Change | What can break | Where |
| --- | --- | --- |
| Gamescope | PipeWire node name `gamescope`, format keys `requested_size` (0x70000) and `focus_appid` (0x70001), BGRx frames; X11 properties `GAMESCOPE_FOCUSED_APP`, `GAMESCOPE_FOCUSABLE_APPS`, `GAMESCOPE_EXTERNAL_OVERLAY`; one external overlay drawn at a time | `backend/src/pip-overlay.c`, `scripts/compat.json` |
| MangoHud | `mangoapp` window title, used to mirror its overlay | `backend/src/pip-overlay.c` |
| Steam client | `SteamUIStore.MainRunningAppID`, `appStore.GetAppOverviewByAppID`, Library route | `src/index.tsx` |
| Decky Loader | `@decky/api`, `@decky/ui`, plugin environment variables | `src/index.tsx`, `main.py` |
| SteamOS | PipeWire and X11 libraries the helper links against | `backend/Dockerfile` |

When the compatibility workflow reports a newer Gamescope or MangoHud, test Floater on a device with it, then update `verified` in `scripts/compat.json`.

### Releasing

1. Update `version` in `package.json` and commit.
2. Tag the commit `vX.Y.Z` and push the tag. CI creates a draft release with the ZIP.
3. Install the ZIP on a Steam Deck and a desktop Game Mode setup, then publish the draft.
4. For the Decky Plugin Store, update Floater's entry in [decky-plugin-database](https://github.com/SteamDeckHomebrew/decky-plugin-database) to the new commit.

## License

Floater is Copyright (C) 2026 moderntimes and licensed under the GNU General Public License v3.0 or later. See [LICENSE](LICENSE), which also contains the BSD 3-Clause notice of the Decky plugin template. Third-party licences are listed in [THIRD_PARTY_NOTICES.md](defaults/THIRD_PARTY_NOTICES.md).

## Credits

- [Decky Loader](https://github.com/SteamDeckHomebrew/decky-loader) and the [Decky plugin template](https://github.com/SteamDeckHomebrew/decky-plugin-template) by Steam Deck Homebrew.
- [Gamescope](https://github.com/ValveSoftware/gamescope) by Valve.
- Picture in Picture icon from [Material Design Icons](https://github.com/google/material-design-icons) via [react-icons](https://github.com/react-icons/react-icons).
