#!/usr/bin/env python3
"""Run/validate the API-neutral workload contract and retain raw performance data."""
import argparse
import datetime as dt
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def command(args, cwd=ROOT):
    result = subprocess.run(args, cwd=cwd, text=True, encoding="utf-8", errors="replace",
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    return {"command": [str(a) for a in args], "returncode": result.returncode,
            "output": result.stdout.strip()}


def source_snapshot():
    paths = [ROOT / "CMakeLists.txt"]
    for folder in ("src", "include", "shaders", "cmake", "tools", "benchmarks"):
        paths.extend(p for p in (ROOT / folder).rglob("*")
                     if p.is_file() and p.suffix in (".cpp", ".hpp", ".comp", ".vert", ".frag", ".py", ".in", ".cmake")
                     and "baselines" not in p.parts)
    files = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in sorted(set(paths))}
    digest = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return {"sha256": digest, "files": files}


def telemetry():
    try:
        return command(["nvidia-smi", "--query-gpu=name,driver_version,pstate,temperature.gpu,"
                        "power.draw,power.limit,clocks.gr,clocks.mem,utilization.gpu",
                        "--format=csv"])
    except OSError as e:
        return {"unavailable": str(e)}


def stats(values):
    values = sorted(values)
    if not values or any(not math.isfinite(x) for x in values):
        raise ValueError("empty or nonfinite metric")
    # Nearest-rank percentiles: documented and identical for every backend.
    percentile = lambda p: values[max(0, math.ceil(p * len(values)) - 1)]
    return {"count": len(values), "min": values[0], "mean": statistics.fmean(values),
            "p50": percentile(.5), "p95": percentile(.95), "p99": percentile(.99), "max": values[-1]}


def validate(raw):
    if raw["schema_version"] != 1 or raw["workload_version"] != 1:
        raise ValueError("unsupported benchmark contract version")
    measurement = raw.get("measurement_version", 1)
    scheduling = raw.get("scheduling", "sync")
    if measurement not in (1, 2) or scheduling not in ("sync", "gpu"):
        raise ValueError("unsupported scheduling/measurement version")
    if scheduling == "gpu" and measurement < 2:
        raise ValueError("GPU scheduling requires deferred population capture")
    if measurement == 2 and raw.get("population_capture") != "gpu_history_after_window":
        raise ValueError("missing deferred population capture")
    if raw["build_type"].lower() != "release":
        raise ValueError("baseline must use a Release build")
    seen = set()
    for case in raw["cases"]:
        key = case["scenario"], case["capacity"], case["repeat"]
        if key in seen:
            raise ValueError(f"duplicate case: {key}")
        seen.add(key)
        samples = case["samples"]
        if len(samples) != raw["measured_frames"] or case["preflight_changed_pixels"] <= 0:
            raise ValueError(f"incomplete/empty case: {key}")
        if not math.isfinite(case["window_wall_ms"]) or case["window_wall_ms"] <= 0:
            raise ValueError("invalid completed-window time")
        for i, sample in enumerate(samples):
            if sample["frame"] != i or not 0 <= sample["alive"] <= case["capacity"]:
                raise ValueError(f"invalid frame/population: {key}")
            for name in ("cpu_frame_ms", "cpu_update_ms", "cpu_render_ms"):
                if name not in sample:
                    raise ValueError(f"missing {name}")
            for name, value in sample.items():
                if name.endswith("_ms") and (not math.isfinite(value) or value < 0):
                    raise ValueError(f"invalid duration: {name}")
            if sample["cpu_frame_ms"] + 1e-6 < sample["cpu_update_ms"] + sample["cpu_render_ms"]:
                raise ValueError("CPU frame does not contain update/render")
            if raw["gpu_timers"] and sample.get("gpu_frame_span_ms", 0) <= 0:
                raise ValueError("missing/zero GPU frame timestamp interval")
            if raw["variant"] == "stages":
                for name in ("cpu_readback_ms", "cpu_integrate_ms", "cpu_spawn_ms", "cpu_build_live_ms"):
                    if name not in sample:
                        raise ValueError(f"stage instrumentation not active: {name}")
                if sample["cpu_readback_ms"] > sample["cpu_update_ms"] + 1e-6:
                    raise ValueError("readback exceeds enclosing update")
                if sample["alive"]:
                    required = {"sort": ("sort",), "bloom": ("bloom_prepare", "bloom_draw", "bloom_post")}
                    stages = required.get(case["scenario"], ())
                    if case["scenario"].startswith("refraction_"):
                        stages = ("refraction",)
                    for stage in stages:
                        if f"cpu_{stage}_ms" not in sample:
                            raise ValueError(f"missing feature stage: {stage}")
            if case["scenario"] == "burst" and sample["cycle_frame"] != i % 120:
                raise ValueError("burst timeline mismatch")
    if not seen:
        raise ValueError("no benchmark cases")


def aggregate(raw):
    validate(raw)
    groups = {}
    for case in raw["cases"]:
        key = case["scenario"], case["capacity"]
        groups.setdefault(key, []).append(case)
    output = []
    for (scenario, capacity), cases in sorted(groups.items()):
        if sorted(c["repeat"] for c in cases) != list(range(raw["repeats"])):
            raise ValueError("missing repeat")
        samples = [s for c in cases for s in c["samples"]]
        metrics = {k: stats([s[k] for s in samples if k in s])
                   for k in sorted({k for s in samples for k in s if k.endswith("_ms")})}
        repeats = [{"repeat": c["repeat"],
                    "completed_wall_ms_per_frame": c["window_wall_ms"] / len(c["samples"]),
                    "cpu_frame_p50_ms": stats([s["cpu_frame_ms"] for s in c["samples"]])["p50"]}
                   for c in cases]
        item = {"scenario": scenario, "capacity": capacity,
                "alive": stats([s["alive"] for s in samples]), "metrics": metrics, "repeats": repeats,
                "completed_wall_ms_per_frame": stats([r["completed_wall_ms_per_frame"] for r in repeats])}
        if scenario == "burst":
            item["cycle_phases"] = {}
            for name, lo, hi in (("fill", 0, 10), ("hold", 10, 30), ("retire", 30, 41), ("empty", 41, 120)):
                phase = [s for s in samples if lo <= s["cycle_frame"] < hi]
                if phase:
                    item["cycle_phases"][name] = {
                        "alive": stats([s["alive"] for s in phase]),
                        "cpu_frame_ms": stats([s["cpu_frame_ms"] for s in phase]),
                        **({"gpu_frame_span_ms": stats([s["gpu_frame_span_ms"] for s in phase])}
                           if raw["gpu_timers"] else {})}
        output.append(item)
    return {"environment": {k: v for k, v in raw.items() if k != "cases"}, "cases": output}


def markdown(summary):
    first = next(iter(summary["runs"].values()))["environment"]
    lines = ["# ember OpenGL performance baseline", "",
             f"- UTC: {summary['manifest']['started_utc']}",
             f"- GPU: {first['gl_renderer']}; driver/context: {first['gl_version']}",
             f"- Build: {first['compiler']}, {first['build_type']}, {first['system']}",
             f"- Workload v{first['workload_version']}; {first['width']}x{first['height']}, "
             f"{first['target_format']}, no MSAA, no presentation",
             f"- Fixed dt: 1/60 s; seed {first['seed']}; warmup {first['warmup_frames']} frames; "
             f"{first['measured_frames']} measured frames x {first['repeats']} repeats per case",
             f"- Source SHA-256: {summary['manifest']['source']['sha256']}", "",
             f"Scheduling: {first.get('scheduling', 'sync')}; measurement v{first.get('measurement_version', 1)}.",
             "In measurement v2, population is copied to GPU history per frame and read after the window. "
             "History copies are included in completed wall/frame, outside per-frame spans.",
             "cpu_readback_ms means synchronous counter read in sync mode, or ready-snapshot polling "
             "and copy/fence enqueue in gpu mode.",
             "All durations are milliseconds. Percentiles use nearest rank over recorded frames.",
             "CPU call time includes driver/GPU waits; GPU span includes GPU idle time between "
             "timestamp commands. Neither is a pure kernel sum or a displayed frame rate.",
             "Completed wall/frame includes target clear, submission, query collection and the final "
             "GPU drain. Repeats run serially; see raw data and telemetry for environmental variation.", ""]
    for label, run in summary["runs"].items():
        lines += [f"## {label}", "",
                  "| Scenario | Capacity | Alive min-max | CPU frame p50 / p95 / p99 | "
                  "GPU span p50 / p95 | Completed wall/frame mean |",
                  "|---|---:|---:|---:|---:|---:|"]
        for c in run["cases"]:
            cpu = c["metrics"]["cpu_frame_ms"]
            gpu = c["metrics"].get("gpu_frame_span_ms")
            gpu_text = f"{gpu['p50']:.3f} / {gpu['p95']:.3f}" if gpu else "disabled"
            lines.append(f"| {c['scenario']} | {c['capacity']} | "
                         f"{c['alive']['min']}-{c['alive']['max']} | "
                         f"{cpu['p50']:.3f} / {cpu['p95']:.3f} / {cpu['p99']:.3f} | "
                         f"{gpu_text} | {c['completed_wall_ms_per_frame']['mean']:.3f} |")
        if label == "stages":
            names = ["gpu_integrate_ms", "gpu_spawn_ms", "gpu_build_live_ms",
                     "cpu_readback_ms", "gpu_sort_ms", "gpu_bloom_prepare_ms", "gpu_bloom_draw_ms",
                     "gpu_bloom_post_ms", "gpu_refraction_ms"]
            lines += ["", "Diagnostic stage p50 (nested spans; do not add to frame time):", "",
                      "| Scenario | Capacity | " + " | ".join(names) + " |",
                      "|---|---:|" + "---:|" * len(names)]
            for c in run["cases"]:
                values = [f"{c['metrics'][n]['p50']:.3f}" if n in c["metrics"] else "-"
                          for n in names]
                lines.append(f"| {c['scenario']} | {c['capacity']} | " + " | ".join(values) + " |")
        lines.append("")
    if "plain" in summary["runs"] and "plain_no_timers" in summary["runs"]:
        control = {(c["scenario"], c["capacity"]): c for c in summary["runs"]["plain_no_timers"]["cases"]}
        lines += ["## Timestamp overhead control", "",
                  "Ratio of CPU frame medians, timer-enabled plain / timer-disabled plain. "
                  "Serial runs also include clock/thermal/system variation; this is not an exact "
                  "subtraction of query cost.", "",
                  "| Scenario | Capacity | CPU p50 ratio |", "|---|---:|---:|"]
        for c in summary["runs"]["plain"]["cases"]:
            other = control[(c["scenario"], c["capacity"])]
            ratio = c["metrics"]["cpu_frame_ms"]["p50"] / other["metrics"]["cpu_frame_ms"]["p50"]
            lines.append(f"| {c['scenario']} | {c['capacity']} | {ratio:.3f} |")
        lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--variants", default="plain,stages,plain_no_timers")
    parser.add_argument("--counts", default="10000,100000,1000000")
    parser.add_argument("--scenarios", default="dense,forces,churn,burst,sparse,sparse_control,sort,bloom,"
                        "refraction_simple,refraction_depth,refraction_noise")
    parser.add_argument("--frames", type=int, default=240)
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--seed", type=int, default=20260917)
    parser.add_argument("--scheduling", choices=("sync", "gpu"), default="sync")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--note", default="", help="power mode, AC/battery, other workload, etc.")
    args = parser.parse_args()
    variants = args.variants.split(",")
    if len(set(variants)) != len(variants) or any(v not in ("plain", "stages", "plain_no_timers") for v in variants):
        parser.error("variants must be unique: plain,stages,plain_no_timers")
    output = args.output.resolve()
    # Refuse stale mixtures or accidental destruction of an earlier baseline.
    output.mkdir(parents=True, exist_ok=False)
    manifest = {"started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "host": platform.platform(), "cpu": platform.processor(), "logical_cpus": os.cpu_count(),
                "python": platform.python_version(), "note": args.note,
                "git_head": command(["git", "rev-parse", "HEAD"]),
                "git_status": command(["git", "status", "--porcelain"]),
                "source": source_snapshot(), "runs": {}}
    manifest["build"] = command(["cmake", "--build", str(args.build_dir.resolve()),
                                "--config", "Release", "--target",
                                "ember_benchmark_plain", "ember_benchmark_stages", "-j", "4"])
    if manifest["build"]["returncode"]:
        raise RuntimeError("benchmark build failed: " + manifest["build"]["output"])
    manifest["source"] = source_snapshot()
    if os.name == "nt":
        manifest["cpu_details"] = command(["powershell", "-NoProfile", "-Command",
                                          "Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors | ConvertTo-Json -Compress"])
        manifest["power_scheme"] = command(["powercfg", "/getactivescheme"])
    cache = args.build_dir.resolve() / "CMakeCache.txt"
    if cache.exists():
        manifest["build_cache"] = [s for s in cache.read_text(encoding="utf-8").splitlines()
                                   if s.startswith(("CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS:",
                                                    "CMAKE_CXX_FLAGS_RELEASE:", "CMAKE_BUILD_TYPE:",
                                                    "CMAKE_GENERATOR:"))]
    summary = {"manifest": manifest, "runs": {}}
    for label in variants:
        variant = "plain" if label == "plain_no_timers" else label
        exe = args.build_dir.resolve() / ("ember_benchmark_" + variant + (".exe" if os.name == "nt" else ""))
        if not exe.exists():  # Visual Studio / other multi-config generators
            exe = exe.parent / "Release" / exe.name
        raw_file = output / f"{label}.partial.json"
        cmd = [str(exe), "--output", str(raw_file), "--counts", args.counts, "--scenarios", args.scenarios,
               "--frames", str(args.frames), "--warmup", str(args.warmup), "--repeats", str(args.repeats),
               "--seed", str(args.seed), "--width", str(args.width), "--height", str(args.height),
               "--shader-dir", str(ROOT / "shaders"),
               "--scheduling", args.scheduling,
               "--gpu-timers", "off" if label == "plain_no_timers" else "on"]
        manifest["runs"][label] = {"command": cmd, "telemetry_before": telemetry(),
                                   "executable_sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
        print(f"Running {label}", flush=True)
        with (output / f"{label}.log").open("w", encoding="utf-8") as log:
            subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        data = raw_file.read_bytes()
        raw = json.loads(data)
        expected = {(s, int(n), r) for s in args.scenarios.split(",")
                    for n in args.counts.split(",") for r in range(args.repeats)}
        actual = {(c["scenario"], c["capacity"], c["repeat"]) for c in raw["cases"]}
        if actual != expected or raw["variant"] != variant or raw.get("scheduling") != args.scheduling:
            raise ValueError("run does not match requested workload matrix")
        summary["runs"][label] = aggregate(raw)
        manifest["runs"][label]["telemetry_after"] = telemetry()
        manifest["runs"][label]["raw_sha256"] = hashlib.sha256(data).hexdigest()
        (output / f"{label}.json.gz").write_bytes(gzip.compress(data, mtime=0))
        raw_file.unlink()
    if source_snapshot() != manifest["source"]:
        raise RuntimeError("benchmark source changed during run; results are not a valid baseline")
    manifest["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    (output / "report.md").write_text(markdown(summary), encoding="utf-8")
    print(f"Baseline saved: {output / 'report.md'}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"benchmark FAILED: {error}", file=sys.stderr)
        sys.exit(1)
