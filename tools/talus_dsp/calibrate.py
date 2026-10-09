"""Derives Talus's level constants from renders of the default voice at C3,
full force, Variation 0, on the stub-compiled channel:

  NOISE_FORCE, TONE_FORCE   Noise or Tone at 100 % rings the head to the strike's peak.
  DIRECT_GAIN               Direct at 100 % peaks with the head.
  WIRES_GAIN                Saturated Wires at 100 % peak with the head.
  WIRE_HEAD_REFERENCE       The head motion's peak (the wire knee's 0 dB).
  TENSION_NORMAL            One over the head power's peak.
  OUTPUT_TRIM               The full output peaks at -7 dBFS, per model.

Each constant depends on the others through the default mix, so run it until
the changes settle: build, calibrate --write, build again.

    python gdsion/tools/talus_dsp/calibrate.py [--write]
"""

import argparse
import math
import re

import numpy as np

import harness as h

CHANNEL = h.GDSION / "src" / "chip" / "channels" / "siopm_channel_talus.cpp"
TARGET_PEAK = h.PIPE_PEAK * 10 ** (-7 / 20)
RATE = 48000
F1 = 440.0 * 2 ** ((60 - 69) / 12)
NAMES = ["NOISE_FORCE", "TONE_FORCE", "DIRECT_GAIN", "WIRES_GAIN", "WIRE_HEAD_REFERENCE", "TENSION_NORMAL"]


def read_constants(text: str) -> dict:
	constants = {name: float(re.search(rf"constexpr double {name} = ([0-9.e+-]+);", text).group(1)) for name in NAMES}
	trims = re.search(r"constexpr double OUTPUT_TRIM\[\] = \{ ([^}]*) \};", text).group(1)
	constants["OUTPUT_TRIM"] = [float(v) for v in trims.split(",")]
	return constants


def write_constants(text: str, constants: dict) -> str:
	for name in NAMES:
		text = re.sub(rf"(constexpr double {name} = )[0-9.e+-]+;", rf"\g<1>{constants[name]:.4g};", text)
	trims = ", ".join(f"{v:.4g}" for v in constants["OUTPUT_TRIM"])
	return re.sub(r"(constexpr double OUTPUT_TRIM\[\] = \{ )[^}]*( \};)", rf"\g<1>{trims}\g<2>", text)


def one_pole_coeff(hz: float) -> float:
	return 1.0 - math.exp(-2 * math.pi * hz / RATE)


def peak(signal: np.ndarray) -> float:
	return float(np.abs(signal).max())


def main() -> None:
	parser = argparse.ArgumentParser()
	parser.add_argument("--write", action="store_true")
	args = parser.parse_args()
	text = CHANNEL.read_text(encoding="utf-8")
	old = read_constants(text)

	def voice(**overrides):
		return h.talus_params(variation=0, **overrides)

	cases = {
		"strike": voice(noise_level=0, tone_level=0, direct=0),
		"noise": voice(strike_level=0, noise_level=100, tone_level=0, direct=0),
		"tone": voice(strike_level=0, noise_level=0, tone_level=100, direct=0),
		"head": voice(direct=0),
		"direct": voice(head=0),
		"wires": voice(head=0, direct=0, wires_level=100, wires_tension=0),
		"membrane": voice(),
		"loop": voice(model=1),
	}
	jobs = [h.Job(rate=RATE, params=params).hit(60, 1.0).render(1.0) for params in cases.values()]
	outputs = dict(zip(cases, h.run(jobs)))
	membrane_gain = old["OUTPUT_TRIM"][0] * h.PIPE_PEAK

	new = dict(old)
	new["NOISE_FORCE"] = old["NOISE_FORCE"] * peak(outputs["strike"]) / peak(outputs["noise"])
	new["TONE_FORCE"] = old["TONE_FORCE"] * peak(outputs["strike"]) / peak(outputs["tone"])
	new["DIRECT_GAIN"] = old["DIRECT_GAIN"] * peak(outputs["head"]) / peak(outputs["direct"])
	new["WIRES_GAIN"] = old["WIRES_GAIN"] * peak(outputs["head"]) / peak(outputs["wires"])

	head = outputs["head"] / membrane_gain
	motion = np.zeros_like(head)
	power = np.zeros_like(head)
	motion_coeff = one_pole_coeff(4 * F1)
	attack = 1 - math.exp(-1 / (0.001 * RATE))
	release = 1 - math.exp(-1 / (max(0.005, 2 / F1) * RATE))
	m = p = 0.0
	for i, x in enumerate(head):
		m += motion_coeff * (x - m)
		motion[i] = m
		p += (x * x - p) * (attack if x * x > p else release)
		power[i] = p
	new["WIRE_HEAD_REFERENCE"] = peak(motion)
	new["TENSION_NORMAL"] = 1.0 / power.max()
	new["OUTPUT_TRIM"] = [
		old["OUTPUT_TRIM"][0] * TARGET_PEAK / peak(outputs["membrane"]),
		old["OUTPUT_TRIM"][1] * TARGET_PEAK / peak(outputs["loop"]),
	]

	for name in NAMES + ["OUTPUT_TRIM"]:
		print(f"{name:20s} {old[name]!s:>24} -> {new[name]}")
	if args.write:
		CHANNEL.write_text(write_constants(text, new), encoding="utf-8", newline="\n")
		print(f"wrote {CHANNEL.name}; rebuild and run again until the changes settle")


if __name__ == "__main__":
	main()
