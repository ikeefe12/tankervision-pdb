#!/usr/bin/env python3
"""Normalize charger bench logs; optionally plot using matplotlib.

Usage: python analyze_charger.py results/hardware.log [--output-dir DIR] [--plot]
The parser and CSV/JSON exports use only the Python standard library. A hardware
FAIL remains a recorded test result, not an error running this analysis program.
"""

import argparse
import csv
import io
import json
import math
from pathlib import Path
import re
import sys


ADC_MV_TO_VCAP_V = 0.0092
DEFAULT_TARGET_V = 20.629
RAW_COLUMNS = [
    "phase", "time_us", "adc_min_mv", "adc_max_mv", "adc_mean_mv",
    "input", "output", "pins",
]
NORMALIZED_COLUMNS = [
    "phase", "time_s", "vcap_min_v", "vcap_max_v", "vcap_mean_v",
    "input_raw", "output_raw", "pins_raw",
    "adc_min_mv", "adc_max_mv", "adc_mean_mv",
]
LIMITATION = (
    "VCAP is estimated from the ESP32 ADC using a 9.2:1 divider; no meter calibration. "
    "Each ADC value averages 16 readings; 10 ms bins show their min/max/mean. "
    "Sampling starts after the CE setter returns. Analog RC filtering and polling "
    "do not resolve switching ripple or peak overshoot. Digital bytes are the "
    "latest status read, nominally refreshed every 20 ms."
)


def scalar(value):
    """Convert unambiguous decimal telemetry values, retaining labels as strings."""
    if re.fullmatch(r"[+-]?\d+", value):
        return int(value, 10)
    try:
        number = float(value)
        return number if math.isfinite(number) else value
    except ValueError:
        return value


def fields(text, hex_keys=()):
    result = {}
    for match in re.finditer(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)", text):
        key, value = match.groups()
        result[key] = int(value, 16) if key in hex_keys else scalar(value)
    return result


def parse_log(path):
    rows, summaries, shutdowns, finals, results, faults, phase_begins = [], [], [], [], [], [], []
    target_v = DEFAULT_TARGET_V
    with path.open(encoding="utf-8", errors="replace") as source:
        for line_number, line in enumerate(source, 1):
            line = line.strip()
            try:
                if line.startswith("CHARGE_CSV "):
                    values = next(csv.reader(io.StringIO(line[len("CHARGE_CSV "):])))
                    if values == RAW_COLUMNS:
                        continue
                    if len(values) != len(RAW_COLUMNS):
                        raise ValueError(f"expected {len(RAW_COLUMNS)} CSV fields, received {len(values)}")
                    raw = dict(zip(RAW_COLUMNS, values))
                    phase = raw.pop("phase")
                    raw = {key: int(value, 10) for key, value in raw.items()}
                    if not phase or any(value < 0 for value in raw.values()):
                        raise ValueError("empty phase or negative ADC/time/register value")
                    if any(raw[key] > 255 for key in ("input", "output", "pins")):
                        raise ValueError("register value does not fit one byte")
                    if not raw["adc_min_mv"] <= raw["adc_mean_mv"] <= raw["adc_max_mv"]:
                        raise ValueError("ADC mean is outside min/max range")
                    rows.append({
                        "phase": phase,
                        "time_s": raw["time_us"] / 1_000_000,
                        "vcap_min_v": round(raw["adc_min_mv"] * ADC_MV_TO_VCAP_V, 4),
                        "vcap_max_v": round(raw["adc_max_mv"] * ADC_MV_TO_VCAP_V, 4),
                        "vcap_mean_v": round(raw["adc_mean_mv"] * ADC_MV_TO_VCAP_V, 4),
                        "input_raw": raw["input"], "output_raw": raw["output"],
                        "pins_raw": raw["pins"],
                        **{key: raw[key] for key in ("adc_min_mv", "adc_max_mv", "adc_mean_mv")},
                    })
                elif line.startswith("CHARGE_SUMMARY "):
                    entry = fields(line)
                    if "phase" not in entry or entry.get("result") not in ("PASS", "FAIL"):
                        raise ValueError("summary is missing phase or PASS/FAIL result")
                    summaries.append(entry)
                elif line.startswith("CHARGE_FINAL "):
                    finals.append(fields(line, hex_keys=("OUT", "PINS", "EOUT")))
                elif line.startswith("CHARGE_SHUTDOWN "):
                    entry = fields(line)
                    if "phase" not in entry:
                        raise ValueError("shutdown observation is missing its base phase")
                    shutdowns.append(entry)
                elif line.startswith("RESULT CHARGER_DEEP "):
                    verdict = line.split()[2]
                    if verdict not in ("PASS", "FAIL"):
                        raise ValueError("unknown CHARGER_DEEP result")
                    results.append({"result": verdict, **fields(line)})
                elif line.startswith("CHARGE_FAULT "):
                    faults.append(line[len("CHARGE_FAULT "):])
                elif line.startswith("PHASE_BEGIN "):
                    phase_begins.append({"phase": line.split()[1], **fields(line)})
                elif line.startswith("BEGIN CHARGER_DEEP "):
                    target_v = float(fields(line).get("target", DEFAULT_TARGET_V))
            except (ValueError, IndexError, csv.Error) as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error
    if not rows and not summaries and not results:
        raise ValueError(f"{path}: no charger characterization records found")
    phase_names = list(dict.fromkeys(row["phase"] for row in rows))
    document = {
        "source_log": str(path.resolve()),
        "sample_bins": len(rows),
        "phase_order": phase_names,
        "target_v": target_v,
        "adc_mv_to_vcap_v": ADC_MV_TO_VCAP_V,
        "measurement_limitations": LIMITATION,
        "units": {
            "normalized_time_s": "seconds since CE setter returned for this phase; *_shutdown uses the CE-low setter return",
            "normalized_vcap": "estimated volts at VCAP",
            "raw_registers": "unsigned decimal bytes (CHARGE_FINAL hex decoded to integers)",
            "summary_min_max_tail": "estimated VCAP volts; tail is final 2 seconds or entire shorter phase",
            "summary_count": "ADC 16-reading averages, before 10 ms binning",
            "summary_first_window_us": "microseconds to first in-window ADC value; -1 means none",
            "summary_sensor_mv": "millivolts on the real NTC branch, including during TS override",
            "shutdown_first_last": "estimated VCAP volts after CE-low setter returned",
            "shutdown_below_1_us": "microseconds after CE-low setter returned until first reading below 1 V; -1 means none",
        },
        "phase_settings": phase_begins,
        "phase_summaries": summaries,
        "shutdown_observations": shutdowns,
        "final_states": finals,
        "results": results,
        "faults": faults,
        "has_final_result": bool(results),
    }
    return rows, document


def plot_rows(rows, document, stem):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as error:
        raise RuntimeError("--plot requires matplotlib; CSV and JSON were still exported") from error
    if not rows:
        raise RuntimeError("no ADC samples are available to plot; CSV and JSON were still exported")
    phase_names = document["phase_order"]
    ncols = min(2, len(phase_names))
    nrows = math.ceil(len(phase_names) / ncols)
    figure, axes = plt.subplots(nrows, ncols, figsize=(12, 2.7 * nrows + 1.0),
                                squeeze=False, sharey=True)
    summaries = {item["phase"]: item for item in document["phase_summaries"]}
    shutdowns = {item["phase"] + "_shutdown": item for item in document["shutdown_observations"]}
    target_v = document["target_v"]
    max_v = max(target_v, max(row["vcap_max_v"] for row in rows))
    handles = None
    for axis, phase_name in zip(axes.flat, phase_names):
        phase = [row for row in rows if row["phase"] == phase_name]
        times = [row["time_s"] for row in phase]
        low = [row["vcap_min_v"] for row in phase]
        high = [row["vcap_max_v"] for row in phase]
        mean = [row["vcap_mean_v"] for row in phase]
        band = axis.fill_between(times, low, high, color="#007f86", alpha=0.24,
                                 label="10 ms bin min–max")
        line, = axis.plot(times, mean, color="#007f86", linewidth=1.3, label="10 ms bin mean")
        target = axis.axhline(target_v, color="#a85817", linestyle="--", linewidth=1.0,
                             label=f"Target {target_v:.3f} V")
        handles = [line, band, target]
        summary = summaries.get(phase_name, {})
        shutdown = shutdowns.get(phase_name)
        verdict = summary.get("result", "OBSERVED" if shutdown is not None else "INCOMPLETE")
        axis.set_title(f"{phase_name} — {verdict}", loc="left", fontsize=10)
        time_label = "Time since CE-low setter returned (s)" if phase_name.endswith("_shutdown") else "Time since CE setter returned (s)"
        axis.set(xlabel=time_label, ylabel="VCAP estimate (V)",
                 ylim=(-0.6, max_v + 1.5), xlim=(0, max(times[-1], 0.01)))
        axis.grid(alpha=0.22)
        tail = summary.get("tail_mean")
        if isinstance(tail, (int, float)):
            axis.text(0.98, 0.06, f"Tail mean: {tail:.3f} V", transform=axis.transAxes,
                      ha="right", va="bottom", fontsize=9)
        elif shutdown is not None:
            first, last = shutdown.get("first"), shutdown.get("last")
            if isinstance(first, (int, float)) and isinstance(last, (int, float)):
                axis.text(0.98, 0.06, f"First / last: {first:.3f} / {last:.3f} V",
                          transform=axis.transAxes, ha="right", va="bottom", fontsize=9)
    for axis in list(axes.flat)[len(phase_names):]:
        axis.set_visible(False)
    result = document["results"][-1]["result"] if document["results"] else "INCOMPLETE CAPTURE"
    figure.suptitle(f"Unloaded charger / thermistor selection — {result}", fontsize=14, y=0.993)
    figure.legend(handles=handles, loc="upper center", bbox_to_anchor=(0.5, 0.973),
                  ncol=3, frameon=False)
    figure.text(0.02, 0.008,
                "ESP32 ADC × 9.2 divider; uncalibrated estimate. Bins contain 16-reading averages.\n"
                "RC filtering, CE-call latency and polling limit transient capture; switching ripple and peak overshoot are unresolved.",
                fontsize=8, color="#444444", va="bottom")
    figure.tight_layout(rect=(0, 0.052, 1, 0.945))
    for suffix in (".png", ".svg"):
        output = stem.parent / (stem.name + suffix)
        figure.savefig(output, dpi=180, facecolor="white")
        print(output)
    plt.close(figure)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="serial log from the charger-deep command")
    parser.add_argument("--output-dir", type=Path, help="defaults to the log's directory")
    parser.add_argument("--plot", action="store_true", help="also create PNG/SVG plots; requires matplotlib")
    args = parser.parse_args()
    try:
        rows, document = parse_log(args.log)
        output_dir = args.output_dir or args.log.parent
        output_dir.mkdir(parents=True, exist_ok=True)
        stem = output_dir / args.log.stem
        csv_path = output_dir / (stem.name + ".csv")
        json_path = output_dir / (stem.name + ".summary.json")
        if args.log.resolve() in (csv_path.resolve(), json_path.resolve()):
            raise ValueError("export path would overwrite the source log; use --output-dir")
        with csv_path.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=NORMALIZED_COLUMNS)
            writer.writeheader()
            writer.writerows(rows)
        json_path.write_text(json.dumps(document, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        print(f"Exported {len(rows)} ADC bins across {len(document['phase_order'])} phases")
        print(csv_path)
        print(json_path)
        for summary in document["phase_summaries"]:
            print(f"{summary['phase']}: {summary['result']} "
                  f"tail={summary.get('tail_min')}..{summary.get('tail_max')} V, "
                  f"mean={summary.get('tail_mean')} V")
        if not document["has_final_result"]:
            print("Warning: no RESULT CHARGER_DEEP record; capture may be incomplete", file=sys.stderr)
        if args.plot:
            plot_rows(rows, document, stem)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
