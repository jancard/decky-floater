import { addEventListener, callable, definePlugin, removeEventListener, toaster } from "@decky/api";
import {
  ButtonItem,
  DropdownItem,
  Navigation,
  PanelSection,
  PanelSectionRow,
  SliderField,
  ToggleField,
  staticClasses,
} from "@decky/ui";
import { useEffect, useRef, useState } from "react";
import { MdPictureInPictureAlt } from "react-icons/md";

type Corner = "top-left" | "top-right" | "bottom-left" | "bottom-right";

interface Settings {
  corner: Corner;
  scale: number;
  opacity: number;
  fps: number;
  border: number;
  pause_hyperhdr: boolean;
}

interface State {
  active: boolean;
  appid: number | null;
  visible: boolean;
  streaming: boolean;
  reason: string | null;
  paused_hyperhdr: boolean;
  settings: Settings;
}

interface Apps {
  focused: number | null;
  running: number[];
}

interface Audio {
  streams: number;
  volume: number | null;
  muted: boolean;
}

const getState = callable<[], State>("get_state");
const getApps = callable<[], Apps>("get_apps");
const startPip = callable<[appid: number], State>("start");
const stopPip = callable<[], State>("stop");
const setSettings = callable<[changes: Partial<Settings>], State>("set_settings");
const getAudio = callable<[appid: number], Audio>("get_audio");
const setVolume = callable<[appid: number, volume: number], unknown>("set_volume");
const setMuted = callable<[appid: number, muted: boolean], unknown>("set_muted");

const STEAM_APPID = 769;
const CORNERS: { data: Corner; label: string }[] = [
  { data: "top-left", label: "Top left" },
  { data: "top-right", label: "Top right" },
  { data: "bottom-left", label: "Bottom left" },
  { data: "bottom-right", label: "Bottom right" },
];
const STOP_REASONS: Record<string, string> = {
  "source-closed": "The Picture in Picture app closed",
  "capture-busy": "Another app started using the capture stream (for example Steam Game Recording)",
  "gamescope-stream-missing": "Gamescope's capture stream is not available",
  "stream-error": "The capture stream failed",
  "unsupported-format": "Gamescope offered an unsupported video format",
  "shared-memory-failed": "Could not allocate the video buffer",
  "display-closed": "The display session ended",
  "pipewire-lost": "Lost the connection to PipeWire",
  "helper-exited": "The Picture in Picture helper stopped",
};

const message = (failure: unknown) => (failure instanceof Error ? failure.message : String(failure));
const appName = (appid: number) =>
  window.appStore?.GetAppOverviewByAppID(appid)?.display_name || `App ${appid}`;

// The panel re-mounts when the dropdown's menu closes, so keep the choice outside React state.
let lastSelected: number | null = null;

// Sends only the latest value while a call is in flight, so sliders don't queue up backend calls.
function useLatest<T>(send: (value: T) => Promise<unknown>, onError: (text: string) => void) {
  const pending = useRef<T | null>(null);
  const running = useRef(false);
  return async (value: T) => {
    pending.current = value;
    if (running.current) return;
    running.current = true;
    try {
      while (pending.current !== null) {
        const next = pending.current;
        pending.current = null;
        await send(next);
      }
    } catch (failure) {
      pending.current = null;
      onError(message(failure));
    } finally {
      running.current = false;
    }
  };
}

function Content() {
  const [state, setState] = useState<State | null>(null);
  const [apps, setApps] = useState<Apps | null>(null);
  const [selected, setSelected] = useState<number | null>(null);
  const [audio, setAudio] = useState<Audio | null>(null);
  const [audioError, setAudioError] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    const onState = (next: State) => {
      if (mounted.current) setState(next);
    };
    addEventListener("floater_state", onState);
    const load = async () => {
      try {
        const [current, running] = await Promise.all([getState(), getApps()]);
        if (!mounted.current) return;
        setState(current);
        setApps(running);
        const candidates = running.running.filter((appid) => appid !== STEAM_APPID);
        const front = window.SteamUIStore?.MainRunningAppID ?? running.focused;
        const preferred = front && candidates.includes(front) ? front : candidates[0];
        const remembered = lastSelected !== null && candidates.includes(lastSelected) ? lastSelected : null;
        setSelected(current.active && current.appid ? current.appid : remembered ?? preferred ?? null);
      } catch (failure) {
        if (mounted.current) setError(message(failure));
      }
    };
    void load();
    return () => {
      mounted.current = false;
      removeEventListener("floater_state", onState);
    };
  }, []);

  const pipAppid = state?.active ? state.appid : null;
  useEffect(() => {
    setAudio(null);
    setAudioError(null);
    if (pipAppid === null) return;
    getAudio(pipAppid)
      .then((result) => mounted.current && setAudio(result))
      .catch((failure) => mounted.current && setAudioError(message(failure)));
  }, [pipAppid]);

  const run = async (action: () => Promise<State>, leaveAppid: number | null) => {
    if (busy) return;
    setBusy(true);
    setError(null);
    // Gamescope reports Steam as focused while the QAM is open; Steam knows which app is behind it.
    const front = window.SteamUIStore?.MainRunningAppID;
    try {
      const next = await action();
      if (mounted.current) setState(next);
      // The box stays hidden while its app is in front, so only then switch to the Library.
      // Choosing from the dropdown remounts the panel, so this must not depend on `mounted`.
      const inFront = front !== undefined ? front === leaveAppid : (await getApps()).focused === leaveAppid;
      if (leaveAppid !== null && inFront) {
        Navigation.Navigate("/library/home");
        Navigation.CloseSideMenus();
      }
    } catch (failure) {
      const text = message(failure);
      if (mounted.current) setError(text);
      toaster.toast({ title: "Picture in Picture", body: text });
    } finally {
      if (mounted.current) setBusy(false);
    }
  };

  const changeSettings = useLatest<Partial<Settings>>(async (changes) => {
    const next = await setSettings(changes);
    if (mounted.current) setState(next);
  }, (text) => toaster.toast({ title: "Picture in Picture", body: text }));

  const changeVolume = useLatest<number>(async (volume) => {
    if (pipAppid !== null) await setVolume(pipAppid, volume);
  }, (text) => mounted.current && setAudioError(text));

  const toggleMute = async (muted: boolean) => {
    if (pipAppid === null) return;
    setAudio((current) => (current ? { ...current, muted } : current));
    try {
      await setMuted(pipAppid, muted);
    } catch (failure) {
      if (mounted.current) setAudioError(message(failure));
    }
  };

  const settings = state?.settings;
  const candidates = (apps?.running ?? []).filter((appid) => appid !== STEAM_APPID);
  const stopNote = !state?.active && state?.reason ? STOP_REASONS[state.reason] : undefined;
  const choose = (appid: number) => {
    lastSelected = appid;
    setSelected(appid);
    if (state?.active && appid !== state.appid) void run(() => startPip(appid), appid);
  };
  const status = error ?? stopNote ?? (
    !state ? "Loading..."
    : busy && state.active ? "Switching..."
    : state.active ? (state.visible ? "Showing" : "Hidden while the app is in front")
      + (state.paused_hyperhdr ? " · HyperHDR paused until closed" : "")
    : candidates.length === 0 ? "Start an app, then come back here"
    : undefined
  );

  return (
    <>
      <PanelSection title="Picture in Picture">
        <PanelSectionRow>
          <DropdownItem
            label="App"
            layout="below"
            menuLabel="Picture in Picture app"
            disabled={busy || candidates.length === 0}
            rgOptions={candidates.map((appid) => ({ data: appid, label: appName(appid) }))}
            selectedOption={pipAppid ?? selected}
            onChange={(option) => choose(option.data as number)}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          {state?.active ? (
            <ButtonItem layout="below" description={status} disabled={busy}
              onClick={() => void run(() => stopPip(), null)}>
              Close Picture in Picture
            </ButtonItem>
          ) : (
            <ButtonItem layout="below" description={status} disabled={busy || selected === null}
              onClick={() => selected !== null && void run(() => startPip(selected), selected)}>
              {busy ? "Starting..." : "Start Picture in Picture"}
            </ButtonItem>
          )}
        </PanelSectionRow>
      </PanelSection>
      {state?.active && (
        <PanelSection title="Picture in Picture audio">
          <PanelSectionRow>
            <SliderField
              label="Volume"
              description={audioError ?? undefined}
              value={Math.min(audio?.volume ?? 100, 130)}
              min={0}
              max={130}
              step={5}
              minimumDpadGranularity={5}
              resetValue={100}
              showValue
              valueSuffix="%"
              disabled={audio === null}
              onChange={(volume) => {
                setAudio((current) => (current ? { ...current, volume } : current));
                void changeVolume(volume);
              }}
            />
          </PanelSectionRow>
          <PanelSectionRow>
            <ToggleField label="Mute" checked={audio?.muted ?? false} disabled={audio === null}
              onChange={(muted) => void toggleMute(muted)} />
          </PanelSectionRow>
        </PanelSection>
      )}
      {settings && (
        <PanelSection title="Layout">
          <PanelSectionRow>
            <DropdownItem
              label="Corner"
              rgOptions={CORNERS}
              selectedOption={settings.corner}
              onChange={(option) => void changeSettings({ corner: option.data as Corner })}
            />
          </PanelSectionRow>
          <PanelSectionRow>
            <SliderField
              label="Size"
              value={Math.round(settings.scale * 100)}
              min={10}
              max={50}
              step={5}
              showValue
              valueSuffix="%"
              onChange={(value) => {
                setState({ ...state!, settings: { ...settings, scale: value / 100 } });
                void changeSettings({ scale: value / 100 });
              }}
            />
          </PanelSectionRow>
          <PanelSectionRow>
            <SliderField
              label="Opacity"
              value={Math.round(settings.opacity * 100)}
              min={20}
              max={100}
              step={5}
              showValue
              valueSuffix="%"
              onChange={(value) => {
                setState({ ...state!, settings: { ...settings, opacity: value / 100 } });
                void changeSettings({ opacity: value / 100 });
              }}
            />
          </PanelSectionRow>
          <PanelSectionRow>
            <SliderField
              label="Border"
              value={settings.border}
              min={0}
              max={10}
              step={1}
              resetValue={0}
              showValue
              valueSuffix=" px"
              onChange={(value) => {
                setState({ ...state!, settings: { ...settings, border: value } });
                void changeSettings({ border: value });
              }}
            />
          </PanelSectionRow>
          <PanelSectionRow>
            <ToggleField
              label="Smooth video (60 FPS)"
              description="Uses more GPU time while a game runs"
              checked={settings.fps >= 60}
              onChange={(smooth) => void changeSettings({ fps: smooth ? 60 : 30 })}
            />
          </PanelSectionRow>
        </PanelSection>
      )}
    </>
  );
}

export default definePlugin(() => ({
  name: "Floater",
  titleView: <div className={staticClasses.Title}>Floater</div>,
  content: <Content />,
  icon: <MdPictureInPictureAlt />,
  onDismount() {},
}));
