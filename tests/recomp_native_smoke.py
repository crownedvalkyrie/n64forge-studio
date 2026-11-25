"""Generate a firmware-free synthetic cartridge, compile it, and run it."""

import pathlib
import struct
import subprocess
import sys
import tempfile


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    cmake = sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="n64forge-native-") as tmp:
        path = pathlib.Path(tmp)
        rom_path = path / "synthetic.z64"
        source, build = path / "source", path / "build"
        rom = bytearray(0x2000)
        struct.pack_into(">I", rom, 0, 0x80371240)
        struct.pack_into(">I", rom, 8, 0x80001000)
        # Save RA, call a thunk, then branch if v0 is 14. The branch's delay
        # slot changes v0 to 15, so evaluating the predicate late fails.
        code = (0x03E08025, 0x0C000418, 0x24020005, 0x24420007, 0x2408000E,
                0x10480002, 0x24420001, 0x2402FFFF, 0x0200F825, 0x03E00008, 0)
        struct.pack_into(">11I", rom, 0x1000, *code)
        # The thunk jumps through a value loaded from RDRAM. Static analysis
        # cannot resolve it, so generated code must use dispatchIndirect().
        struct.pack_into(">4I", rom, 0x1060, 0x3C098000, 0x8D291084,
                         0x01200008, 0)
        # The callee reads the value 2 from the mapped boot image, adds it in
        # its return delay slot, and returns to the caller's continuation.
        struct.pack_into(">4I", rom, 0x1040, 0x3C098000, 0x8D2A1080,
                         0x03E00008, 0x004A1021)
        struct.pack_into(">I", rom, 0x1080, 2)
        struct.pack_into(">I", rom, 0x1084, 0x80001040)
        # A statically direct call makes the thunk's runtime target discoverable
        # without teaching the analyzer to track data values.
        struct.pack_into(">4I", rom, 0x1088, 0x0C000410, 0, 0x03E00008, 0)
        rom_path.write_bytes(rom)
        run(cli, "recompile", rom_path, "-o", source, "--entry", "0x80001088")
        preserved = (source / "main.cpp").read_bytes()
        overwrite = subprocess.run(
            [str(cli), "recompile", str(rom_path), "-o", str(source),
             "--entry", "0x80001088"], capture_output=True)
        if overwrite.returncode == 0 or (source / "main.cpp").read_bytes() != preserved:
            raise RuntimeError("recompile overwrote a nonempty project")
        run(cmake, "-S", source, "-B", build, "-G", sys.argv[3], "-DCMAKE_BUILD_TYPE=Release")
        run(cmake, "--build", build, "--config", "Release", "--target", "n64forge_native", "--parallel", "4")
        executable = build / "Release" / "n64forge_native"
        if not executable.is_file():
            executable = build / "n64forge_native"
        result = run(executable, rom_path, "--print-result", capture_output=True, text=True)
        if result.stdout.strip() != "v0=0xF":
            raise RuntimeError(f"unexpected native guest result: {result.stdout!r} {result.stderr!r}")
        print(result.stdout.strip())


if __name__ == "__main__":
    main()
