"""Runs scripted renders on the stub-compiled channels (out/talus_render.exe).

A Job is the command list render_main.cpp reads. run() sends any number of
jobs to one renderer process and returns each job's output as a float array
in pipe units (8192 = full scale).
"""

import os
import pathlib
import re
import subprocess
import tempfile

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
GDSION = HERE.parents[1]
OUT = HERE / "out"
RENDERER = OUT / "talus_render.exe"
IRON_BASELINE = OUT / "iron_baseline.exe"
PIPE_PEAK = 8192.0


def _read_defaults(header: str, struct: str) -> tuple[list[str], list[int]]:
	text = (GDSION / "src" / "chip" / "channels" / header).read_text(encoding="utf-8")
	body = text[text.index(f"struct {struct}"):]
	slots = re.findall(r"^\t\t([A-Z0-9_]+),", body[:body.index("SLOT_COUNT")], re.M)
	values_text = body[body.index("int values[SLOT_COUNT] ="):]
	values_text = values_text[values_text.index("{") + 1:values_text.index("};")]
	values_text = re.sub(r"//[^\n]*", "", values_text)
	values = [int(v) for v in re.findall(r"-?\d+", values_text)]
	assert len(slots) == len(values), (slots, values)
	return slots, values


TALUS_SLOTS, TALUS_DEFAULTS = _read_defaults("siopm_talus_params.h", "TalusParams")
IRON_SLOTS, IRON_DEFAULTS = _read_defaults("siopm_iron_params.h", "IronParams")


def talus_params(**overrides: int) -> list[int]:
	"""The default tuple with slots overridden by lowercase slot name, e.g. decay_ms=2000."""
	values = list(TALUS_DEFAULTS)
	for name, value in overrides.items():
		values[TALUS_SLOTS.index(name.upper())] = int(value)
	return values


def note_pitch(note: float) -> int:
	return int(round(note * 64))


class Job:
	def __init__(self, channel: str = "talus", rate: int = 48000, params: list[int] | None = None):
		self.lines = [f"rate {rate}", f"channel {channel}"]
		self.rate = rate
		if params is not None:
			self.params(params)

	def params(self, values: list[int]) -> "Job":
		self.lines.append("params " + " ".join(str(v) for v in values))
		return self

	def hit(self, note: float = 60, velocity: float = 1.0) -> "Job":
		self.lines += [f"pitch {note_pitch(note)}", f"expression {int(round(velocity * 128))}", "on"]
		return self

	def pitch(self, note: float) -> "Job":
		self.lines.append(f"pitch {note_pitch(note)}")
		return self

	def off(self) -> "Job":
		self.lines.append("off")
		return self

	def render(self, seconds: float | None = None, samples: int | None = None, blocks: list[int] | None = None) -> "Job":
		count = samples if samples is not None else int(round(seconds * self.rate))
		self.lines.append(f"render {count} " + " ".join(str(b) for b in (blocks or [])))
		return self

	def until_idle(self, seconds: float, block: int = 32) -> "Job":
		self.lines.append(f"until_idle {int(seconds * self.rate)} {block}")
		return self

	def time(self, label: str, seconds: float, block: int = 256) -> "Job":
		self.lines.append(f"time {label} {int(seconds * self.rate)} {block}")
		return self


def run_with_report(jobs: list[Job], renderer: pathlib.Path = RENDERER) -> tuple[list[np.ndarray], list[str]]:
	"""Renders every job; returns one array per job, and the renderer's report lines."""
	with tempfile.TemporaryDirectory() as scratch:
		path = os.path.join(scratch, "out.bin")
		script = [f"out {path}"]
		for job in jobs:
			script += job.lines + ["end"]
		result = subprocess.run([str(renderer)], input="\n".join(script) + "\n", capture_output=True, text=True)
		if result.returncode != 0:
			raise RuntimeError(result.stderr)
		data = np.fromfile(path, dtype=np.int32)
	outputs = []
	position = 0
	while position < len(data):
		count = data[position]
		outputs.append(data[position + 1:position + 1 + count].astype(np.float64))
		position += 1 + count
	return outputs, result.stdout.splitlines()


def run(jobs: list[Job], renderer: pathlib.Path = RENDERER) -> list[np.ndarray]:
	return run_with_report(jobs, renderer)[0]


def report_values(lines: list[str], key: str) -> list[str]:
	"""The values of every report line that starts with key, in order."""
	return [line.split(maxsplit=1)[1] for line in lines if line.startswith(key + " ")]


def render(params: list[int] | None = None, note: float = 60, velocity: float = 1.0, seconds: float = 1.0, rate: int = 48000) -> np.ndarray:
	return run([Job(rate=rate, params=params).hit(note, velocity).render(seconds)])[0]


def run_timing(jobs: list[Job], renderer: pathlib.Path = RENDERER) -> dict[str, float]:
	"""Runs jobs that use time(); returns nanoseconds per sample by label."""
	lines = run_with_report(jobs, renderer)[1]
	return {label: float(value) for label, value in (entry.split() for entry in report_values(lines, "TIME"))}
