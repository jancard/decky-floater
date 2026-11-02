import asyncio
import json
import os
import re
import shutil
from pathlib import Path

import decky

HELPER_NAME = "pip-overlay"
HYPERHDR_UNIT = "hyperhdr.service"
CORNERS = ("top-left", "top-right", "bottom-left", "bottom-right")
DEFAULT_SETTINGS = {
    "corner": "bottom-right",
    "scale": 0.25,
    "opacity": 1.0,
    "fps": 30,
    "border": 0,
    "pause_hyperhdr": True,
}
MAX_APPID = 2**32 - 1
DISPLAY_PATTERN = re.compile(r"^:\d+(\.\d+)?$")
APPID_CGROUP = re.compile(r"app-steam-app(\d+)-")
BUSY_RETRIES = 8


def _validate_appid(appid):
    if type(appid) is not int or not 0 < appid <= MAX_APPID:
        raise ValueError("appid must be a positive 32-bit integer")
    return appid


def validate_settings(changes):
    if not isinstance(changes, dict):
        raise ValueError("settings must be an object")
    unknown = set(changes) - set(DEFAULT_SETTINGS)
    if unknown:
        raise ValueError(f"unknown settings: {', '.join(sorted(unknown))}")
    checked = {}
    for key, value in changes.items():
        if key == "corner":
            if value not in CORNERS:
                raise ValueError("corner must be one of " + ", ".join(CORNERS))
        elif key in ("scale", "opacity"):
            low, high = (0.10, 0.50) if key == "scale" else (0.20, 1.00)
            if type(value) not in (int, float) or not low <= value <= high:
                raise ValueError(f"{key} must be between {low:.2f} and {high:.2f}")
            value = float(value)
        elif key == "fps":
            if type(value) is not int or not 1 <= value <= 60:
                raise ValueError("fps must be an integer between 1 and 60")
        elif key == "border":
            if type(value) is not int or not 0 <= value <= 20:
                raise ValueError("border must be an integer between 0 and 20")
        elif key == "pause_hyperhdr":
            if type(value) is not bool:
                raise ValueError("pause_hyperhdr must be a boolean")
        checked[key] = value
    return checked


def settings_arguments(settings):
    return (f"corner={settings['corner']} scale={settings['scale']:.3f} "
            f"opacity={settings['opacity']:.3f} fps={settings['fps']} border={settings['border']}")


def appid_for_pid(pid, proc="/proc"):
    """Steam launches every app in a systemd scope named after its AppID."""
    try:
        cgroup = Path(proc, str(pid), "cgroup").read_text()
    except OSError:
        return None
    match = APPID_CGROUP.search(cgroup)
    return int(match.group(1)) if match else None


def audio_nodes_for_app(dump, appid, proc="/proc"):
    # Streams usually carry their process id only on the PipeWire client that owns them.
    client_pids = {}
    for item in dump:
        if item.get("type") == "PipeWire:Interface:Client":
            props = (item.get("info") or {}).get("props") or {}
            pid = props.get("application.process.id") or props.get("pipewire.sec.pid")
            if pid is not None:
                client_pids[int(item["id"])] = int(pid)
    nodes = []
    for item in dump:
        if item.get("type") != "PipeWire:Interface:Node":
            continue
        props = (item.get("info") or {}).get("props") or {}
        if props.get("media.class") != "Stream/Output/Audio":
            continue
        pid = props.get("application.process.id")
        if pid is None and props.get("client.id") is not None:
            pid = client_pids.get(int(props["client.id"]))
        if pid is not None and appid_for_pid(int(pid), proc) == appid:
            nodes.append(int(item["id"]))
    return nodes


def read_display(runtime_dir, proc="/proc"):
    """The Steam session's X display: Gamescope's environment file, then Steam's own environment."""
    try:
        text = Path(runtime_dir, "gamescope-environment").read_text()
        match = re.search(r"^DISPLAY=(\S+)$", text, re.M)
        if match and DISPLAY_PATTERN.match(match.group(1)):
            return match.group(1)
    except OSError:
        pass
    for entry in Path(proc).iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if (entry / "comm").read_text().strip() != "steam":
                continue
            environ = (entry / "environ").read_bytes().split(b"\0")
        except OSError:
            continue
        for variable in environ:
            if variable.startswith(b"DISPLAY="):
                display = variable[8:].decode(errors="replace")
                if DISPLAY_PATTERN.match(display):
                    return display
    return ":0"


class Helper:
    """The native pip-overlay process: commands on stdin, JSON events on stdout."""

    def __init__(self, path, environment, on_event):
        self._path = path
        self._environment = environment
        self._on_event = on_event
        self._process = None
        self._reader = None
        self._logger_task = None
        self._waiters = []

    @property
    def alive(self):
        return self._process is not None and self._process.returncode is None

    async def start(self):
        self._process = await asyncio.create_subprocess_exec(
            self._path, env=self._environment,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        ready = self._wait_for({"ready"})
        self._reader = asyncio.create_task(self._read_events())
        self._logger_task = asyncio.create_task(self._read_log())
        try:
            return await asyncio.wait_for(ready, timeout=10)
        except asyncio.TimeoutError:
            await self.stop()
            raise RuntimeError("Picture in Picture helper did not start") from None

    def _wait_for(self, events):
        future = asyncio.get_running_loop().create_future()
        self._waiters.append((events, future))
        return future

    async def request(self, command, events, timeout=5):
        if not self.alive:
            raise RuntimeError("Picture in Picture helper is not running")
        future = self._wait_for(events)
        await self.send(command)
        try:
            return await asyncio.wait_for(future, timeout=timeout)
        except asyncio.TimeoutError:
            raise RuntimeError(f"Picture in Picture helper did not answer '{command.split()[0]}'") from None

    async def send(self, command):
        if not self.alive:
            raise RuntimeError("Picture in Picture helper is not running")
        self._process.stdin.write(command.encode() + b"\n")
        await self._process.stdin.drain()

    async def _read_events(self):
        async for line in self._process.stdout:
            try:
                event = json.loads(line)
            except ValueError:
                decky.logger.warning("helper sent invalid output: %r", line[:200])
                continue
            for waiter in list(self._waiters):
                events, future = waiter
                if event.get("event") in events:
                    self._waiters.remove(waiter)
                    if not future.done():
                        future.set_result(event)
                    break
            await self._on_event(event)
        for _, future in self._waiters:
            if not future.done():
                future.set_exception(RuntimeError("Picture in Picture helper exited"))
        self._waiters.clear()
        await self._on_event({"event": "exited"})

    async def _read_log(self):
        async for line in self._process.stderr:
            decky.logger.info("helper: %s", line.decode(errors="replace").rstrip())

    async def stop(self):
        if self._process is None:
            return
        if self.alive:
            try:
                self._process.stdin.write(b"quit\n")
                await self._process.stdin.drain()
                await asyncio.wait_for(self._process.wait(), timeout=5)
            except (asyncio.TimeoutError, ConnectionError):
                self._process.kill()
                await self._process.wait()
        for task in (self._reader, self._logger_task):
            if task is not None:
                await asyncio.gather(task, return_exceptions=True)
        self._process = None


class Plugin:
    async def _main(self):
        user_id = os.getuid()
        if user_id == 0:
            raise RuntimeError("Floater must run without the Decky root flag")
        self._lock = asyncio.Lock()
        self._runtime = f"/run/user/{user_id}"
        self._environment = os.environ.copy()
        self._environment.update(
            XDG_RUNTIME_DIR=self._runtime,
            DBUS_SESSION_BUS_ADDRESS=f"unix:path={self._runtime}/bus",
        )
        self._settings_path = Path(decky.DECKY_PLUGIN_SETTINGS_DIR, "settings.json")
        self._settings = self._load_settings()
        self._helper = None
        self._paused_hyperhdr = False
        self._state = {"active": False, "appid": None, "visible": False, "streaming": False,
                       "reason": None, "stats": None}

    def _load_settings(self):
        settings = dict(DEFAULT_SETTINGS)
        try:
            settings.update(validate_settings(json.loads(self._settings_path.read_text())))
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as error:
            decky.logger.warning("ignoring invalid settings: %s", error)
        return settings

    def _save_settings(self):
        self._settings_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self._settings_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(self._settings, indent=2))
        temporary.replace(self._settings_path)

    async def _run(self, *command, timeout=10):
        process = await asyncio.create_subprocess_exec(
            *command, env=self._environment,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
        try:
            output, errors = await asyncio.wait_for(process.communicate(), timeout=timeout)
        except asyncio.TimeoutError:
            process.kill()
            await process.communicate()
            raise RuntimeError(f"{Path(command[0]).name} timed out") from None
        if process.returncode != 0:
            raise RuntimeError(errors.decode().strip() or f"{Path(command[0]).name} failed")
        return output.decode()

    async def _hyperhdr_active(self):
        try:
            output = await self._run("/usr/bin/systemctl", "--user", "is-active", HYPERHDR_UNIT)
        except RuntimeError:
            return False
        return output.strip() in ("active", "activating")

    async def _restore_hyperhdr(self):
        if not self._paused_hyperhdr:
            return
        self._paused_hyperhdr = False
        try:
            await self._run("/usr/bin/systemctl", "--user", "start", HYPERHDR_UNIT, timeout=25)
            decky.logger.info("restarted %s after Picture in Picture", HYPERHDR_UNIT)
        except RuntimeError as error:
            decky.logger.error("could not restart %s: %s", HYPERHDR_UNIT, error)

    async def _ensure_helper(self):
        if self._helper is not None and self._helper.alive:
            return
        environment = dict(self._environment, DISPLAY=read_display(self._runtime))
        self._helper = Helper(self._helper_path(), environment, self._on_event)
        ready = await self._helper.start()
        if not ready.get("gamescope_stream"):
            decky.logger.warning("Gamescope's PipeWire stream is not available yet")

    def _helper_path(self):
        installed = Path(decky.DECKY_PLUGIN_DIR, "bin", HELPER_NAME)
        if os.access(installed, os.X_OK):
            return str(installed)
        # ZIP installs can drop the executable bit, and the plugin directory is root-owned.
        private = Path(decky.DECKY_PLUGIN_RUNTIME_DIR, HELPER_NAME)
        private.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(installed, private)
        private.chmod(0o755)
        return str(private)

    async def _on_event(self, event):
        kind = event.get("event")
        if kind == "visible":
            self._state["visible"] = bool(event.get("value"))
        elif kind == "streaming":
            self._state["streaming"] = True
        elif kind == "stats":
            self._state["stats"] = {key: event.get(key) for key in
                                    ("buffers_per_second", "presented", "cpu_percent")}
        elif kind in ("stopped", "exited"):
            reason = event.get("reason", "helper-exited")
            was_active = self._state["active"]
            self._state.update(active=False, visible=False, streaming=False, stats=None,
                               reason=reason)
            if was_active and reason not in ("hidden", "quit"):
                asyncio.create_task(self._restore_hyperhdr())
        elif kind == "error":
            decky.logger.error("helper error: %s", event.get("message"))
            return
        else:
            return
        await self._emit_state()

    async def _emit_state(self):
        try:
            await decky.emit("floater_state", self.get_state_sync())
        except Exception as error:
            decky.logger.debug("could not emit state: %s", error)

    def get_state_sync(self):
        return dict(self._state, settings=dict(self._settings),
                    paused_hyperhdr=self._paused_hyperhdr)

    async def get_state(self):
        return self.get_state_sync()

    async def get_apps(self):
        async with self._lock:
            await self._ensure_helper()
            event = await self._helper.request("apps", {"apps"})
            return {"focused": event.get("focused") or None, "running": event.get("running", [])}

    async def start(self, appid):
        _validate_appid(appid)
        async with self._lock:
            await self._ensure_helper()
            if self._state["active"] and self._state["appid"] == appid:
                await self._helper.send(f"set {settings_arguments(self._settings)}")
                return self.get_state_sync()
            command = f"show appid={appid} {settings_arguments(self._settings)}"
            for attempt in range(BUSY_RETRIES + 1):
                event = await self._helper.request(command, {"starting", "stopped"})
                if event["event"] == "starting":
                    self._state.update(active=True, appid=appid, reason=None, streaming=False)
                    await self._emit_state()
                    return self.get_state_sync()
                if event.get("reason") != "capture-busy" or attempt == BUSY_RETRIES:
                    break
                if (attempt == 0 and self._settings["pause_hyperhdr"] and not self._paused_hyperhdr
                        and await self._hyperhdr_active()):
                    await self._run("/usr/bin/systemctl", "--user", "stop", HYPERHDR_UNIT, timeout=25)
                    self._paused_hyperhdr = True
                    decky.logger.info("paused %s for Picture in Picture", HYPERHDR_UNIT)
                elif not self._paused_hyperhdr:
                    break
                await asyncio.sleep(0.5)
            await self._restore_hyperhdr()
            reason = event.get("reason", "unknown")
            self._state.update(active=False, reason=reason)
            await self._emit_state()
            if reason == "capture-busy":
                raise RuntimeError("Another app is using Gamescope's capture stream "
                                   "(for example Steam Game Recording)")
            raise RuntimeError(f"Picture in Picture could not start: {reason}")

    async def stop(self):
        async with self._lock:
            if self._helper is not None and self._helper.alive and self._state["active"]:
                await self._helper.request("hide", {"stopped"})
            self._state.update(active=False, visible=False, streaming=False, stats=None)
            await self._restore_hyperhdr()
            await self._emit_state()
            return self.get_state_sync()

    async def set_settings(self, changes):
        checked = validate_settings(changes)
        async with self._lock:
            self._settings.update(checked)
            self._save_settings()
            helper_keys = {key: checked[key] for key in ("corner", "scale", "opacity", "fps", "border")
                           if key in checked}
            if helper_keys and self._state["active"] and self._helper is not None:
                pairs = " ".join(f"{key}={value:.3f}" if isinstance(value, float) else f"{key}={value}"
                                 for key, value in helper_keys.items())
                await self._helper.send(f"set {pairs}")
            return self.get_state_sync()

    async def _app_audio_nodes(self, appid):
        dump = json.loads(await self._run("/usr/bin/pw-dump"))
        nodes = audio_nodes_for_app(dump, appid)
        if not nodes:
            raise RuntimeError("The Picture in Picture app has no audio playing")
        return nodes

    async def set_volume(self, appid, volume):
        _validate_appid(appid)
        if type(volume) is not int or not 0 <= volume <= 150:
            raise ValueError("volume must be an integer between 0 and 150")
        nodes = await self._app_audio_nodes(appid)
        for node in nodes:
            await self._run("/usr/bin/wpctl", "set-volume", str(node), f"{volume / 100:.2f}")
        return {"streams": len(nodes), "volume": volume}

    async def set_muted(self, appid, muted):
        _validate_appid(appid)
        if type(muted) is not bool:
            raise ValueError("muted must be a boolean")
        nodes = await self._app_audio_nodes(appid)
        for node in nodes:
            await self._run("/usr/bin/wpctl", "set-mute", str(node), "1" if muted else "0")
        return {"streams": len(nodes), "muted": muted}

    async def get_audio(self, appid):
        _validate_appid(appid)
        nodes = await self._app_audio_nodes(appid)
        output = await self._run("/usr/bin/wpctl", "get-volume", str(nodes[0]))
        match = re.search(r"Volume:\s*([\d.]+)", output)
        return {"streams": len(nodes),
                "volume": round(float(match.group(1)) * 100) if match else None,
                "muted": "[MUTED]" in output}

    async def _unload(self):
        if self._helper is not None:
            await self._helper.stop()
        await self._restore_hyperhdr()
