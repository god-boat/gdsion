"""Renders every shipped Talus preset, and each pad of every kit built from them,
to out/renders/<name>.wav on the stub-compiled channel, for the by-ear A/B
against reference recordings.

Each render plays hits at velocity 1.0, 0.7, 0.4 and 0.2 (a ghost note), half
a second apart, then a two-bar 16th-note roll at 120 BPM accenting every
fourth hit, all on one channel: the way a re-strike voice plays a drum. The
WAVs are the channel output before Inst Gain, so a full hit peaks near -7 dBFS.
Prints each preset's peak and how long a full hit takes to idle.

    python gdsion/tools/talus_dsp/render_presets.py
"""

import json
import pathlib
import re
import wave

import numpy as np

import harness as h

APP = h.GDSION.parent
LIBRARY = APP / "resources" / "Core Library" / "Instrument Presets"
SPECS = APP / "objects" / "voice_specs" / "talus_specs.tres"
RENDERS = h.OUT / "renders"
RATE = 48000
NOTE = 60


def spec_slots() -> dict[str, int]:
	text = SPECS.read_text(encoding="utf-8")
	slots = {}
	for block in text.split("[sub_resource")[1:]:
		param_id = re.search(r'^id = "([^"]+)"', block, re.M).group(1)
		slots[param_id] = int(re.search(r"^param_index = (\d+)", block, re.M).group(1))
	return slots


def talus_devices(node: dict, path: list[str]):
	"""Every Talus synth in a device graph, with a name naming where it sits."""
	for device in node.get("devices", []):
		if device.get("deviceData", {}).get("voiceType") == "Talus":
			yield " - ".join(path + [device["name"]]) if path else device["name"], device
		for chain in device.get("chains", []):
			yield from talus_devices(chain, path + [device["name"]])


def tuple_for(device: dict, slots: dict[str, int]) -> list[int]:
	values = list(h.TALUS_DEFAULTS)
	for key, value in device["parameters"]["channel"].items():
		if key in slots:
			values[slots[key]] = int(value)
	return values


def pattern(params: list[int]) -> h.Job:
	job = h.Job(rate=RATE, params=params)
	for velocity in (1.0, 0.7, 0.4, 0.2):
		job.hit(NOTE, velocity).render(0.5)
	sixteenth = 0.125
	for i in range(32):
		job.hit(NOTE, 0.9 if i % 4 == 0 else 0.45).render(sixteenth)
	return job.render(1.5)


def write_wav(path: pathlib.Path, pipe: np.ndarray) -> None:
	samples = np.clip(np.round(pipe / h.PIPE_PEAK * 32767), -32768, 32767).astype("<i2")
	with wave.open(str(path), "wb") as out:
		out.setnchannels(1)
		out.setsampwidth(2)
		out.setframerate(RATE)
		out.writeframes(samples.tobytes())


def main() -> None:
	slots = spec_slots()
	RENDERS.mkdir(parents=True, exist_ok=True)
	found = []
	for path in sorted(LIBRARY.rglob("*.poolypreset")):
		document = json.loads(path.read_text(encoding="utf-8"))
		for name, device in talus_devices(document["deviceGraph"], []):
			found.append((name, tuple_for(device, slots)))
	jobs = [pattern(params) for _, params in found]
	idle_jobs = [h.Job(rate=RATE, params=params).hit(NOTE, 1.0).until_idle(30.0, 32) for _, params in found]
	outputs = h.run(jobs)
	_, lines = h.run_with_report(idle_jobs)
	idle = [int(v) / RATE for v in h.report_values(lines, "IDLE")]
	seen = set()
	for (name, _), y, idle_after in zip(found, outputs, idle):
		if name in seen:
			continue
		seen.add(name)
		write_wav(RENDERS / f"{name}.wav", y)
		peak = 20 * np.log10(np.abs(y).max() / h.PIPE_PEAK)
		print(f"{name:32s} peak {peak:6.1f} dBFS, a full hit idles after {idle_after:.2f} s")
	print(f"wrote {len(seen)} renders to {RENDERS}")


if __name__ == "__main__":
	main()
