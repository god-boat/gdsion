"""Renders every shipped Iron preset to out/renders/Iron - <name>.wav on the
stub-compiled channel, and the same riff on the baseline build (Iron as
committed at build.py's --iron-rev) to "... (baseline).wav", for the by-ear A/B.

One channel plays the whole riff, the way one voice slot plays a mono guitar
part, so every pick and release takes the next draws of the channel's random
sequence: palm-muted sixteenths with accents, held chords, a lead line with
hammer-ons and pull-offs, and a hard pick left to ring. The WAVs are the
channel output before Inst Gain and before any effects in the preset's rack.
Prints each preset's peak on both builds.

    python gdsion/tools/talus_dsp/render_iron.py
"""

import json

import numpy as np

import harness as h
import render_presets as rp

SPECS = rp.APP / "objects" / "voice_specs" / "iron_specs.tres"
STEP = 0.1  # A sixteenth at 150 BPM.

# (note, velocity, sixteenths held, sixteenths to the next event, the notes slurred to and their sixteenths)
CHUG = [(40, 1.0 if i % 8 in (0, 3) else 0.7, 0.7, 1, ()) for i in range(16)]
CHORDS = [(43, 1.0, 3.5, 4, ()), (45, 0.9, 3.5, 4, ()), (40, 1.0, 7, 8, ())]
LEAD = [
	(52, 0.9, 2, 0, ((55, 2), (57, 2))),
	(59, 0.8, 2, 0, ((57, 1), (55, 1), (52, 3))),
	(64, 0.9, 1.5, 2, ()), (62, 0.7, 1.5, 2, ()), (59, 0.8, 1.5, 2, ()), (55, 1.0, 5, 6, ()),
]
RING = [(40, 1.0, 22, 26, ())]
RIFF = CHUG + CHORDS + CHUG + LEAD + RING


def riff(params: list[int]) -> h.Job:
	job = h.Job("iron", rate=rp.RATE, params=params)
	for note, velocity, held, step, slurs in RIFF:
		job.hit(note, velocity).render(held * STEP)
		for slurred, length in slurs:
			job.pitch(slurred).render(length * STEP)
		job.off().render(max(step - held, 0.3 if slurs else 0.0) * STEP)
	return job


def iron_presets():
	slots = rp.spec_slots(SPECS)
	for path in sorted(rp.LIBRARY.rglob("*.poolypreset")):
		for device in json.loads(path.read_text(encoding="utf-8"))["deviceGraph"]["devices"]:
			if device.get("deviceData", {}).get("voiceType") == "Iron":
				values = list(h.IRON_DEFAULTS)
				for key, value in device["parameters"]["channel"].items():
					if key in slots:
						values[slots[key]] = int(value)
				yield device["name"], values


def main() -> None:
	rp.RENDERS.mkdir(parents=True, exist_ok=True)
	presets = list(iron_presets())
	jobs = [riff(params) for _, params in presets]
	for (name, _), current, baseline in zip(presets, h.run(jobs), h.run(jobs, h.IRON_BASELINE)):
		rp.write_wav(rp.RENDERS / f"Iron - {name}.wav", current)
		rp.write_wav(rp.RENDERS / f"Iron - {name} (baseline).wav", baseline)
		peaks = [20 * np.log10(np.abs(y).max() / h.PIPE_PEAK) for y in (current, baseline)]
		print(f"{name:20s} peak {peaks[0]:6.1f} dBFS, baseline {peaks[1]:6.1f} dBFS")
	print(f"wrote {2 * len(presets)} renders to {rp.RENDERS}")


if __name__ == "__main__":
	main()
