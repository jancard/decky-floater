import asyncio
import json
import os
import re
import shutil
from pathlib import Path

import decky

HELPER_NAME = "pip-overlay"
CORNERS = ("top-left", "top-right", "bottom-left", "bottom-right")
DEFAULT_SETTINGS = {
    "corner": "bottom-left",
    "scale": 0.30,
    "opacity": 1.0,
    "fps": 30,
    "border": 1,
    "pause_services": [],
}
MAX_APPID = 2**32 - 1
DISPLAY_PATTERN = re.compile(r"^:\d+(\.\d+)?$")
APPID_CGROUP = re.compile(r"app-steam-app(\d+)-")
FLATPAK_CGROUP = re.compile(r"app-flatpak-([A-Za-z0-9_.-]+)-\d+\.scope")
FLATPAK_ID = re.compile(r"^[A-Za-z_][\w-]*(\.[A-Za-z_][\w-]*){2,}$")
SERVICE_NAME = re.compile(r"^[\w@.:-]+\.service$")
USER_SERVICE_CGROUP = re.compile(
    r"^0::(/user\.slice/user-\d+\.slice/user@\d+\.service/app\.slice/([\w@.:-]+\.service))$", re.M)
# Never offer to pause the game session itself.
PROTECTED_PROCESSES = {"steam", "steamwebhelper", "gamescope", "gamescope-wl", "Xwayland"}
MAX_PAUSE_SERVICES = 8
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
        elif key == "pause_services":
            if (not isinstance(value, list) or len(value) > MAX_PAUSE_SERVICES or len(set(value)) != len(value)
                    or not all(isinstance(unit, str) and SERVICE_NAME.match(unit) for unit in value)):
                raise ValueError(f"pause_services must be up to {MAX_PAUSE_SERVICES} distinct .service names")
            value = list(value)
        checked[key] = value
    return checked


def settings_arguments(settings):
    return (f"corner={settings['corner']} scale={settings['scale']:.3f} "
            f"opacity={settings['opacity']:.3f} fps={settings['fps']} border={settings['border']}")


def _cgroup(pid, proc):
    try:
        return Path(proc, str(pid), "cgroup").read_text()
    except OSError:
        return ""


def appid_for_pid(pid, proc="/proc"):
    """Steam launches every app in a systemd scope named after its AppID."""
    match = APPID_CGROUP.search(_cgroup(pid, proc))
    return int(match.group(1)) if match else None


def flatpaks_for_app(appid, proc="/proc"):
    """Flatpak apps move into their own scope, but Steam's launcher keeps `flatpak run ID` in its command line."""
    ids = set()
    for entry in Path(proc).iterdir():
        if not entry.name.isdigit() or appid_for_pid(entry.name, proc) != appid:
            continue
        try:
            args = [arg.decode(errors="replace") for arg in Path(entry, "cmdline").read_bytes().split(b"\0")]
        except OSError:
            continue
        for index, arg in enumerate(args[:-1]):
            if Path(arg).name == "flatpak" and args[index + 1] == "run":
                ids.update(name for name in args[index + 2:] if FLATPAK_ID.match(name))
    return ids


def node_pids(dump):
    """Candidate process ids per PipeWire node.

    Nodes usually carry their process id only on the PipeWire client that owns them, and
    sandboxed apps report ids from their own PID namespace, so collect every candidate.
    """
    client_pids = {}
    for item in dump:
        if item.get("type") == "PipeWire:Interface:Client":
            props = (item.get("info") or {}).get("props") or {}
            client_pids[int(item["id"])] = [props.get("pipewire.sec.pid"), props.get("application.process.id")]
    nodes = {}
    for item in dump:
        if item.get("type") != "PipeWire:Interface:Node":
            continue
        props = (item.get("info") or {}).get("props") or {}
        candidates = [props.get("application.process.id")]
        if props.get("client.id") is not None:
            candidates += client_pids.get(int(props["client.id"]), [])
        nodes[int(item["id"])] = (props, {int(pid) for pid in candidates if pid is not None})
    return nodes


def audio_nodes_for_app(dump, appid, proc="/proc"):
    flatpaks = None
    nodes = []
    for node, (props, pids) in node_pids(dump).items():
        if props.get("media.class") != "Stream/Output/Audio":
            continue
        for pid in pids:
            if appid_for_pid(pid, proc) == appid:
                nodes.append(node)
                break
            match = FLATPAK_CGROUP.search(_cgroup(pid, proc))
            if match:
                if flatpaks is None:
                    flatpaks = flatpaks_for_app(appid, proc)
                if match.group(1) in flatpaks:
                    nodes.append(node)
                    break
    return nodes


def user_service_for_pid(pid, proc="/proc", cgroup_root="/sys/fs/cgroup"):
    """The systemd user service a process runs in, unless that service is part of the game session."""
    match = USER_SERVICE_CGROUP.search(_cgroup(pid, proc))
    if not match or match.group(2).startswith(("steam", "gamescope-session")):
        return None
    try:
        members = Path(cgroup_root + match.group(1), "cgroup.procs").read_text().split()
    except OSError:
        return None
    for member in members:
        try:
            if Path(proc, member, "comm").read_text().strip() in PROTECTED_PROCESSES:
                return None
        except OSError:
            continue
    return match.group(2)


def service_for_node(dump, node, proc="/proc", cgroup_root="/sys/fs/cgroup"):
    _, pids = node_pids(dump).get(node, ({}, set()))
    for pid in sorted(pids):
        unit = user_service_for_pid(pid, proc, cgroup_root)
        if unit is not None:
            return unit
    return None


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
        self._paused_services = []
        self._busy_service = None
        self._muted_appid = None
        self._state = {"active": False, "appid": None, "visible": False, "streaming": False,
                       "reason": None, "stats": None}

    def _load_settings(self):
        settings = dict(DEFAULT_SETTINGS)
        try:
            saved = json.loads(self._settings_path.read_text())
            if not isinstance(saved, dict):
                raise ValueError("settings must be an object")
            # Settings from older versions are dropped, not allowed to discard the rest.
            settings.update(validate_settings({key: saved[key] for key in saved if key in DEFAULT_SETTINGS}))
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

    async def _reader_service(self, node):
        """The user service whose process is reading Gamescope's capture stream, if any."""
        if type(node) is not int:
            return None
        try:
            return service_for_node(json.loads(await self._run("/usr/bin/pw-dump")), node)
        except (RuntimeError, ValueError) as error:
            decky.logger.info("could not identify the capture reader: %s", error)
            return None

    async def _restore_services(self):
        units, self._paused_services = self._paused_services, []
        for unit in reversed(units):
            try:
                await self._run("/usr/bin/systemctl", "--user", "start", unit, timeout=25)
                decky.logger.info("restarted %s after Picture in Picture", unit)
            except RuntimeError as error:
                decky.logger.error("could not restart %s: %s", unit, error)

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
                asyncio.create_task(self._restore_services())
                asyncio.create_task(self._restore_audio())
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
                    paused_services=list(self._paused_services), busy_service=self._busy_service)

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
            if self._state["active"]:
                await self._restore_audio()
            command = f"show appid={appid} {settings_arguments(self._settings)}"
            self._busy_service = None
            for attempt in range(BUSY_RETRIES + 1):
                event = await self._helper.request(command, {"starting", "stopped"})
                if event["event"] == "starting":
                    self._state.update(active=True, appid=appid, reason=None, streaming=False)
                    await self._emit_state()
                    return self.get_state_sync()
                if event.get("reason") != "capture-busy" or attempt == BUSY_RETRIES:
                    break
                unit = await self._reader_service(event.get("node"))
                if unit in self._paused_services or (unit is None and self._paused_services):
                    pass  # A paused reader's link takes a moment to disappear.
                elif unit is not None and unit in self._settings["pause_services"]:
                    try:
                        await self._run("/usr/bin/systemctl", "--user", "stop", unit, timeout=25)
                    except RuntimeError as error:
                        decky.logger.error("could not pause %s: %s", unit, error)
                        break
                    self._paused_services.append(unit)
                    decky.logger.info("paused %s for Picture in Picture", unit)
                else:
                    self._busy_service = unit
                    break
                await asyncio.sleep(0.5)
            await self._restore_services()
            reason = event.get("reason", "unknown")
            self._state.update(active=False, reason=reason)
            await self._emit_state()
            if self._busy_service is not None:
                raise RuntimeError(f"{self._busy_service} is using Gamescope's capture stream")
            if reason == "capture-busy":
                raise RuntimeError("Another app is using Gamescope's capture stream "
                                   "(for example Steam Game Recording)")
            raise RuntimeError(f"Picture in Picture could not start: {reason}")

    async def stop(self):
        async with self._lock:
            if self._helper is not None and self._helper.alive and self._state["active"]:
                await self._helper.request("hide", {"stopped"})
            self._state.update(active=False, visible=False, streaming=False, stats=None)
            await self._restore_services()
            await self._restore_audio()
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
        if type(volume) is not int or not 0 <= volume <= 130:
            raise ValueError("volume must be an integer between 0 and 130")
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
        self._muted_appid = appid if muted else None
        return {"streams": len(nodes), "muted": muted}

    async def _restore_audio(self):
        """Mute is only reachable from the Picture in Picture panel, so undo it when that closes."""
        appid, self._muted_appid = self._muted_appid, None
        if appid is None:
            return
        try:
            await self.set_muted(appid, False)
        except (RuntimeError, ValueError) as error:
            decky.logger.info("could not unmute %s: %s", appid, error)

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
        await self._restore_services()
        await self._restore_audio()
