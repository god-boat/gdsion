"""Renders every shipped Talus preset, and each pad of every kit built from them,
to out/renders/<name>.wav on the stub-compiled channel, for the by-ear A/B
against reference recordings.

A drum (Keytrack 0) plays hits at velocity 1.0, 0.7, 0.4 and 0.2 (a ghost
note), half a second apart, then a two-bar 16th-note roll at 120 BPM accenting
every fourth hit, all on one channel: the way a re-strike voice plays a drum.
A pitched preset plays a phrase from C2 to C6 and a chord, one channel per
note, the way the voice pool plays it, each key held for 0.4 s. The WAVs are
the channel output before Inst Gain and before any effects in the preset's
rack, so a full hit peaks near -7 dBFS. Prints each preset's peak and how long
a full hit takes to idle.

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


HELD = 0.4
RING = 4.0
# (seconds, note, velocity): a rising phrase, four velocities on one key, then a chord.
PHRASE = (
	[(0.5 * i, note, 0.8) for i, note in enumerate((36, 43, 48, 55, 60, 64, 67, 72, 79, 84))]
	+ [(5.5 + 0.6 * i, 60, velocity) for i, velocity in enumerate((1.0, 0.7, 0.4, 0.2))]
	+ [(8.5 + 0.03 * i, note, 0.75) for i, note in enumerate((48, 55, 60, 64, 67))]
)


def pattern(params: list[int]) -> list[tuple[float, h.Job]]:
	"""The jobs that play a preset, each with the time its output starts."""
	if params[h.TALUS_SLOTS.index("KEYTRACK")] > 0:
		return [(start, h.Job(rate=RATE, params=params).hit(note, velocity).render(HELD).off().render(RING)) for start, note, velocity in PHRASE]
	job = h.Job(rate=RATE, params=params)
	for velocity in (1.0, 0.7, 0.4, 0.2):
		job.hit(NOTE, velocity).render(0.5)
	sixteenth = 0.125
	for i in range(32):
		job.hit(NOTE, 0.9 if i % 4 == 0 else 0.45).render(sixteenth)
	return [(0.0, job.render(1.5))]


def mix(starts: list[float], outputs: list[np.ndarray]) -> np.ndarray:
	offsets = [int(round(start * RATE)) for start in starts]
	mixed = np.zeros(max(offset + len(y) for offset, y in zip(offsets, outputs)))
	for offset, y in zip(offsets, outputs):
		mixed[offset:offset + len(y)] += y
	return mixed


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
	patterns = [pattern(params) for _, params in found]
	idle_jobs = [h.Job(rate=RATE, params=params).hit(NOTE, 1.0).until_idle(30.0, 32) for _, params in found]
	rendered = iter(h.run([job for jobs in patterns for _, job in jobs]))
	outputs = [mix([start for start, _ in jobs], [next(rendered) for _ in jobs]) for jobs in patterns]
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
