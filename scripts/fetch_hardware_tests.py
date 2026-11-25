"""Populate the ignored corpus with pinned, public diagnostic payloads."""

import argparse
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
from urllib.request import Request, urlopen
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1] / "tests/hardware_corpus"
PETER = "7085543e4a19d8c539fc9e0a4d2869e788b4ed4b"
NEMU = "9a8b9f7d94ee2f6f57d7feed70c98c22cdc30e6c"
DILLON = "https://api.github.com/repos/Dillonb/n64-tests/releases/assets/198830557"
DILLON_SHA256 = "ad5108f5f2bf2da652e85b6178da62a376c084801460aefccd3c9354d34dc2e6"

PETER_TESTS = {
    "cpu": ["CPUTest/CPU/" + p for p in (
        "DADD/CPUDADD.N64", "DDIV/CPUDDIV.N64", "DMULT/CPUDMULT.N64",
        "DIV/CPUDIV.N64", "MULT/CPUMULT.N64", "LOADSTORE/LW/CPULW.N64",
        "LOADSTORE/LL_LLD_SC_SCD/LL_LLD_SC_SCD.N64", "TIMINGNTSC/CPUTIMINGNTSC.N64")],
    "cop1": ["CPUTest/CP1/COP1FullMode/COP1FullMode.N64"],
    "rsp": ["RSPTest/CP2/" + p for p in (
        "VADD/RSPCP2VADD.N64", "VMULF/RSPCP2VMULF.N64", "VCL/RSPCP2VCL.N64",
        "LOADSTORE/LTV/RSPCP2LTV.N64")],
    "rdp": ["RDP/16BPP/Rectangle/" + p for p in (
        "FillRectangle/FillRectangle320x240/FillRectangle16BPP320X240.N64",
        "TextureRectangle/TLUT/CopyTextureRectangleTLUTRGBA4B320x240/CopyTextureRectangle16BPPTLUTRGBA4B320X240.N64",
        "TextureRectangle/TLUT/CopyTextureRectangleTLUTRGBA8B320x240/CopyTextureRectangle16BPPTLUTRGBA8B320X240.N64")],
    "dma": ["RSP/DMAStride/RSPDMAStride.N64",
            "CPUTest/DMAAlignment-PI-cart/DMAAlignment-PI-ROM-FROM.N64"],
}


def fetch(url, accept=None):
    headers = {"User-Agent": "N64Forge-hardware-corpus"}
    if accept:
        headers["Accept"] = accept
    with urlopen(Request(url, headers=headers), timeout=60) as response:
        return response.read()


def install(path, raw, strip_ipl=False):
    if len(raw) <= 0x1000 or raw[:4] != b"\x80\x37\x12\x40":
        raise ValueError(f"{path.name}: invalid big-endian ROM")
    if strip_ipl:
        # N64Forge runs the post-IPL payload. Don't persist upstream firmware.
        raw = raw[:0x40] + bytes(0xFC0) + raw[0x1000:]
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if path.read_bytes() != raw:
            raise ValueError(f"{path} differs; move it aside before refetching")
    else:
        with path.open("xb") as file:
            file.write(raw)
    print(f"{hashlib.sha256(raw).hexdigest()}  {path.relative_to(ROOT)}", flush=True)


def dillon():
    raw = fetch(DILLON, "application/octet-stream")
    if hashlib.sha256(raw).hexdigest() != DILLON_SHA256:
        raise ValueError("Dillon release archive SHA-256 mismatch")
    with ZipFile(io.BytesIO(raw)) as archive:
        roms = sorted(entry for entry in archive.namelist() if entry.lower().endswith(".z64"))
        if not roms:
            raise ValueError("no ROMs in Dillon release")
        for entry in roms:
            install(ROOT / "dillon/cpu" / Path(entry).name, archive.read(entry))


def peter():
    errors = []
    for subsystem, paths in PETER_TESTS.items():
        for path in paths:
            try:
                install(ROOT / "peter" / subsystem / Path(path).name,
                        fetch(f"https://raw.githubusercontent.com/PeterLemon/N64/{PETER}/{path}"),
                        strip_ipl=True)
            except (OSError, ValueError) as error:
                errors.append(f"{path}: {error}")
    if errors:
        raise ValueError("\n".join(errors))


def nemu():
    # Upstream has no release asset. Use its pinned compiler/ROM packager.
    work = ROOT / ".build"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "nemu64-test"
    if not source.exists():
        subprocess.run(["git", "init", str(source)], check=True)
    subprocess.run(["git", "fetch", "--depth=1",
                    "https://github.com/thelemmy/nemu64-test.git", NEMU], cwd=source, check=True)
    subprocess.run(["git", "checkout", "--detach", NEMU], cwd=source, check=True)
    tools = work / "tools"
    rust_bin = tools / "bin" / "nust64"
    if not rust_bin.exists():
        subprocess.run(["cargo", "+stable", "install", "nust64", "--version", "0.4.1",
                        "--locked", "--root", str(tools)], cwd=work, check=True)
    env = {**os.environ, "PATH": str(tools / "bin") + os.pathsep + os.environ["PATH"]}
    target = work / "target"
    subprocess.run(["cargo", "run", "--locked", "--release", "--target-dir", str(target),
                    "--features", "cycle,timing"], cwd=source, env=env, check=True)
    output = target / "mips-nintendo64-none/release/n64-systemtest.z64"
    install(ROOT / "nemu/system/n64-systemtest-cycle-timing.z64", output.read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["all", "nemu", "dillon", "peter"], default="all")
    args = parser.parse_args()
    failed = False
    for name, prepare in [("dillon", dillon), ("peter", peter), ("nemu", nemu)]:
        if args.suite not in ("all", name):
            continue
        try:
            prepare()
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            print(f"{name}: setup failed: {error}", file=sys.stderr)
            failed = True
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
