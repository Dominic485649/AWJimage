"""Verify generated HEVC chroma/precision/CICP through the actual AWJ CLI."""
import argparse
import csv
import hashlib
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    binary, fixtures, output = args.binary.resolve(), args.fixtures.resolve(), args.output.resolve()
    if not args.verify_only:
        output.mkdir(parents=True, exist_ok=False)
    spec = json.loads((fixtures / "fixtures.json").read_text(encoding="utf-8"))
    results = []
    for fmt in ("png", "avif"):
        target = output / fmt
        command = [str(binary), "-i", str(fixtures), "-o", str(target), "-f", fmt,
                   "-q", "100" if fmt == "png" else "80", "-t", "2", "--summary", "--no-log"]
        if fmt == "avif":
            command += ["--speed", "8"]
        if not args.verify_only:
            converted = subprocess.run(command, capture_output=True, timeout=120)
            (output / f"{fmt}.log").write_bytes(converted.stdout + converted.stderr)
            assert converted.returncode == 0, (fmt, converted.returncode)
        with (target / "summary.csv").open(encoding="utf-8-sig", newline="") as source:
            rows = {row["input"]: row for row in csv.DictReader(source)}
        assert len(rows) == len(spec["cases"]), (fmt, len(rows))
        for case in spec["cases"]:
            row = rows[case["name"]]
            assert row["status"] == "ok" and row["decoder_id"] == "libheif-libde265", row
            assert row["input_sha256"] == case["sha256"], row
            assert row["source_chroma"] == case["chroma"], row
            assert int(row["source_bit_depth"]) == case["depth"], row
            for label, value in (("color_primaries", case["primaries"]),
                                 ("transfer_characteristics", case["transfer"]), ("color_range", 1)):
                assert int(row["source_" + label]) == value, (case, label, row)
                assert int(row["applied_" + label]) == value, (case, label, row)
            if fmt == "avif":
                assert row["applied_chroma"] == case["chroma"], row
                # Lossy AVIF auto selects at least 10-bit; source precision must never fall.
                assert int(row["applied_bit_depth"]) == max(10, case["depth"]), row
            else:
                assert int(row["applied_bit_depth"]) == (8 if case["depth"] == 8 else 16), row
            probe = subprocess.run([args.ffprobe, "-v", "error", "-show_streams", "-of", "json",
                                    str(target / row["output"])], capture_output=True, check=True)
            stream = json.loads(probe.stdout)["streams"][0]
            assert (stream["width"], stream["height"]) == (case["width"], case["height"]), stream
            results.append({"case": case["name"], "format": fmt, "input_sha256": case["sha256"],
                            "output_sha256": row["output_sha256"], "stream": stream,
                            "source_chroma": row["source_chroma"], "source_depth": row["source_bit_depth"],
                            "source_transfer": row["source_transfer_characteristics"]})
    (output / "results.json").write_text(json.dumps({
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "results": results},
        indent=2), encoding="utf-8")
    print(f"Passed {len(results)} HEVC conversions with chroma, precision, CICP and dimensions verified.")


if __name__ == "__main__":
    main()
