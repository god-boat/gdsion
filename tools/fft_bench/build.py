"""Compiles fft_main.cpp against src/dsp/fft.h into out/fft_bench.exe with MSVC
and runs it. Not the SCons build: no Godot, no DLL.

    python gdsion/tools/fft_bench/build.py
"""

import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
GDSION = HERE.parents[1]
OUT = HERE / "out"

VSWHERE = pathlib.Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
# The extension's own release flags: /O2, no /arch, precise floating point.
FLAGS = "/nologo /std:c++17 /O2 /EHsc /utf-8 /W3 /DNOMINMAX"


def find_vcvars() -> pathlib.Path:
	found = subprocess.run(
		[str(VSWHERE), "-latest", "-products", "*", "-property", "installationPath"],
		capture_output=True, text=True, check=True,
	).stdout.strip()
	return pathlib.Path(found) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"


def main() -> None:
	OUT.mkdir(exist_ok=True)
	includes = f'/I "{GDSION / "src"}" /I "{GDSION / "godot-cpp" / "include"}"'
	script = OUT / "build.bat"
	script.write_text(
		"@echo off\r\n"
		f'call "{find_vcvars()}" >nul\r\n'
		f'cl {FLAGS} {includes} "{HERE / "fft_main.cpp"}" /Fo"{OUT}\\\\" /Fe"{OUT / "fft_bench.exe"}"\r\n',
		encoding="utf-8",
	)
	result = subprocess.run(["cmd", "/c", str(script)], capture_output=True, text=True)
	if result.returncode != 0:
		print(result.stdout, result.stderr)
		sys.exit("build failed")
	sys.exit(subprocess.run([str(OUT / "fft_bench.exe")]).returncode)


if __name__ == "__main__":
	main()
