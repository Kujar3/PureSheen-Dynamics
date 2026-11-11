#!/usr/bin/env python3
"""End-to-end test of the Claude bridge: the real helper program against a real suite instance.

    python tools/bridge/test_bridge.py <PureSheenBridge.exe> <PureSheenDev.exe>

Starts `PureSheenDev bridgehost` (one suite instance playing a test tone) on a temporary folder, starts the helper the
way Claude does, and speaks MCP to it over stdin / stdout. Nothing outside the temporary folder is touched.
"""
import json
import math
import os
import pathlib
import random
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave

failures = 0
checks = 0


def check(ok, what):
    global failures, checks
    checks += 1
    if not ok:
        failures += 1
        print("FAILED:", what)


def write_test_wav(path, seconds):
    """A stereo tone with slow level movement and a little noise, 16 bit, 48 kHz."""
    generator = random.Random(1)
    with wave.open(str(path), "wb") as file:
        file.setnchannels(2)
        file.setsampwidth(2)
        file.setframerate(48000)
        frames = bytearray()
        for i in range(48000 * seconds):
            tone = 0.3 * math.sin(0.03 * i) * (0.6 + 0.4 * math.sin(0.0005 * i))
            for _ in range(2):
                frames += struct.pack("<h", int(32767 * (tone + 0.05 * (generator.random() - 0.5))))
        file.writeframes(bytes(frames))


def wait_for(read, done, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        value = read()
        if done(value):
            return value
        time.sleep(0.25)
    return None


class Helper:
    def __init__(self, exe, folder):
        env = dict(os.environ, PURESHEEN_BRIDGE_DIR=str(folder))
        self.process = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.next_id = 0

    def write(self, text):
        self.process.stdin.write(text.encode("utf-8") + b"\n")
        self.process.stdin.flush()

    def read(self, timeout=30):
        result = {}

        def reader():
            result["line"] = self.process.stdout.readline()

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        thread.join(timeout)
        if thread.is_alive():
            raise TimeoutError("no answer from the helper")
        return result["line"].decode("utf-8")

    def request(self, method, params=None):
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            message["params"] = params
        self.write(json.dumps(message))
        line = self.read()
        check(line.endswith("\n") and line.count("\n") == 1, "a response is exactly one line")
        reply = json.loads(line)
        check(reply.get("jsonrpc") == "2.0" and reply.get("id") == self.next_id, f"{method}: the id comes back")
        return reply

    def tool(self, name, arguments=None):
        reply = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        result = reply["result"]
        text = result["content"][0]["text"]
        return result.get("isError", False), text

    def tool_json(self, name, arguments=None):
        error, text = self.tool(name, arguments)
        check(not error, f"{name}: {text[:200]}")
        return json.loads(text) if not error else {}


def main():
    helper_exe, dev_exe = sys.argv[1], sys.argv[2]
    folder = pathlib.Path(tempfile.mkdtemp(prefix="puresheen-bridge-e2e-"))

    # --- no instance yet
    helper = Helper(helper_exe, folder)
    reply = helper.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
    result = reply["result"]
    check(result["protocolVersion"] == "2025-06-18", "the client's version is kept")
    check("tools" in result["capabilities"], "declares tools")
    check(result["serverInfo"]["name"] == "puresheen-dynamics", "server name")
    check("cannot hear" in result["instructions"], "tells Claude what it cannot do")
    check("never leave" not in result["instructions"] and "sample peak" in result["instructions"], "explains the ceiling's two modes")

    helper.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}))   # no answer expected
    check(helper.request("ping")["result"] == {}, "ping")

    reply = helper.request("initialize", {"protocolVersion": "1999-01-01", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
    check(reply["result"]["protocolVersion"] != "1999-01-01", "an unknown version gets ours")

    check(helper.request("no/such/method")["error"]["code"] == -32601, "unknown method")
    check("error" in helper.request("tools/call", {"name": "no_such_tool", "arguments": {}}), "unknown tool")

    helper.write("this is not json")
    check(json.loads(helper.read())["error"]["code"] == -32700, "a parse error is answered")
    helper.write("[" * 100000)
    check(json.loads(helper.read())["error"]["code"] == -32700, "deep nesting is refused, not parsed")

    # JSON-RPC envelopes that are not requests
    for bad, what in (("[]", "a list"), ('{"id":5}', "no method"), ('{"jsonrpc":"2.0","id":5}', "an id and no method"),
                      ('{"jsonrpc":"1.0","id":5,"method":"ping"}', "the wrong version"),
                      ('{"jsonrpc":"2.0","id":5,"method":7}', "a number for a method")):
        helper.write(bad)
        check(json.loads(helper.read())["error"]["code"] == -32600, f"invalid request: {what}")
    helper.write('{"jsonrpc":"2.0","id":99,"result":{}}')     # a response to nothing: no answer
    check(helper.request("ping")["result"] == {}, "a stray response gets no answer")

    tools = helper.request("tools/list")["result"]["tools"]
    names = [t["name"] for t in tools]
    check(len(names) == len(set(names)), "tool names are unique")
    check(len(names) >= 30, "tools are listed without a running instance")
    for tool in tools:
        check(tool["inputSchema"]["type"] == "object", f"{tool['name']}: schema")
        check(all(r in tool["inputSchema"]["properties"] for r in tool["inputSchema"]["required"]), f"{tool['name']}: required are properties")
        check(len(tool["description"]) > 10, f"{tool['name']}: description")
    for dangerous in ("delete_preset", "delete_file", "run", "shell"):
        check(dangerous not in names, f"no {dangerous}")

    error, text = helper.tool("get_status")
    check(error and "Allow Claude to connect" in text, "with no instance it says what to do")
    check(helper.tool_json("list_instances")["instances"] == [], "no instances")

    # --- a stale file from a crashed host goes away
    stale = folder / "deadbeef.json"
    stale.write_text(json.dumps({"id": "deadbeef", "port": 1, "token": "x", "pid": 999999}))
    check(helper.tool_json("list_instances")["instances"] == [], "a dead instance is not listed")
    check(not stale.exists(), "its file is removed")

    # --- one instance
    host = subprocess.Popen([dev_exe, "bridgehost", str(folder), "240"], stdout=subprocess.PIPE)
    check(host.stdout.readline().strip() == b"ready", "the host listens")
    try:
        instances = helper.tool_json("list_instances")["instances"]
        check(len(instances) == 1, "one instance")
        instance_id = instances[0]["id"]

        status = helper.tool_json("get_status")
        check(status["product"] == "PureSheen Dynamics", "get_status through the helper")
        check(len(status["chain"]) >= 1, "a chain")

        # Every tool the helper lists must exist in the suite: wrong arguments are fine, unknown commands are not.
        for name in names:
            if name in ("list_instances", "select_instance", "undo", "redo", "assistant_listen"):
                continue
            error, text = helper.tool(name, {})
            check("Unknown command" not in text, f"{name}: the suite knows it")

        # A real edit, and its undo.
        before = helper.tool_json("get_parameters", {"ids": ["max.gain"]})["parameters"][0]["value"]
        changed = helper.tool_json("set_parameters", {"changes": [{"id": "max.gain", "value": 3.0}], "description": "from the test"})
        check(abs(changed["changes"][0]["value"] - 3.0) < 0.01, "set_parameters")
        history = helper.tool_json("get_history", {"limit": 2})
        check(history["steps"][-1]["description"] == "Claude: from the test", "the history names Claude")
        check(changed.get("request", 0) > 0 and history["steps"][-1].get("request") == changed["request"], "the step carries the request number")
        check("request" not in history["steps"][0] or history["steps"][0]["index"] != 0, "a step Claude did not cause has none")
        check(abs(changed["changes"][0]["previous"] - before) < 0.01, "the reply says what the value was")

        # What the sessions that read these replies asked for.
        error, raw = helper.tool("get_parameters", {"module": "eq1"})
        check(not error and not re.search(r"\d\.\d{5,}", raw), "numbers are rounded")
        shapes = [p for p in json.loads(raw)["parameters"] if p["id"].endswith(".shape")]
        check(len(shapes) >= 2 and "choices" in shapes[0] and "choices" not in shapes[1] and shapes[1].get("choices_as") == shapes[0]["id"],
              "a long list of choices comes once per reply")
        helper.tool_json("set_parameters", {"changes": [{"id": "eq1.a.b4.gain", "value": 2.0}]})
        ids = [p["id"] for p in helper.tool_json("get_parameters", {"module": "eq1", "changed_only": True})["parameters"]]
        check("eq1.a.b4.gain" in ids and "eq1.a.b4.shape" in ids and "eq1.a.b4.on" in ids, f"changed_only keeps what a band's value means: {ids}")
        check("eq1.a.b5.shape" not in ids, "and leaves other bands out")
        noted = helper.tool_json("set_parameters", {"changes": [{"id": "eq1.b.b4.gain", "value": 1.0}]})
        check("Stereo" in noted["changes"][0].get("channel_note", ""), "set B in Stereo says it does nothing")
        helper.tool_json("undo")
        helper.tool_json("undo")

        meters = helper.tool_json("get_meters")
        again = helper.tool_json("get_meters")
        check(again["peak_window_seconds"] is not None and again["peak_window_seconds"] >= 0, "get_meters says what its peaks cover")
        check("host_position_seconds" in meters and "seconds_since_last_change" in meters, "and where the playhead and the last change are")
        comparison = helper.tool_json("get_comparison")
        check("settings_changed_during_analysis" in comparison and "analysed_seconds_since_last_change" in comparison, "get_comparison says what it spans")
        check(helper.tool_json("reset_comparison")["reset"] is True, "the comparison can be reset")
        instances = helper.tool_json("list_instances")["instances"]
        check(len(instances) != 1 or instances[0]["selected"] is True, "the only instance reads as selected")

        # A snapshot brings the preset bar back with the settings.
        status = helper.tool_json("get_status")
        helper.tool_json("store_snapshot", {"slot": "D"})
        helper.tool_json("load_preset", {"name": "Streaming Ready"})
        check(helper.tool_json("get_status")["preset"] == "Streaming Ready", "the preset loaded")
        helper.tool_json("recall_snapshot", {"slot": "D"})
        recalled = helper.tool_json("get_status")
        check(recalled["preset"] == status["preset"] and recalled["preset_modified"] == status["preset_modified"],
              f"recalling a snapshot restores the preset name: {recalled['preset']} / {status['preset']}")
        helper.tool_json("undo")
        helper.tool_json("undo")
        helper.tool_json("undo")
        after = helper.tool_json("get_parameters", {"ids": ["max.gain"]})["parameters"][0]["value"]
        check(after == before, "undo restores it")

        # The instance argument, and a wrong one.
        check(helper.tool_json("get_status", {"instance": instance_id})["product"] == "PureSheen Dynamics", "instance argument")
        error, text = helper.tool("get_status", {"instance": "nope"})
        check(error, "a wrong instance is an error")
        error, _ = helper.tool("select_instance", {"id": "nope"})
        check(error, "selecting a wrong instance is an error")
        helper.tool_json("select_instance", {"id": instance_id})

        # Audio reaches the meters.
        deadline = time.time() + 15
        lufs = None
        while lufs is None and time.time() < deadline:
            meters = helper.tool_json("get_meters")
            lufs = meters["output"]["momentary_lufs"]
            time.sleep(0.25)
        check(lufs is not None and -60 < lufs < 0, f"the meters see the tone: {lufs}")
        check(helper.tool_json("get_meters")["host_playing"] in (True, False), "host_playing")
        monitoring = helper.tool_json("get_meters")
        check(monitoring["solo"] is None and monitoring["solo_keeps_maximizer"] is False, f"solo state: {monitoring.get('solo')}")
        check(monitoring["output"]["meters_ignore_gain_match"] is False, "the meters follow Gain Match by default")
        if "dyn" in [m["module"] for m in helper.tool_json("get_status")["chain"]]:
            dyn = monitoring.get("dynamics")
            check(dyn is not None and len(dyn["bands"]) >= 1 and "gain_change_db" in dyn["bands"][0]["parts"][0], f"Dynamics bands in get_meters: {dyn}")
        else:
            helper.tool_json("add_module", {"module": "dyn"})
            dyn = helper.tool_json("get_meters").get("dynamics")
            check(dyn is not None and dyn["channel_mode"] == "Stereo" and dyn["bands"][0]["parts"][0]["part"] == "stereo", f"Dynamics bands in get_meters: {dyn}")
            helper.tool_json("undo")
        helper.tool_json("get_meters")   # clears the peak held since the last read
        time.sleep(0.5)
        peak = helper.tool_json("get_meters")["output"]["true_peak_dbtp"]
        check(peak is not None and -40 < peak < 3, f"true peak since the last read: {peak}")

        # The Maximizer's gain reduction: limiting shows in the peak, whatever one audio block happened to say.
        base_gain = helper.tool_json("get_parameters", {"ids": ["max.gain"]})["parameters"][0]["value"]
        helper.tool_json("set_parameters", {"changes": [{"id": "max.gain", "value": 18.0}], "description": "limiter test"})
        helper.tool_json("get_meters")
        time.sleep(0.5)
        limiting = helper.tool_json("get_meters")["maximizer"]
        check(limiting["peak_gain_reduction_db"] < -1.0, f"the peak shows the limiting: {limiting}")
        helper.tool_json("undo")
        time.sleep(1.0)
        helper.tool_json("get_meters")     # clears the peak held from the limiting
        time.sleep(0.5)
        quiet = helper.tool_json("get_meters")["maximizer"]
        check(quiet["peak_gain_reduction_db"] > -0.5, f"and it clears without limiting: {quiet}")
        check(abs(helper.tool_json("get_parameters", {"ids": ["max.gain"]})["parameters"][0]["value"] - base_gain) < 0.01, "undo restored the gain")

        # Which peak the ceiling limits: the meters say, so a true peak above a sample-peak ceiling is not read as a fault.
        was_on = helper.tool_json("get_parameters", {"ids": ["max.truePeak"]})["parameters"][0]["value"] >= 0.5
        for switch, mode in ((True, "true peak"), (False, "sample peak")):
            helper.tool_json("set_parameters", {"changes": [{"id": "max.truePeak", "value": switch}]})
            maximizer = helper.tool_json("get_meters")["maximizer"]
            check(maximizer["ceiling_applies_to"] == mode, f"the ceiling applies to {mode}: {maximizer}")
            check(isinstance(maximizer["ceiling_db"], (int, float)), "the ceiling is reported")
        helper.tool_json("set_parameters", {"changes": [{"id": "max.truePeak", "value": was_on}]})

        # Bypass stays with the user.
        error, text = helper.tool("set_parameters", {"changes": [{"id": "io.bypass", "value": True}]})
        check(error and "monitoring" in text, "bypass is refused")
        check(helper.tool_json("get_status")["bypassed"] is False, "and stays off")

        # Two instances: nothing goes to the wrong one.
        other_host = subprocess.Popen([dev_exe, "bridgehost", str(folder), "120", "stopB"], stdout=subprocess.PIPE)
        check(other_host.stdout.readline().strip() == b"ready", "a second host listens")
        try:
            instances = helper.tool_json("list_instances")["instances"]
            check(len(instances) == 2, "two instances")
            other_id = next(i["id"] for i in instances if i["id"] != instance_id)

            fresh = Helper(helper_exe, folder)
            fresh.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "fresh", "version": "1"}})
            error, text = fresh.tool("get_status")
            check(error and "Several instances" in text, "with two and no selection it asks")
            check(fresh.tool_json("get_status", {"instance": other_id})["product"] == "PureSheen Dynamics", "the instance argument picks one")
            fresh.tool_json("select_instance", {"id": other_id})
            check(fresh.tool_json("get_status")["product"] == "PureSheen Dynamics", "the selection is used")

            # The selected one closes while the other keeps running: the next edit must not land on the other.
            before = fresh.tool_json("get_parameters", {"ids": ["max.gain"], "instance": instance_id})["parameters"][0]["value"]
            (folder / "stopB").write_text("")
            other_host.wait(30)
            error, text = fresh.tool("set_parameters", {"changes": [{"id": "max.gain", "value": 7.0}]})
            check(error and "not running" in text, "an edit for a closed instance fails")
            after = helper.tool_json("get_parameters", {"ids": ["max.gain"]})["parameters"][0]["value"]
            check(after == before, "and does not reach the other instance")
            fresh.process.stdin.close()
            fresh.process.wait(10)
        finally:
            if other_host.poll() is None:
                other_host.kill()

        # Non-ASCII survives, in the history and on the way back.
        helper.tool_json("set_parameters", {"changes": [{"id": "max.gain", "value": 2.0}], "description": "Größe ✓"})
        check(helper.tool_json("get_history", {"limit": 1})["steps"][-1]["description"] == "Claude: Größe ✓", "non-ASCII text")

        # A second helper at the same time, as with two Claude windows.
        second = Helper(helper_exe, folder)
        second.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "second", "version": "1"}})
        check(second.tool_json("get_status")["product"] == "PureSheen Dynamics", "a second helper")
        check(helper.tool_json("get_status")["product"] == "PureSheen Dynamics", "the first keeps working")
        second.process.stdin.close()
        second.process.wait(10)

        # The Assistant, from a file: analysis, suggestions, apply, macro, undo.
        wav = folder / "track.wav"
        write_test_wav(wav, 12)
        error, text = helper.tool("assistant_set_macro", {"macro": "Width", "value": 50})
        check(error and "applied" in text, "a macro before a suggestion is applied is an error")
        helper.tool_json("assistant_settings", {"intensity": "gentle"})
        started = helper.tool_json("assistant_analyse_file", {"path": str(wav)})
        check(started["phase"] in ("analysing", "ready"), f"the Assistant works on the file: {started['phase']}")
        # Not waiting for "working": that background measuring can take a minute or two, and the cards are usable before it.
        assistant = wait_for(lambda: helper.tool_json("get_assistant"), lambda a: a["applied"] >= 0, 60)
        check(assistant is not None and len(assistant["suggestions"]) >= 2, "suggestions arrive")
        if assistant is not None:
            late = helper.tool_json("get_history", {"limit": 1})["steps"][-1]
            check(late["description"].startswith("Claude: Assistant:"), f"the result that arrives later stays Claude's: {late['description']}")
            check(late.get("request") == started.get("request") and started.get("request", 0) > 0,
                  f"and carries the number of the request that started it: {late.get('request')} / {started.get('request')}")
            check(assistant["settings"]["intensity"] == "gentle", "settings stick")
            film = helper.tool_json("assistant_settings", {"auto_loudness": False, "loudness_lufs": -23})
            check(abs(film["settings"]["loudness_lufs"] + 23) < 0.01, "a film target is accepted")
            floor = helper.tool_json("assistant_settings", {"loudness_lufs": -30})
            check(abs(floor["settings"]["loudness_lufs"] + 24) < 0.01, "the target stops at -24")
            helper.tool_json("assistant_settings", {"auto_loudness": True})
            other = next(s["index"] for s in assistant["suggestions"] if s["index"] != assistant["applied"])
            applied = helper.tool_json("assistant_apply", {"index": other})
            check(applied["applied"] == other, "assistant_apply")
            check(helper.tool_json("get_history", {"limit": 1})["steps"][-1]["description"].startswith("Claude: "), "applying is labelled")
            card = next(s for s in assistant["suggestions"] if s["index"] == other)
            check(helper.tool_json("get_assistant")["status"].startswith(card["name"] + ":"), "the status follows the card applied")

            capped = helper.tool_json("assistant_set_macro", {"macro": "Loudness", "value": -30})
            check(abs(capped["value"] + 24) < 0.01 and "limited" in capped, f"a capped macro says so: {capped}")
            check(capped["maximizer_gain_db"] == 0 and capped["maximizer_trim_db"] < 0 and "Trim" in capped.get("note", ""), f"and the note names the Trim: {capped}")
            helper.tool_json("undo")
            error, _ = helper.tool("assistant_apply", {"index": 999})
            check(error, "a suggestion that does not exist is an error")

            moved = helper.tool_json("assistant_set_macro", {"macro": "Width", "value": 42})
            check(abs(moved["value"] - 42) < 0.5, f"assistant_set_macro: {moved['value']}")
            helper.tool_json("undo")
            back = next(m for m in helper.tool_json("get_assistant")["macros"] if m["name"] == "Width Match" or "Width" in m["name"])
            check(abs(back["value"] - 42) > 0.5, "undo moves the macro back")
            error, _ = helper.tool("assistant_set_macro", {"macro": "nope", "value": 1})
            check(error, "an unknown macro is an error")

            # A macro says when it moved the gain, and turning matching off leaves a hand-set gain alone.
            helper.tool_json("set_parameters", {"changes": [{"id": "max.gain", "value": 2.0}]})
            moved = helper.tool_json("assistant_set_macro", {"macro": "Width", "value": 80})
            gain_now = helper.tool_json("get_parameters", {"ids": ["max.gain"]})["parameters"][0]["value"]
            check(abs(moved["maximizer_gain_db"] - gain_now) < 0.01, "the reply has the gain")
            check(("note" in moved) == (abs(gain_now - 2.0) > 0.005), f"the note follows the gain: {moved}")
            helper.tool_json("assistant_settings", {"loudness_matched": False})
            helper.tool_json("set_parameters", {"changes": [{"id": "max.gain", "value": 2.5}]})
            moved = helper.tool_json("assistant_set_macro", {"macro": "Width", "value": 30})
            check(abs(moved["maximizer_gain_db"] - 2.5) < 0.01 and "note" not in moved, f"matching off keeps the gain: {moved}")
            helper.tool_json("assistant_settings", {"loudness_matched": True})

            # Settings alone keep the current sound; a rebuild is asked for.
            steps = len(helper.tool_json("get_history", {"limit": 1000})["steps"])
            kept = helper.tool_json("assistant_settings", {"intensity": "strong"})
            check(kept["rebuilt"] is False and kept["settings"]["intensity"] == "strong", "settings do not rebuild by themselves")
            check(len(helper.tool_json("get_history", {"limit": 1000})["steps"]) == steps, "and add no undo step")
            width = next(m for m in helper.tool_json("get_assistant")["macros"] if "Width" in m["name"])
            check(abs(width["value"] - 30) < 0.5, f"and keep the changes: {width['value']}")
            check(helper.tool_json("assistant_settings", {"rebuild": True})["rebuilt"] is True, "a rebuild is reported")
            check(wait_for(lambda: helper.tool_json("get_assistant"), lambda a: a["applied"] >= 0, 60) is not None, "a rebuild brings suggestions")

        # Comparing without a reference: no placeholder numbers.
        empty = helper.tool_json("get_comparison")
        check(empty["ready"] is False and empty["master"] == {"valid": False}, f"no master numbers without a reference: {empty['master']}")

        # References: load, compare, loop, remove. The file stays on disk.
        added = helper.tool_json("add_reference", {"path": str(wav)})
        reference_id = added["added"]
        ready = wait_for(lambda: helper.tool_json("get_references"), lambda r: r["references"][0]["state"] != "loading", 60)
        check(ready is not None and ready["references"][0]["state"] == "ready", "the reference loads")
        check(ready is not None and ready["references"][0]["integrated_lufs"] is not None, "with its loudness")
        comparison = helper.tool_json("get_comparison")
        check(len(comparison["bands"]) == 6 and len(comparison["hints"]) >= 1, "get_comparison has bands and hints")
        check(all("master_dynamic_range_db" in b and "reference_peak_over_median_db" in b for b in comparison["bands"]), "every band has its dynamics fields")
        check(any(b["reference_dynamic_range_db"] is not None for b in comparison["bands"]), f"the reference has band dynamics: {comparison['bands']}")
        looped = helper.tool_json("set_reference_loop", {"id": reference_id, "letter": "B", "start_seconds": 1.0, "end_seconds": 3.0})
        check(looped["references"][0]["active_loop"] == "B", "a loop becomes active")
        helper.tool_json("set_reference_loop", {"id": reference_id})
        check(helper.tool_json("get_references")["references"][0]["active_loop"] is None, "back to the whole track")
        helper.tool_json("assistant_use_reference", {"id": reference_id})
        check(wait_for(lambda: helper.tool_json("get_assistant"), lambda a: a["reference"] is not None, 30) is not None, "the Assistant takes the reference as its target")
        helper.tool_json("assistant_use_reference", {"id": 0})
        check(helper.tool_json("remove_reference", {"id": reference_id})["references"] == [], "remove_reference")
        check(wav.exists(), "the file stays on disk")
        error, _ = helper.tool("select_reference", {"id": reference_id})
        check(error, "a removed reference is an error")

        # Learn: both start and stop on command.
        learning = helper.tool_json("learn", {"target": "maximizer"})
        check(learning["learning"] is True, "the Maximizer learns")
        check(helper.tool_json("learn", {"target": "maximizer", "action": "cancel"})["learning"] is False, "and stops")
        check(helper.tool("learn", {"target": "nope"})[0], "an unknown learn target is an error")

        # The host goes away.
        (folder / "stop").write_text("")
        host.wait(30)
        error, text = helper.tool("get_status")
        check(error, "calls fail in words after the host closes")
    finally:
        if host.poll() is None:
            host.kill()

    # --- stdin closes: the helper exits by itself
    helper.process.stdin.close()
    check(helper.process.wait(10) == 0, "the helper exits when Claude does")

    for leftover in folder.glob("*.json"):
        check(False, f"left behind: {leftover.name}")

    print(f"test_bridge: {checks - failures} passed, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
