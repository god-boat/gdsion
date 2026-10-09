"""Compiles the real Talus and Iron channel sources against the stubs into
out/talus_render.exe with MSVC. Not the SCons build: no Godot, no DLL.

Also builds out/iron_baseline.exe from Iron as committed at --iron-rev
(default HEAD of the gdsion repo), so tests.py can check that sharing DSP
blocks left Iron's output bit-identical.

    python gdsion/tools/talus_dsp/build.py [--iron-rev HEAD]
"""

import argparse
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
GDSION = HERE.parents[1]
SRC = GDSION / "src"
OUT = HERE / "out"

VSWHERE = pathlib.Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
FLAGS = "/nologo /std:c++17 /O2 /EHsc /utf-8 /W3 /wd4244 /DNOMINMAX"


def find_vcvars() -> pathlib.Path:
	if VSWHERE.exists():
		found = subprocess.run(
			[str(VSWHERE), "-latest", "-products", "*", "-property", "installationPath"],
			capture_output=True, text=True, check=True,
		).stdout.strip()
		if found:
			return pathlib.Path(found) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
	return pathlib.Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat")


def compile_exe(name: str, defines: str, includes: list[pathlib.Path], sources: list[pathlib.Path]) -> None:
	objects = OUT / f"{name}_obj"
	objects.mkdir(parents=True, exist_ok=True)
	include_flags = " ".join(f'/I "{path}"' for path in includes)
	source_args = " ".join(f'"{path}"' for path in sources)
	script = OUT / f"build_{name}.bat"
	script.write_text(
		"@echo off\r\n"
		f'call "{find_vcvars()}" >nul\r\n'
		f'cl {FLAGS} {defines} {include_flags} {source_args} /Fo"{objects}\\\\" /Fe"{OUT / name}.exe"\r\n',
		encoding="utf-8",
	)
	result = subprocess.run(["cmd", "/c", str(script)], capture_output=True, text=True)
	lines = [line for line in result.stdout.splitlines() if line.strip() and not line.strip().endswith(".cpp")]
	if lines:
		print("\n".join(lines))
	if result.returncode != 0:
		print(result.stderr)
		sys.exit(f"build of {name} failed")
	print(f"built {(OUT / name).relative_to(GDSION)}.exe")


def build_iron_baseline(rev: str) -> None:
	baseline = OUT / "iron_baseline" / "chip" / "channels"
	baseline.mkdir(parents=True, exist_ok=True)
	for name in ("siopm_channel_iron.h", "siopm_channel_iron.cpp"):
		text = subprocess.run(
			["git", "-C", str(GDSION), "show", f"{rev}:src/chip/channels/{name}"],
			capture_output=True, text=True, check=True,
		).stdout
		(baseline / name).write_text(text, encoding="utf-8", newline="\n")
	compile_exe(
		"iron_baseline",
		"/DHARNESS_IRON /DHARNESS_NO_TALUS",
		[OUT / "iron_baseline", HERE / "stubs", SRC],
		[HERE / "render_main.cpp", baseline / "siopm_channel_iron.cpp"],
	)


def main() -> None:
	parser = argparse.ArgumentParser()
	parser.add_argument("--iron-rev", default="HEAD")
	args = parser.parse_args()
	OUT.mkdir(exist_ok=True)
	compile_exe(
		"talus_render",
		"/DHARNESS_IRON",
		[HERE / "stubs", SRC],
		[HERE / "render_main.cpp", SRC / "chip" / "channels" / "siopm_channel_talus.cpp", SRC / "chip" / "channels" / "siopm_channel_iron.cpp"],
	)
	build_iron_baseline(args.iron_rev)


if __name__ == "__main__":
	main()
