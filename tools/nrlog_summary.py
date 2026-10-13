#!/usr/bin/env python3
"""
nrlog_summary.py - Summary and comparison of OptiScaler logs from runs with DLSS-NR.

Usage:
    python3 tools/nrlog_summary.py <OptiScaler.log> [...]
    python3 tools/nrlog_summary.py --compare <LogA> <LogB>
"""

import argparse
import sys
import os
import re
import math
from collections import Counter, defaultdict

TS_RE = re.compile(r"^\[(\d{2}:\d{2}:\d{2}(?:\.\d+)?)\]")
COVERAGE_RE = re.compile(
    r"DLSS-NR coverage stage=(\S+).*?calls=(\d+).*?model_ok=(\d+).*?model_failed=(\d+).*?skipped=(\d+).*?fallback=(\d+)"
)
TRANSFER_STATE_RE = re.compile(r"PublishEnlarge DLSS-NR enlargement: ([^\s;]+) \(was ([^\)]+)\)")
COMP_TRANSFER_RE = re.compile(r"transfer\s+(.+?),\s+model\s")  # "matched residual, sharp" carries a comma
COMP_MODEL_RE = re.compile(r"model\s+([^,]+)")
DLSSG_MODE_RE = re.compile(r"mode\s+([^\s]+)")
STREAMLINE_REPEAT_RE = re.compile(r"repeated (\d+) more times since the previous line")

# DLSS-NR gpu timing confirmed: run=... stage=... carried=... dispatch_ms=... model_ms=...
GPU_TIMING_CONFIRMED_RE = re.compile(
    r"DLSS-NR gpu timing confirmed:.*?stage=(\S+).*?carried=(\S+).*?dispatch_ms=([0-9.]+).*?model_ms=([0-9.]+)"
)

PROBLEM_KEYWORDS = ["faulted", "device removed", "DEVICE_REMOVED", "hung", "0x887A"]

def parse_time_to_seconds(ts_str):
    try:
        parts = ts_str.split(":")
        h = int(parts[0])
        m = int(parts[1])
        s_parts = parts[2].split(".")
        s = int(s_parts[0])
        us = 0
        if len(s_parts) > 1:
            us_str = (s_parts[1] + "000000")[:6]
            us = int(us_str)
        return h * 3600 + m * 60 + s + us / 1e6
    except Exception:
        return None

def format_duration(seconds):
    if seconds is None:
        return "unknown"
    if seconds < 0:
        seconds += 24 * 3600  # crossed midnight
    h = int(seconds // 3600)
    m = int((seconds % 3600) // 60)
    s = seconds % 60
    if h > 0:
        return f"{h}h {m:02d}m {s:04.1f}s ({seconds:.2f}s)"
    return f"{m}m {s:04.1f}s ({seconds:.2f}s)"

def extract_ts(line):
    m = TS_RE.match(line)
    return m.group(1) if m else None

def strip_ts(line):
    return TS_RE.sub("", line).strip()

def calc_stats(values):
    if not values:
        return None
    s = sorted(values)
    n = len(s)
    avg = sum(s) / n
    if n % 2 == 1:
        med = s[n // 2]
    else:
        med = (s[n // 2 - 1] + s[n // 2]) / 2.0
    p95_idx = int(math.ceil(0.95 * n)) - 1
    p95 = s[max(0, min(n - 1, p95_idx))]
    return {"avg": avg, "med": med, "p95": p95, "count": n}

def parse_log(filepath):
    if not os.path.exists(filepath):
        return None

    data = {
        "filepath": filepath,
        "build_info": None,
        "first_ts": None,
        "last_ts": None,
        "first_line_raw": None,
        "last_line_raw": None,
        "coverage": None,
        "coverage_raw": None,
        "transfer2_events": [],
        "comp_changes": [],
        "cadence_lines": [],
        "periphery_lines": [],
        "dlssg_transitions": [],
        "problems": Counter(),
        "sl_suppressed_lines": 0,
        "sl_suppressed_total": 0,
        "gpu_timing_samples": [],
    }

    last_comp_key = None
    last_dlssg_mode = None

    with open(filepath, "r", errors="replace") as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line:
                continue

            if data["first_line_raw"] is None:
                data["first_line_raw"] = line
            data["last_line_raw"] = line

            ts = extract_ts(line)
            if ts:
                if data["first_ts"] is None:
                    data["first_ts"] = ts
                data["last_ts"] = ts

            # Build
            if not data["build_info"] and "loaded" in line:
                m_build = re.search(r"(OptiScaler v[^\s]+ \([0-9a-fA-F]+\) \(\d+_\d+\) loaded)", line)
                if m_build:
                    data["build_info"] = m_build.group(1)

            # Coverage
            if "DLSS-NR coverage stage=" in line:
                data["coverage_raw"] = line
                m_cov = COVERAGE_RE.search(line)
                if m_cov:
                    stage, calls, ok, failed, skipped, fallback = m_cov.groups()
                    data["coverage"] = {
                        "stage": stage,
                        "calls": int(calls),
                        "model_ok": int(ok),
                        "model_failed": int(failed),
                        "skipped": int(skipped),
                        "fallback": int(fallback),
                    }

            # Transfer 2
            if "PublishEnlarge DLSS-NR enlargement:" in line:
                m_tstate = TRANSFER_STATE_RE.search(line)
                if m_tstate:
                    state, was = m_tstate.group(1), m_tstate.group(2)
                    data["transfer2_events"].append((ts, f"State: {state} (was {was})"))
            elif "BuildEnlarger" in line or "EnlargeEdit" in line:
                msg = strip_ts(line)
                clean_msg = re.sub(r"^\[.*?\]\s*`?anonymous-namespace'?::", "", msg)
                data["transfer2_events"].append((ts, clean_msg))

            # Composition
            if "DLSS-NR composition:" in line:
                m_trans = COMP_TRANSFER_RE.search(line)
                m_mod = COMP_MODEL_RE.search(line)
                transfer_val = m_trans.group(1).strip() if m_trans else "unknown"
                model_val = m_mod.group(1).strip() if m_mod else "unknown"
                key = (transfer_val, model_val)
                if key != last_comp_key:
                    data["comp_changes"].append((ts, transfer_val, model_val))
                    last_comp_key = key

            # Cadence and periphery
            if "DLSS-NR cadence:" in line:
                data["cadence_lines"].append((ts, strip_ts(line)))
            if "DLSS-NR periphery:" in line:
                data["periphery_lines"].append((ts, strip_ts(line)))

            # DLSS-G
            if "DLSSG override state:" in line:
                m_gmode = DLSSG_MODE_RE.search(line)
                if m_gmode:
                    mode = m_gmode.group(1).strip()
                    if mode != last_dlssg_mode:
                        data["dlssg_transitions"].append((ts, mode, strip_ts(line)))
                        last_dlssg_mode = mode

            # Problems
            is_problem = False
            if "[E]" in line and "streamlineLogCallback" not in line:
                is_problem = True
            else:
                for kw in PROBLEM_KEYWORDS:
                    if kw in line:
                        is_problem = True
                        break
            if is_problem:
                data["problems"][strip_ts(line)] += 1

            # Streamline suppressions
            m_rep = STREAMLINE_REPEAT_RE.search(line)
            if m_rep:
                data["sl_suppressed_lines"] += 1
                data["sl_suppressed_total"] += int(m_rep.group(1))

            # Confirmed GPU timing
            if "DLSS-NR gpu timing confirmed:" in line:
                m_gt = GPU_TIMING_CONFIRMED_RE.search(line)
                if m_gt:
                    g_stage = m_gt.group(1)
                    g_carried_str = m_gt.group(2).lower()
                    g_carried = g_carried_str in ["true", "1"]
                    g_dispatch_ms = float(m_gt.group(3))
                    g_model_ms = float(m_gt.group(4))
                    data["gpu_timing_samples"].append({
                        "ts": ts,
                        "stage": g_stage,
                        "carried": g_carried,
                        "dispatch_ms": g_dispatch_ms,
                        "model_ms": g_model_ms,
                    })

    # Duration
    dur_sec = None
    if data["first_ts"] and data["last_ts"]:
        t1 = parse_time_to_seconds(data["first_ts"])
        t2 = parse_time_to_seconds(data["last_ts"])
        if t1 is not None and t2 is not None:
            dur_sec = t2 - t1
            if dur_sec < 0:
                dur_sec += 24 * 3600
    data["duration_sec"] = dur_sec
    data["duration_str"] = format_duration(dur_sec)

    return data

def print_single_report(d):
    print("=" * 80)
    print(f"Log: {d['filepath']}")
    print("=" * 80)

    # Build
    print(f"* Build: {d['build_info'] or 'not found in the log'}")

    # Time window
    print(f"* Time window: {d['first_ts'] or 'N/A'} -> {d['last_ts'] or 'N/A'} (duration: {d['duration_str']})")

    # NR coverage
    print("* DLSS-NR coverage (last record):")
    if d["coverage"]:
        cov = d["coverage"]
        print(f"  - stage: {cov['stage']}")
        print(f"  - calls: {cov['calls']}")
        print(f"  - model_ok: {cov['model_ok']}")
        print(f"  - model_failed: {cov['model_failed']}")
        print(f"  - skipped: {cov['skipped']}")
        print(f"  - fallback: {cov['fallback']}")
    elif d["coverage_raw"]:
        print(f"  {d['coverage_raw']}")
    else:
        print("  No DLSS-NR coverage record found.")

    # Transfer 2
    print("* Transfer 2 (transitions and events):")
    if d["transfer2_events"]:
        for ev_ts, ev_text in d["transfer2_events"]:
            print(f"  [{ev_ts or 'N/A'}] {ev_text}")
    else:
        print("  No Transfer 2 events.")

    # Composition
    print("* DLSS-NR composition (transfer and model changes):")
    if d["comp_changes"]:
        for c_ts, c_trans, c_mod in d["comp_changes"]:
            print(f"  [{c_ts or 'N/A'}] transfer: '{c_trans}', model: {c_mod}")
    else:
        print("  No composition change recorded.")

    # Cadence
    print("* DLSS-NR cadence:")
    if d["cadence_lines"]:
        for cad_ts, cad_text in d["cadence_lines"]:
            print(f"  [{cad_ts or 'N/A'}] {cad_text}")
    else:
        print("  No cadence record.")

    # Periphery
    print("* DLSS-NR periphery:")
    if d["periphery_lines"]:
        for per_ts, per_text in d["periphery_lines"]:
            print(f"  [{per_ts or 'N/A'}] {per_text}")
    else:
        print("  No periphery record.")

    # DLSS-G
    print("* DLSS-G (mode transitions):")
    if d["dlssg_transitions"]:
        for g_ts, g_mode, g_line in d["dlssg_transitions"]:
            print(f"  [{g_ts or 'N/A'}] mode -> {g_mode} ({g_line})")
    else:
        print("  No DLSS-G transition recorded.")

    # Problems
    print(f"* Problems / errors ({sum(d['problems'].values())} occurrence(s)):")
    if d["problems"]:
        for text, count in d["problems"].most_common():
            print(f"  [{count}x] {text}")
    else:
        print("  No error or critical failure found.")

    # Streamline suprimido
    print(f"* Streamline filter: {d['sl_suppressed_lines']} folded line(s), {d['sl_suppressed_total']} repeat(s) suppressed.")

    # Confirmed GPU timing
    print("* Confirmed GPU timing samples (NR GPU timing):")
    if d["gpu_timing_samples"]:
        samples = d["gpu_timing_samples"]
        print(f"  Confirmed samples in total: {len(samples)}")
        groups = defaultdict(lambda: {"dispatch": [], "model": []})
        for s in samples:
            key = (s["stage"], s["carried"])
            groups[key]["dispatch"].append(s["dispatch_ms"])
            groups[key]["model"].append(s["model_ms"])

        for (stage, carried), times in sorted(groups.items()):
            kind_str = "frame carried by the cadence (carried=true)" if carried else "frame run through the model (carried=false)"
            disp_st = calc_stats(times["dispatch"])
            mod_st = calc_stats(times["model"])
            print(f"  [{stage}] {kind_str} (N={disp_st['count']}):")
            print(f"    - Total time (dispatch_ms): mean={disp_st['avg']:.3f} ms | median={disp_st['med']:.3f} ms | p95={disp_st['p95']:.3f} ms")
            print(f"    - Model time (model_ms):      mean={mod_st['avg']:.3f} ms | median={mod_st['med']:.3f} ms | p95={mod_st['p95']:.3f} ms")
    else:
        print("  No confirmed GPU timing sample in the log.")
    print()

def compare_logs(path_a, path_b):
    da = parse_log(path_a)
    db = parse_log(path_b)

    if not da or not db:
        print("[-] Could not load one of the logs for comparison.")
        return

    print("=" * 100)
    print(f"LOG COMPARISON")
    print(f"Log A: {path_a}")
    print(f"Log B: {path_b}")
    print("=" * 100)

    rows = []

    # 1. Build
    rows.append(("Build", da["build_info"] or "N/A", db["build_info"] or "N/A"))

    # 2. Duration and window
    rows.append(("Time window", f"{da['first_ts']} -> {da['last_ts']}", f"{db['first_ts']} -> {db['last_ts']}"))
    rows.append(("Duration", da["duration_str"], db["duration_str"]))

    # 3. Coverage
    if da["coverage"]:
        ca = da["coverage"]
        cov_a_str = f"stage={ca['stage']} calls={ca['calls']} ok={ca['model_ok']} fail={ca['model_failed']} skip={ca['skipped']}"
    else:
        cov_a_str = "N/A"
    if db["coverage"]:
        cb = db["coverage"]
        cov_b_str = f"stage={cb['stage']} calls={cb['calls']} ok={cb['model_ok']} fail={cb['model_failed']} skip={cb['skipped']}"
    else:
        cov_b_str = "N/A"
    rows.append(("NR coverage", cov_a_str, cov_b_str))

    # 4. Effective transfer
    trans_a = ", ".join([f"'{t[1]}'" for t in da["comp_changes"]]) if da["comp_changes"] else "N/A"
    trans_b = ", ".join([f"'{t[1]}'" for t in db["comp_changes"]]) if db["comp_changes"] else "N/A"
    rows.append(("Effective transfer", trans_a, trans_b))

    # 5. Periphery
    per_a = da["periphery_lines"][-1][1][:45] if da["periphery_lines"] else "Inactive / no records"
    per_b = db["periphery_lines"][-1][1][:45] if db["periphery_lines"] else "Inactive / no records"
    rows.append(("Periphery", per_a, per_b))

    # 6. Cadence
    cad_a = da["cadence_lines"][-1][1][:45] if da["cadence_lines"] else "Default / no records"
    cad_b = db["cadence_lines"][-1][1][:45] if db["cadence_lines"] else "Default / no records"
    rows.append(("Cadence", cad_a, cad_b))

    # 7. DLSS-G
    g_a = " -> ".join([m[1] for m in da["dlssg_transitions"]]) if da["dlssg_transitions"] else "None"
    g_b = " -> ".join([m[1] for m in db["dlssg_transitions"]]) if db["dlssg_transitions"] else "None"
    rows.append(("DLSS-G modes", g_a, g_b))

    # 8. Errors
    err_a = f"{sum(da['problems'].values())} error(s)"
    err_b = f"{sum(db['problems'].values())} error(s)"
    rows.append(("Problems / errors", err_a, err_b))

    # 9. Streamline filter
    sl_a = f"{da['sl_suppressed_lines']} folded ({da['sl_suppressed_total']} suppressed)"
    sl_b = f"{db['sl_suppressed_lines']} folded ({db['sl_suppressed_total']} suppressed)"
    rows.append(("Streamline filter", sl_a, sl_b))

    # 10. GPU Timing
    def fmt_timing_summary(d):
        if not d["gpu_timing_samples"]:
            return "No samples"
        s = d["gpu_timing_samples"]
        st = calc_stats([x["dispatch_ms"] for x in s])
        return f"N={st['count']}, disp_avg={st['avg']:.2f}ms p95={st['p95']:.2f}ms"

    rows.append(("GPU Timing", fmt_timing_summary(da), fmt_timing_summary(db)))

    col1_w = 20
    col2_w = 38
    col3_w = 38

    print(f"{'Metric':<{col1_w}} | {'Log A':<{col2_w}} | {'Log B':<{col3_w}}")
    print("-" * (col1_w + col2_w + col3_w + 6))
    for metric, val_a, val_b in rows:
        # truncate if longer
        val_a_s = (val_a[:col2_w - 3] + "...") if len(val_a) > col2_w else val_a
        val_b_s = (val_b[:col3_w - 3] + "...") if len(val_b) > col3_w else val_b
        print(f"{metric:<{col1_w}} | {val_a_s:<{col2_w}} | {val_b_s:<{col3_w}}")
    print("=" * 100)
    print()

def main():
    ap = argparse.ArgumentParser(
        description="Summarise, or compare two, OptiScaler.log files from runs with DLSS-NR.")
    ap.add_argument("logs", nargs="*", metavar="LOG",
                    help="OptiScaler.log file(s), or directories whose *.log files are summarised")
    ap.add_argument("--compare", nargs=2, metavar=("LOG_A", "LOG_B"),
                    help="print a side-by-side comparison of two logs")
    args = ap.parse_args()

    if args.compare:
        compare_logs(*args.compare)
        return
    if not args.logs:
        ap.print_usage(sys.stderr)
        sys.exit(1)

    for target in args.logs:
        if os.path.isdir(target):
            log_files = [os.path.join(target, f) for f in sorted(os.listdir(target)) if f.endswith(".log")]
            if not log_files:
                print(f"[-] No .log file found in directory: {target}")
            for lf in log_files:
                d = parse_log(lf)
                if d:
                    print_single_report(d)
        else:
            d = parse_log(target)
            if d:
                print_single_report(d)

if __name__ == "__main__":
    main()
