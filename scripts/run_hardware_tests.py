"""Run the optional N64 hardware corpus and retain per-ROM diagnostic artifacts."""

import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
ROM_SUFFIXES = {".n64", ".z64", ".v64"}


def png_from_ppm(ppm, png):
    magic, size, maximum, rgb = ppm.read_bytes().split(b"\n", 3)
    width, height = map(int, size.split())
    if magic != b"P6" or maximum != b"255" or not (width and height) or len(rgb) != width * height * 3:
        raise ValueError("invalid VI PPM")
    pitch = width * 3
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0)
    rows = b"".join(b"\0" + rgb[y:y + pitch] for y in range(0, len(rgb), pitch))
    png.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                    chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def ocr_tool(run):
    if sys.platform == "darwin" and shutil.which("xcrun"):
        binary = run / "vision-ocr"
        try:
            subprocess.run(["xcrun", "swiftc", str(ROOT / "scripts/hardware_ocr.swift"),
                            "-o", str(binary)], check=True, capture_output=True, timeout=90)
            return lambda image: [str(binary), str(image)]
        except (OSError, subprocess.SubprocessError) as error:
            print(f"Vision OCR unavailable: {error}", file=sys.stderr)
    if shutil.which("tesseract"):
        return lambda image: ["tesseract", str(image), "stdout", "--psm", "11"]
    return None


def verdict(suite, telemetry, text):
    stop = telemetry["stop"]
    if stop == "load_error":
        return "ERROR", "ROM rejected (header, size or boot entry)"
    if stop == "execution_error":
        return "ERROR", "host execution error; see stderr"
    if suite == "dillon":
        result = telemetry["r30"]
        if stop == "r30" and result == -1:
            return "PASS", "r30 = -1"
        if stop == "r30" and result > 0:
            return "FAIL", f"subtest {result} (r30)"
        return "INCONCLUSIVE", f"no defined completion signal (r30 = {result})"
    if suite == "nemu":
        done = re.search(r"Done\s*[!.:]?\s*Tests\s*[:.]\s*(\d+)\s*[.,;]?\s*Failed\s*[:.]\s*(\d+)",
                         text, re.IGNORECASE)
        if done:
            tests, failed = map(int, done.groups())
            if tests > 0:
                return ("FAIL" if failed else "PASS"), f"guest OCR: {tests} tests, {failed} failed"
        return "INCONCLUSIVE", "no completion banner; see VI/OCR"
    if not telemetry["non_black"]:
        return "INCONCLUSIVE", "no non-black VI pixels at budget"
    fail = re.search(r"^.*\bFAIL(?:ED)?\b.*$", text, re.IGNORECASE | re.MULTILINE)
    if fail:
        return "FAIL", f"OCR marker: {fail.group().strip()}"
    return "REVIEW", "visual output; compare PNG/OCR with hardware"


def run_rom(cli, rom, suite, budget, timeout, folder, ocr):
    folder.mkdir(parents=True)
    ppm = folder / "vi.ppm"
    png = folder / "vi.png"
    protocol = "r30" if suite == "dillon" else "visual"
    result = {}
    try:
        with (folder / "stderr.txt").open("w", encoding="utf-8") as stderr:
            process = subprocess.run([str(cli), "--hw-test-report", str(rom), str(budget),
                                      protocol, str(ppm)], stdout=subprocess.PIPE, stderr=stderr,
                                     check=True, timeout=timeout, text=True, errors="replace")
        (folder / "telemetry.json").write_text(process.stdout, encoding="utf-8")
        result = json.loads(process.stdout)
        text = ""
        if result["snapshot"]:
            png_from_ppm(ppm, png)
            ppm.unlink()
            if ocr and result["non_black"] and suite != "dillon":
                try:
                    process = subprocess.run(ocr(png), check=True, capture_output=True,
                                             timeout=30, text=True, errors="replace")
                    text = process.stdout
                    (folder / "ocr.txt").write_text(text, encoding="utf-8")
                except (OSError, subprocess.SubprocessError) as error:
                    (folder / "ocr-error.txt").write_text(str(error), encoding="utf-8")
        status, message = verdict(suite, result, text)
    except subprocess.TimeoutExpired:
        status, message = "ERROR", f"host timeout ({timeout}s)"
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        status, message = "ERROR", str(error)
    return result, status, message


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=ROOT / "build/macos/Release/n64forge.app/Contents/MacOS/n64forge")
    parser.add_argument("--corpus", type=Path, default=ROOT / "tests/hardware_corpus")
    parser.add_argument("--output", type=Path, default=ROOT / "build/hardware-report")
    parser.add_argument("--cycles", type=int, default=25_000_000, help="nominal cycles per ROM")
    parser.add_argument("--timeout", type=int, default=60, help="host seconds per ROM")
    parser.add_argument("--no-ocr", action="store_true")
    parser.add_argument("--strict", action="store_true", help="fail on FAIL, ERROR or INCONCLUSIVE")
    args = parser.parse_args()
    if not args.cli.is_file() or args.cycles <= 0 or args.timeout <= 0:
        parser.error("provide a built --cli and positive cycle/timeout limits")
    corpus = args.corpus.resolve()
    roms = [rom for rom in sorted(corpus.rglob("*"))
            if rom.is_file() and rom.suffix.lower() in ROM_SUFFIXES
            and not any(part.startswith(".") for part in rom.relative_to(corpus).parts)]
    if not roms:
        parser.error(f"no test ROMs under {corpus}; run scripts/fetch_hardware_tests.sh")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=datetime.now().strftime("%Y%m%d-%H%M%S-"), dir=output))
    ocr = None if args.no_ocr else ocr_tool(run)
    print(f"Hardware report: {run}", flush=True)
    rows = []
    for index, rom in enumerate(roms, 1):
        relative = rom.relative_to(corpus)
        suite = relative.parts[0] if len(relative.parts) >= 2 else "local"
        subsystem = relative.parts[1] if len(relative.parts) >= 3 else "unspecified"
        folder = run / f"{index:03d}-{rom.stem}"
        print(f"[{index}/{len(roms)}] {relative}", flush=True)
        data, status, message = run_rom(args.cli.resolve(), rom, suite, args.cycles, args.timeout, folder, ocr)
        rows.append({"rom": relative.as_posix(), "subsystem": subsystem, "status": status,
                     "message": message, "artifacts": folder.name, **data})
    (run / "report.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    lines = ["| ROM | Subsystem | Status | Failing Subtest / Message | Cycles | Exceptions | RSP launches / RDP cmds / syncs |",
             "|---|---|---|---|---:|---:|---:|"]
    for row in rows:
        counts = " / ".join(str(row.get(k, "-")) for k in ("rsp_launches", "rdp_commands", "rdp_syncs"))
        fields = [row["rom"], row["subsystem"], row["status"], row["message"],
                  row.get("cycles", "-"), row.get("exceptions", "-"), counts]
        lines.append("| " + " | ".join(" ".join(str(x).split()).replace("|", "\\|") for x in fields) + " |")
    report = "\n".join(lines) + "\n"
    (run / "report.md").write_text(report, encoding="utf-8")
    print(report)
    return int(args.strict and any(row["status"] in ("FAIL", "ERROR", "INCONCLUSIVE") for row in rows))


if __name__ == "__main__":
    sys.exit(main())
