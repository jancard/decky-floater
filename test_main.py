import asyncio
import json
import logging
import os
import stat
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

decky = types.ModuleType("decky")
decky.logger = logging.getLogger("floater-test")
decky.emit = AsyncMock()
decky.DECKY_PLUGIN_DIR = "/nonexistent"
decky.DECKY_PLUGIN_SETTINGS_DIR = "/nonexistent"
decky.DECKY_PLUGIN_RUNTIME_DIR = "/nonexistent"
sys.modules["decky"] = decky

import main  # noqa: E402
from main import (DEFAULT_SETTINGS, Helper, Plugin, appid_for_pid, audio_nodes_for_app,  # noqa: E402
                  read_display, settings_arguments, validate_settings)

FAKE_HELPER = r"""#!/usr/bin/env python3
import json, sys
print(json.dumps({"event": "ready", "width": 3840, "height": 2160, "gamescope_stream": True}), flush=True)
for line in sys.stdin:
    verb = line.split()[0] if line.strip() else ""
    if verb == "show":
        print(json.dumps({"event": "starting", "appid": 1}), flush=True)
    elif verb == "apps":
        print(json.dumps({"event": "apps", "focused": 769, "running": [769, 42]}), flush=True)
    elif verb == "hide":
        print(json.dumps({"event": "stopped", "reason": "hidden"}), flush=True)
    elif verb == "quit":
        print(json.dumps({"event": "stopped", "reason": "quit"}), flush=True)
        break
"""


class FakeHelper:
    def __init__(self, responses):
        self.responses = list(responses)
        self.commands = []
        self.alive = True

    async def request(self, command, events, timeout=5):
        self.commands.append(command)
        return self.responses.pop(0)

    async def send(self, command):
        self.commands.append(command)

    async def stop(self):
        self.alive = False


class PureFunctionTests(unittest.TestCase):
    def test_validate_settings_accepts_valid_values(self):
        self.assertEqual(validate_settings({"corner": "top-left", "scale": 0.3, "opacity": 1,
                                            "fps": 60, "border": 3, "pause_hyperhdr": False}),
                         {"corner": "top-left", "scale": 0.3, "opacity": 1.0, "fps": 60, "border": 3,
                          "pause_hyperhdr": False})

    def test_validate_settings_rejects_invalid_values(self):
        for bad in ({"corner": "middle"}, {"scale": 0.9}, {"scale": "0.3"}, {"opacity": 0.1},
                    {"fps": 0}, {"fps": 30.0}, {"fps": True}, {"pause_hyperhdr": 1},
                    {"border": 21}, {"border": -1}, {"border": 2.5},
                    {"colour": "red"}, ["corner"], None):
            with self.assertRaises(ValueError, msg=repr(bad)):
                validate_settings(bad)

    def test_settings_arguments(self):
        self.assertEqual(settings_arguments(DEFAULT_SETTINGS),
                         "corner=bottom-left scale=0.250 opacity=1.000 fps=30 border=1")

    def test_appid_from_cgroup(self):
        with tempfile.TemporaryDirectory() as proc:
            Path(proc, "100").mkdir()
            Path(proc, "100", "cgroup").write_text(
                "0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                "app-steam-app3609786786-931012.scope\n")
            Path(proc, "200").mkdir()
            Path(proc, "200", "cgroup").write_text("0::/user.slice/app.slice/steam-launcher.service\n")
            self.assertEqual(appid_for_pid(100, proc), 3609786786)
            self.assertIsNone(appid_for_pid(200, proc))
            self.assertIsNone(appid_for_pid(300, proc))

    def test_audio_nodes_match_app_streams_only(self):
        dump = [
            {"id": 73, "type": "PipeWire:Interface:Node", "info": {"props": {
                "media.class": "Stream/Output/Audio", "application.process.id": 100}}},
            {"id": 74, "type": "PipeWire:Interface:Node", "info": {"props": {
                "media.class": "Stream/Output/Audio", "application.process.id": 200}}},
            {"id": 75, "type": "PipeWire:Interface:Node", "info": {"props": {
                "media.class": "Stream/Input/Audio", "application.process.id": 100}}},
            {"id": 76, "type": "PipeWire:Interface:Client", "info": {"props": {}}},
            {"id": 77, "type": "PipeWire:Interface:Node", "info": None},
            {"id": 78, "type": "PipeWire:Interface:Client", "info": {"props": {
                "application.process.id": 100}}},
            {"id": 79, "type": "PipeWire:Interface:Node", "info": {"props": {
                "media.class": "Stream/Output/Audio", "client.id": 78}}},
            {"id": 80, "type": "PipeWire:Interface:Node", "info": {"props": {
                "media.class": "Stream/Output/Audio", "client.id": 99}}},
        ]
        with tempfile.TemporaryDirectory() as proc:
            for pid, appid in ((100, 3609786786), (200, 1091500)):
                Path(proc, str(pid)).mkdir()
                Path(proc, str(pid), "cgroup").write_text(f"0::/app-steam-app{appid}-1.scope\n")
            self.assertEqual(audio_nodes_for_app(dump, 3609786786, proc), [73, 79])
            self.assertEqual(audio_nodes_for_app(dump, 42, proc), [])

    def test_read_display_sources(self):
        with tempfile.TemporaryDirectory() as runtime, tempfile.TemporaryDirectory() as proc:
            self.assertEqual(read_display(runtime, proc), ":0")
            Path(proc, "500").mkdir()
            Path(proc, "500", "comm").write_text("steam\n")
            Path(proc, "500", "environ").write_bytes(b"HOME=/home/deck\0DISPLAY=:1\0")
            self.assertEqual(read_display(runtime, proc), ":1")
            Path(runtime, "gamescope-environment").write_text("GAMESCOPE_WAYLAND_DISPLAY=gamescope-0\nDISPLAY=:0\n")
            self.assertEqual(read_display(runtime, proc), ":0")
            Path(runtime, "gamescope-environment").write_text("DISPLAY=$(reboot)\n")
            self.assertEqual(read_display(runtime, proc), ":1")


class PluginTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.settings_dir = tempfile.TemporaryDirectory()
        decky.DECKY_PLUGIN_SETTINGS_DIR = self.settings_dir.name
        decky.emit.reset_mock()
        self.plugin = Plugin()
        with patch("os.getuid", return_value=1000):
            await self.plugin._main()
        self.plugin._ensure_helper = AsyncMock()
        self.systemctl = []

        async def run(*command, timeout=10):
            self.systemctl.append(command[1:])
            if command[-2:] == ("is-active", main.HYPERHDR_UNIT):
                return "active\n" if self.hyperhdr_active else "inactive\n"
            return ""

        self.hyperhdr_active = True
        self.plugin._run = run

    async def asyncTearDown(self):
        self.settings_dir.cleanup()

    async def test_refuses_root(self):
        with patch("os.getuid", return_value=0):
            with self.assertRaisesRegex(RuntimeError, "root flag"):
                await Plugin()._main()

    async def test_helper_path_falls_back_to_executable_copy(self):
        with tempfile.TemporaryDirectory() as plugin_dir, tempfile.TemporaryDirectory() as runtime_dir:
            installed = Path(plugin_dir, "bin", main.HELPER_NAME)
            installed.parent.mkdir()
            installed.write_bytes(b"binary")
            installed.chmod(0o644)
            with patch.object(decky, "DECKY_PLUGIN_DIR", plugin_dir), \
                    patch.object(decky, "DECKY_PLUGIN_RUNTIME_DIR", runtime_dir):
                private = Path(self.plugin._helper_path())
                self.assertEqual(private.parent, Path(runtime_dir))
                self.assertTrue(os.access(private, os.X_OK))
                self.assertEqual(private.read_bytes(), b"binary")
                installed.chmod(0o755)
                self.assertEqual(self.plugin._helper_path(), str(installed))

    async def test_start_success(self):
        self.plugin._helper = FakeHelper([{"event": "starting", "appid": 42}])
        state = await self.plugin.start(42)
        self.assertTrue(state["active"])
        self.assertEqual(state["appid"], 42)
        self.assertEqual(self.plugin._helper.commands,
                         ["show appid=42 corner=bottom-left scale=0.250 opacity=1.000 fps=30 border=1"])
        self.assertEqual(self.systemctl, [])
        decky.emit.assert_awaited()

    async def test_start_pauses_hyperhdr_when_busy_and_restores_on_stop(self):
        self.plugin._helper = FakeHelper([
            {"event": "stopped", "reason": "capture-busy"},
            {"event": "starting", "appid": 42},
            {"event": "stopped", "reason": "hidden"},
        ])
        with patch("asyncio.sleep", AsyncMock()):
            state = await self.plugin.start(42)
        self.assertTrue(state["active"])
        self.assertTrue(state["paused_hyperhdr"])
        self.assertIn(("--user", "stop", main.HYPERHDR_UNIT), self.systemctl)
        await self.plugin.stop()
        self.assertEqual(self.systemctl[-1], ("--user", "start", main.HYPERHDR_UNIT))
        self.assertFalse(self.plugin._paused_hyperhdr)

    async def test_start_busy_without_hyperhdr_fails_without_touching_services(self):
        self.hyperhdr_active = False
        self.plugin._helper = FakeHelper([{"event": "stopped", "reason": "capture-busy"}])
        with self.assertRaisesRegex(RuntimeError, "Game Recording"):
            await self.plugin.start(42)
        self.assertNotIn(("--user", "stop", main.HYPERHDR_UNIT), self.systemctl)
        self.assertFalse(self.plugin._state["active"])

    async def test_start_busy_respects_pause_setting(self):
        self.plugin._settings["pause_hyperhdr"] = False
        self.plugin._helper = FakeHelper([{"event": "stopped", "reason": "capture-busy"}])
        with self.assertRaises(RuntimeError):
            await self.plugin.start(42)
        self.assertEqual(self.systemctl, [])

    async def test_persistent_busy_restores_hyperhdr(self):
        busy = {"event": "stopped", "reason": "capture-busy"}
        self.plugin._helper = FakeHelper([busy] * (main.BUSY_RETRIES + 1))
        with patch("asyncio.sleep", AsyncMock()), self.assertRaises(RuntimeError):
            await self.plugin.start(42)
        self.assertEqual(self.systemctl[-1], ("--user", "start", main.HYPERHDR_UNIT))

    async def test_helper_stopping_on_its_own_restores_hyperhdr(self):
        self.plugin._state["active"] = True
        self.plugin._paused_hyperhdr = True
        await self.plugin._on_event({"event": "stopped", "reason": "source-closed"})
        await asyncio.sleep(0)
        self.assertFalse(self.plugin._state["active"])
        self.assertEqual(self.plugin._state["reason"], "source-closed")
        self.assertEqual(self.systemctl[-1], ("--user", "start", main.HYPERHDR_UNIT))

    async def test_restarting_same_app_updates_settings(self):
        self.plugin._helper = FakeHelper([])
        self.plugin._state.update(active=True, appid=42)
        await self.plugin.start(42)
        self.assertEqual(self.plugin._helper.commands,
                         ["set corner=bottom-left scale=0.250 opacity=1.000 fps=30 border=1"])

    async def test_switching_app_restores_audio_and_shows_new_app(self):
        self.plugin._helper = FakeHelper([{"event": "starting", "appid": 7}])
        self.plugin._state.update(active=True, appid=42)
        self.plugin._muted_appid = 42
        self.plugin.set_muted = AsyncMock()
        state = await self.plugin.start(7)
        self.plugin.set_muted.assert_awaited_once_with(42, False)
        self.assertEqual(state["appid"], 7)
        self.assertTrue(state["active"])
        self.assertEqual(self.plugin._helper.commands,
                         ["show appid=7 corner=bottom-left scale=0.250 opacity=1.000 fps=30 border=1"])

    async def test_rejects_invalid_appids(self):
        for appid in (0, -1, 2**32, "42", 4.2, True, None):
            with self.assertRaises(ValueError):
                await self.plugin.start(appid)

    async def test_settings_persist_and_apply_live(self):
        self.plugin._helper = FakeHelper([])
        self.plugin._state["active"] = True
        await self.plugin.set_settings({"corner": "top-left", "fps": 60})
        self.assertEqual(self.plugin._helper.commands, ["set corner=top-left fps=60"])
        saved = json.loads(Path(self.settings_dir.name, "settings.json").read_text())
        self.assertEqual(saved["corner"], "top-left")
        self.assertEqual(saved["fps"], 60)
        with patch("os.getuid", return_value=1000):
            reloaded = Plugin()
            await reloaded._main()
        self.assertEqual(reloaded._settings["corner"], "top-left")

    async def test_invalid_saved_settings_fall_back_to_defaults(self):
        Path(self.settings_dir.name, "settings.json").write_text('{"fps": 999}')
        with patch("os.getuid", return_value=1000):
            plugin = Plugin()
            await plugin._main()
        self.assertEqual(plugin._settings, DEFAULT_SETTINGS)

    async def test_volume_and_mute_use_wpctl_on_app_streams(self):
        calls = []

        async def run(*command, timeout=10):
            calls.append(command)
            if command[0].endswith("pw-dump"):
                return "[]"
            return ""

        self.plugin._run = run
        with patch.object(main, "audio_nodes_for_app", return_value=[73, 80]):
            self.assertEqual(await self.plugin.set_volume(42, 80), {"streams": 2, "volume": 80})
            await self.plugin.set_muted(42, True)
        self.assertIn(("/usr/bin/wpctl", "set-volume", "73", "0.80"), calls)
        self.assertIn(("/usr/bin/wpctl", "set-volume", "80", "0.80"), calls)
        self.assertIn(("/usr/bin/wpctl", "set-mute", "80", "1"), calls)

    async def test_close_unmutes_app_muted_by_floater(self):
        calls = []

        async def run(*command, timeout=10):
            calls.append(command)
            return "[]"

        self.plugin._run = run
        self.plugin._helper = FakeHelper([{"event": "stopped", "reason": "hidden"}])
        self.plugin._state.update(active=True, appid=42)
        with patch.object(main, "audio_nodes_for_app", return_value=[73]):
            await self.plugin.set_muted(42, True)
            await self.plugin.stop()
        self.assertEqual(calls[-1], ("/usr/bin/wpctl", "set-mute", "73", "0"))
        self.assertIsNone(self.plugin._muted_appid)

    async def test_volume_validation_and_missing_audio(self):
        for volume in (-1, 131, 50.5, "50", None, True):
            with self.assertRaises(ValueError):
                await self.plugin.set_volume(42, volume)
        with self.assertRaises(ValueError):
            await self.plugin.set_muted(42, "yes")

        async def run(*command, timeout=10):
            return "[]"

        self.plugin._run = run
        with self.assertRaisesRegex(RuntimeError, "no audio"):
            await self.plugin.set_volume(42, 50)


class HelperProcessTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name, "pip-overlay")
        self.path.write_text(FAKE_HELPER)
        self.path.chmod(self.path.stat().st_mode | stat.S_IXUSR)
        self.events = []

        async def on_event(event):
            self.events.append(event)

        self.helper = Helper(str(self.path), dict(os.environ), on_event)

    async def asyncTearDown(self):
        await self.helper.stop()
        self.directory.cleanup()

    async def test_round_trip(self):
        ready = await self.helper.start()
        self.assertTrue(ready["gamescope_stream"])
        apps = await self.helper.request("apps", {"apps"})
        self.assertEqual(apps["running"], [769, 42])
        started = await self.helper.request("show appid=42", {"starting", "stopped"})
        self.assertEqual(started["event"], "starting")
        await self.helper.stop()
        self.assertFalse(self.helper.alive)
        self.assertEqual(self.events[-1], {"event": "exited"})


if __name__ == "__main__":
    unittest.main()
