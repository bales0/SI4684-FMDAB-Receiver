"""Compile the production decoder for the host and run synthetic fixtures."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from io import BytesIO
from pathlib import Path

from jpeg_fixture_matrix import DHT, make_jpeg, remove_header_marker


ROOT = Path(__file__).resolve().parents[1]

CASES = {
    "baseline_444": dict(sampling="444"),
    "baseline_422": dict(sampling="422"),
    "baseline_420": dict(sampling="420"),
    "baseline_440": dict(sampling="440"),
    "baseline_411": dict(sampling="411"),
    "baseline_max10_blocks": dict(sampling="max10"),
    "baseline_gray": dict(sampling="gray"),
    "baseline_odd_size": dict(width=319, height=237, sampling="420"),
    "baseline_restart": dict(sampling="420", restart=7),
    "baseline_app_com_fill": dict(sampling="444", extras=True, marker_fill=True),
    "baseline_multi_tables": dict(sampling="420", multiple_tables=True),
    "baseline_separate_scans": dict(sampling="420", separate=True),
    "baseline_repeated_tables": dict(sampling="420", separate=True,
                                      repeat_tables=True),
    "progressive_444": dict(sampling="444", progressive=True),
    "progressive_420": dict(sampling="420", progressive=True),
    "progressive_gray": dict(sampling="gray", progressive=True),
    "progressive_refine": dict(sampling="420", progressive=True, refine=True),
    "progressive_repeated_tables": dict(sampling="420", progressive=True,
                                         refine=True, repeat_tables=True),
    "progressive_restart": dict(sampling="420", progressive=True,
                                refine=True, restart=11),
}

PILLOW_CASES = {
    "pillow_baseline_444": dict(mode="RGB", subsampling=0),
    "pillow_baseline_422": dict(mode="RGB", subsampling=1),
    "pillow_baseline_420": dict(mode="RGB", subsampling=2),
    "pillow_baseline_gray": dict(mode="L"),
    "pillow_progressive_444": dict(mode="RGB", subsampling=0, progressive=True),
    "pillow_progressive_420": dict(mode="RGB", subsampling=2, progressive=True),
    "pillow_progressive_gray": dict(mode="L", progressive=True),
    "pillow_baseline_restart_blocks": dict(mode="RGB", subsampling=2,
                                            restart_marker_blocks=7),
    "pillow_progressive_restart_rows": dict(mode="RGB", subsampling=2,
                                             progressive=True,
                                             restart_marker_rows=2),
    "pillow_odd_optimized": dict(mode="RGB", size=(319, 237), subsampling=2,
                                  optimize=True),
}


def find_compiler() -> str:
    configured = os.environ.get("CXX")
    candidates = [configured] if configured else []
    candidates += [shutil.which("g++"), shutil.which("clang++")]
    if os.name == "nt":
        candidates.append(str(Path.home() / ".platformio" / "packages" /
                              "toolchain-gccmingw32" / "bin" / "g++.exe"))
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    raise RuntimeError("no host C++ compiler found (set CXX)")


def main() -> int:
    compiler = find_compiler()
    compiler_env = os.environ.copy()
    compiler_env["PATH"] = str(Path(compiler).parent) + os.pathsep + compiler_env["PATH"]
    with tempfile.TemporaryDirectory(prefix="sls-jpeg-host-") as temp_name:
        temp = Path(temp_name)
        executable = temp / ("jpeg_decoder_host.exe" if os.name == "nt" else
                             "jpeg_decoder_host")
        subprocess.run([
            compiler, "-std=c++11", "-O2",
            "-DJPEGDECODER_TEST_MAX_ALLOCATION=51188",
            "-I" + str(ROOT / "tests" / "host_stubs"),
            "-I" + str(ROOT / "src"),
            str(ROOT / "src" / "JPEGdecoder.cpp"),
            str(ROOT / "tests" / "jpeg_decoder_host.cpp"),
            "-o", str(executable),
        ], check=True, env=compiler_env)

        for name, options in CASES.items():
            fixture = temp / f"{name}.jpg"
            fixture.write_bytes(make_jpeg(**options))
            width = options.get("width", 320)
            height = options.get("height", 240)
            completed = subprocess.run(
                [str(executable), str(fixture), str(width), str(height)],
                text=True, capture_output=True, env=compiler_env,
            )
            if completed.returncode:
                print(f"FAIL {name} rc={completed.returncode}: "
                      f"{completed.stderr.strip()}")
                return 1
            print(f"PASS {name}: {completed.stdout.strip()}")

        try:
            from PIL import Image
        except ImportError:
            print("SKIP Pillow encoder matrix: Pillow not installed")
        else:
            for name, options in PILLOW_CASES.items():
                mode = options["mode"]
                width, height = options.get("size", (320, 240))
                image = Image.new(mode, (width, height))
                if mode == "L":
                    image.putdata([(x * 5 + y * 3) & 0xFF
                                   for y in range(height)
                                   for x in range(width)])
                else:
                    image.putdata([((x * 5) & 0xFF, (y * 3) & 0xFF,
                                    ((x + y) * 7) & 0xFF)
                                   for y in range(height)
                                   for x in range(width)])
                save_options = {key: value for key, value in options.items()
                                if key not in ("mode", "size")}
                encoded = BytesIO()
                image.save(encoded, "JPEG", quality=83, **save_options)
                fixture = temp / f"{name}.jpg"
                fixture.write_bytes(encoded.getvalue())
                completed = subprocess.run(
                    [str(executable), str(fixture), str(width), str(height)],
                    text=True, capture_output=True, env=compiler_env,
                )
                if completed.returncode:
                    print(f"FAIL {name} rc={completed.returncode}: "
                          f"{completed.stderr.strip()}")
                    return 1
                print(f"PASS {name}: {completed.stdout.strip()}")

            default_image = Image.new("RGB", (31, 29), (20, 90, 170))
            encoded = BytesIO()
            default_image.save(encoded, "JPEG", quality=83, subsampling=2,
                               optimize=False)
            without_dht = remove_header_marker(encoded.getvalue(), DHT)
            # Confirm independently that libjpeg accepts the abbreviated form.
            with Image.open(BytesIO(without_dht)) as reference:
                reference.load()
            fixture = temp / "pillow_annex_k_without_dht.jpg"
            fixture.write_bytes(without_dht)
            completed = subprocess.run(
                [str(executable), str(fixture), "31", "29"],
                text=True, capture_output=True, env=compiler_env,
            )
            if completed.returncode:
                print("FAIL Annex K fallback without DHT: "
                      f"{completed.stderr.strip()}")
                return 1
            print(f"PASS Annex K fallback without DHT: "
                  f"{completed.stdout.strip()}")

        truncated = temp / "truncated.jpg"
        truncated.write_bytes(make_jpeg(progressive=True, refine=True)[:-17])
        rejected = subprocess.run(
            [str(executable), str(truncated), "320", "240"],
            capture_output=True, env=compiler_env,
        )
        if rejected.returncode == 0 or b"rows=0" not in rejected.stderr:
            print("FAIL truncated stream was accepted")
            return 1
        print("PASS truncated stream: clean rejection")

        # One grayscale block encodes as DC-0 + EOB (two zero bits). Replace
        # the six conventional one-padding bits with zeros; expected data is
        # still complete and only the final partial byte is non-canonical.
        tolerant = bytearray(make_jpeg(width=7, height=7, sampling="gray"))
        tolerant[-3] = 0x00
        padding = temp / "zero_padding.jpg"
        padding.write_bytes(tolerant)
        accepted = subprocess.run(
            [str(executable), str(padding), "7", "7"],
            capture_output=True, env=compiler_env,
        )
        if accepted.returncode:
            print("FAIL complete scan with zero padding was rejected")
            return 1
        print("PASS non-canonical <=7-bit padding: accepted")

        malformed_data = bytearray(make_jpeg())
        dht = malformed_data.index(b"\xff\xc4")
        malformed_data[dht + 2 : dht + 4] = b"\x00\x02"
        malformed = temp / "malformed_huffman.jpg"
        malformed.write_bytes(malformed_data)
        rejected = subprocess.run(
            [str(executable), str(malformed), "320", "240"],
            capture_output=True, env=compiler_env,
        )
        if rejected.returncode == 0 or b"rows=0" not in rejected.stderr:
            print("FAIL malformed Huffman table was accepted")
            return 1
        print("PASS malformed Huffman table: clean rejection")
    return 0


if __name__ == "__main__":
    sys.exit(main())
