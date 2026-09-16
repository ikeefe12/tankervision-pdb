import json
import pathlib
import sys

for name in sys.argv[1:]:
    path = pathlib.Path(name)
    lines = [json.loads(line, parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))
             for line in path.read_text().splitlines() if line]
    assert lines, path
    previous = 0
    telemetry = []
    for obj in lines:
        assert obj["v"] == 1 and obj["boot_id"] == "A1B2C3D4", obj
        assert obj["seq"] > previous, obj
        previous = obj["seq"]
        assert isinstance(obj["uptime_ms"], int), obj
        if obj["type"] == "telemetry":
            telemetry.append(obj)
            assert len(obj["readings"]["analog"]) == 9, obj
            assert len(obj["readings"]["signals"]) == 30, obj
            assert isinstance(obj["readings"]["signals"]["BOOT_N"], bool), obj
            assert set(obj["charger"]) == {"enabled_expected", "maintenance"}, obj
            assert set(obj["readings"]["expanders"]) == {"internal", "external"}, obj
            assert len(obj["readings"]["pd"]["versions"]) == 16, obj
            assert isinstance(obj["jetson"]["responsive"], bool), obj
    settling = [obj for obj in telemetry if obj["uptime_ms"] < 10000]
    assert [obj["uptime_ms"] for obj in settling] == list(range(0, 10000, 1000)), path
    assert all(obj["state"] == "SETTLING" and obj["readings"]["signals"]["STAT_LED"] for obj in settling), path
    running = [obj for obj in telemetry if obj["state"] == "RUNNING"]
    assert running, path
    healthy = [obj for obj in running if obj["charger"]["enabled_expected"]]
    assert healthy and all(obj["readings"]["signals"]["SCC_EN"] and obj["charger"]["maintenance"] for obj in healthy), path
    if path.stem == "reconnect":
        faulted = [obj for obj in running if "CHARGER_FAULT" in obj["issues"]]
        assert faulted and all(obj["last_error"] == "CHARGER_TEMPERATURE_INVALID" for obj in faulted), path
        assert all(not obj["charger"]["enabled_expected"] and not obj["charger"]["maintenance"]
                   and all(obj["ports"].values()) and obj["backup_armed"] and obj["shutdown"] is None
                   for obj in faulted), path
        assert any(not obj["readings"]["signals"]["BOOT_N"] for obj in running), path
        settled_fault = faulted[-1]
        assert not settled_fault["readings"]["signals"]["SCC_EN"], path
        assert not settled_fault["readings"]["signals"]["24V_VBUS_EN"], path
    shutdowns = [obj for obj in telemetry if obj["state"] == "SHUTDOWN_WAIT"]
    if shutdowns:
        deadlines = {obj["shutdown"]["deadline_ms"] for obj in shutdowns}
        assert len(deadlines) == 1, path
        assert any(obj["shutdown"]["ack"] and obj["shutdown"]["ready"] for obj in shutdowns), path
        assert all(obj["ports"]["vbus_ss"] for obj in shutdowns), path
        if path.stem == "loss":
            complete = [obj for obj in shutdowns if obj["uptime_ms"] >= obj["shutdown"]["deadline_ms"] - 60000 + 300]
            assert complete, path
            assert all(not obj["ports"]["vbus"] and not obj["ports"]["5v_vbus"]
                       and obj["ports"]["5v_ss"] and not obj["readings"]["signals"]["5V_VBUS_EN"]
                       for obj in complete), path
            assert any(obj["readings"]["signals"]["PG_USB"] for obj in complete), path
        if path.stem == "reboot":
            assert all(all(obj["ports"].values()) for obj in shutdowns), path
    print(f"JSON validated: {path.name}: {len(lines)} frames, {len(telemetry)} telemetry snapshots")
