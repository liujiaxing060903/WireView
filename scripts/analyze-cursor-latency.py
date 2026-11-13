"""Match cursor markers to compositor presents; requested PTS is not latency."""
import argparse
import json
import os
from pathlib import Path
import re
import statistics

def analyze(path, surface_path=None, legacy_pts=False):
    lines = path.read_text(encoding="utf-8-sig", errors="replace").splitlines()
    variant = next((re.search(r"variant=([^ |]+)", line).group(1)
                    for line in lines if "Cursor diagnostics: variant=" in line), None)
    if surface_path is None:
        pid = re.search(r"-(\d+)-\d+\.log$", path.name)
        if pid:
            surface_path = Path(__file__).resolve().parents[1] / "build/latency-comparison/measurements" / f"surface-{pid.group(1)}.txt"
    presents = {}
    if surface_path is not None and surface_path.exists():
        for line in surface_path.read_text(encoding="utf-8-sig").splitlines():
            parts = line.split()
            if len(parts) == 3 and all(value.isdigit() for value in parts):
                wanted, actual, _ = map(int, parts)
                if wanted > 0 and 0 < actual < 2**63-1:
                    presents[wanted] = min(actual, presents.get(wanted, actual))
    motions = {}
    markers = matched = legacy_count = 0
    for line in lines:
        is_legacy = "Cursor latency sample:" in line
        if not ("Cursor frame marker:" in line or is_legacy):
            continue
        fields = {key: float(value) if "." in value else int(value)
                  for key, value in re.findall(r"(\w+)=(-?\d+(?:\.\d+)?)", line)}
        markers += 1
        if is_legacy:
            legacy_count += 1
            if not (legacy_pts and variant == "v1.1.20"):
                continue
            wanted = fields["framePtsUs"] * 1000
        else:
            wanted = fields["surfaceWantedNs"]
        actual = presents.get(wanted)
        if actual is None:
            continue
        matched += 1
        elapsed = (fields["observedToRenderEstimateMs"] + (actual / 1000 - fields["framePtsUs"]) / 1000
                   if is_legacy else
                   (actual / 1000 - fields["androidMinusHostUs"] - fields["observedHostUs"]) / 1000)
        sample = {"actualNs": actual, "observedToPresentEstimateMs": elapsed,
                  "clockRttMs": fields["clockRttMs"], "pollGapMs": fields["pollGapMs"]}
        motion = fields["motion"]
        if motion not in motions or actual < motions[motion]["actualNs"]:
            motions[motion] = sample
    result = {"file": str(path), "variant": variant, "surfaceFile": str(surface_path),
              "frameMarkers": markers, "compositorMatchedMarkers": matched, "samples": len(motions)}
    if legacy_count:
        result["legacyApiTimestampsAreNotMeasuredLatency"] = True
        result["legacyPtsRecoveryRequested"] = legacy_pts
    for key in ["observedToPresentEstimateMs", "clockRttMs", "pollGapMs"]:
        values = sorted(sample[key] for sample in motions.values())
        if values:
            result[key] = {"mean": round(statistics.mean(values), 3), "p50": round(statistics.median(values), 3),
                           "p95": round(values[min(len(values)-1, int(len(values)*0.95))], 3),
                           "min": round(values[0], 3), "max": round(values[-1], 3)}
    if not motions:
        result["status"] = "no compositor-matched samples; requested/callback timestamps are not latency"
    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="*")
    parser.add_argument("--surface", type=Path)
    parser.add_argument("--legacy-pts", action="store_true")
    args = parser.parse_args()
    paths = [Path(name) for name in args.logs]
    if not paths:
        paths = sorted((Path(os.environ["LOCALAPPDATA"]) / "WireView/logs").glob("performance-*.log"),
                       key=lambda path: path.stat().st_mtime, reverse=True)[:4]
    print(json.dumps([analyze(path, args.surface, args.legacy_pts) for path in paths], ensure_ascii=False, indent=2))

if __name__ == "__main__":
    main()
