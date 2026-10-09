"""Talus verification: the cases T1-T14 from docs/TalusPhysicalModelDesign.md,
plus T0 (the C++ matches the model) and IRON (sharing DSP blocks left Iron's
output bit-identical to --iron-rev in build.py).

Every case runs on the stub-compiled C++ channel. The cases that measure the
model run on it too, over a coarser note grid; T0, T14 and IRON are C++ only.
Prints one PASS/FAIL line per case and engine, then a summary.

    python gdsion/tools/talus_dsp/build.py
    python gdsion/tools/talus_dsp/tests.py [--engine cpp|model|both] [--case T1 ...]
"""

import argparse
import math
import sys
import time

import numpy as np

import harness as h
import model

RATE = 48000
CENTS_PER_OCTAVE = 1200.0


class Engine:
	def __init__(self, name: str, run, quantized: bool, notes: list[int]):
		self.name = name
		self.run = run
		self.quantized = quantized
		self.notes = notes


CPP = Engine("cpp", h.run, True, list(range(36, 85)))
MODEL = Engine("model", model.run, False, [36, 48, 60, 72, 84])


def voice(**overrides) -> list[int]:
	"""The default voice with Variation 0, so every random draw is multiplied away."""
	overrides.setdefault("variation", 0)
	return h.talus_params(**overrides)


def note_hz(note: float) -> float:
	return 440.0 * 2 ** ((note - 69) / 12)


def cents(hz: float, reference: float) -> float:
	return CENTS_PER_OCTAVE * math.log2(hz / reference)


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------

def demodulate(y: np.ndarray, rate: int, hz: float, periods: float = 8.0) -> np.ndarray:
	"""The complex envelope of y near hz: shifted to 0 Hz, then three boxcars of whole periods."""
	shifted = y * np.exp(-2j * np.pi * hz * np.arange(len(y)) / rate)
	width = max(1, int(round(periods * rate / hz)))
	kernel = np.ones(width) / width
	for _ in range(3):
		shifted = np.convolve(shifted, kernel, mode="valid")
	return shifted


def frequency(y: np.ndarray, rate: int, hz: float, start: float = 0.05, length: float = 0.5, periods: float = 8.0) -> float:
	"""The partial near hz, from the slope of its demodulated phase, weighted by amplitude.

	Only where the partial stands 16 pipe units clear: below that, the C++ output's
	truncation to whole units dominates its phase.
	"""
	envelope = demodulate(y[int(start * rate):int((start + length) * rate)], rate, hz, periods)
	amplitude = np.abs(envelope)
	weight = amplitude / amplitude.max()
	keep = (weight > 1e-3) & (amplitude > 16)
	t = np.arange(len(envelope)) / rate
	slope = np.polyfit(t[keep], np.unwrap(np.angle(envelope[keep])), 1, w=weight[keep])[0]
	return hz + slope / (2 * np.pi)


def t60(y: np.ndarray, rate: int, hz: float) -> float:
	"""A line fit to the partial's level from -5 to -35 dB below its peak, extrapolated to 60 dB."""
	level = 20 * np.log10(np.abs(demodulate(y, rate, hz)) + 1e-30)
	level -= level.max()
	after = level[int(np.argmax(level)):]
	first = int(np.argmax(after <= -5))
	last = int(np.argmax(after <= -35))
	t = np.arange(first, last) / rate
	slope = np.polyfit(t, after[first:last], 1)[0]
	return -60.0 / slope


def mode_level(y: np.ndarray, rate: int, hz: float, start: float = 0.05, length: float = 0.5) -> float:
	"""The Hann-windowed DFT magnitude at hz: its sidelobes fall fast, so neighbouring modes don't leak in."""
	segment = y[int(start * rate):int((start + length) * rate)]
	n = np.arange(len(segment))
	return float(np.abs(np.sum(segment * np.hanning(len(segment)) * np.exp(-2j * np.pi * hz * n / rate))))


def spectral_peak(y: np.ndarray, rate: int, hz: float, width: float, start: float = 0.05, length: float = 1.0) -> float:
	segment = y[int(start * rate):int((start + length) * rate)]
	segment = segment * np.hanning(len(segment))
	pad = 8 * len(segment)
	spectrum = np.log(np.abs(np.fft.rfft(segment, pad)) + 1e-30)
	freqs = np.fft.rfftfreq(pad, 1 / rate)
	window = np.nonzero(np.abs(freqs - hz) <= width * hz)[0]
	i = window[int(np.argmax(spectrum[window]))]
	a, b, c = spectrum[i - 1], spectrum[i], spectrum[i + 1]
	return freqs[i] + 0.5 * (a - c) / (a - 2 * b + c) * (freqs[1] - freqs[0])


def centroid(y: np.ndarray, rate: int) -> float:
	power = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
	freqs = np.fft.rfftfreq(len(y), 1 / rate)
	return float(np.sum(freqs * power) / np.sum(power))


def instantaneous_cents(y: np.ndarray, rate: int, hz: float, low: float, high: float) -> np.ndarray:
	"""Cents relative to hz of the analytic signal of y band-passed from low to high times hz."""
	spectrum = np.fft.fft(y)
	freqs = np.fft.fftfreq(len(y), 1 / rate)
	ratio = freqs / hz
	edge = 0.05
	band = np.clip(np.minimum((ratio - low) / edge, (high - ratio) / edge), 0, 1)
	band = np.where(freqs > 0, 2 * (0.5 - 0.5 * np.cos(np.pi * band)), 0)
	analytic = np.fft.ifft(spectrum * band)
	phase = np.unwrap(np.angle(analytic))
	inst_hz = np.diff(phase) * rate / (2 * np.pi)
	return CENTS_PER_OCTAVE * np.log2(np.maximum(inst_hz, 1e-9) / hz)


def renders(engine: Engine, jobs: list[h.Job]) -> list[np.ndarray]:
	return engine.run(jobs)


def hit(params: list[int], note: float = 60, velocity: float = 1.0, seconds: float = 1.0, rate: int = RATE) -> h.Job:
	return h.Job(rate=rate, params=params).hit(note, velocity).render(seconds)


# ---------------------------------------------------------------------------
# Cases. Each returns (passed, detail).
# ---------------------------------------------------------------------------

def t0_model_match(engine: Engine):
	"""The C++ and the model agree to one pipe unit."""
	jobs = [
		hit(voice(), seconds=0.4),
		hit(voice(model=1), seconds=0.4),
		hit(voice(model=1, drive=60, stiffness=70, low_cut_hz=120, cut_hz=900, cut_db=-9, damping=80), note=45, seconds=0.4),
		hit(voice(wires_level=70, wires_tension=30, tension_cents=500, stiffness=60, damping=10, tone_level=60, tone_shape=100), seconds=0.4),
		hit(voice(model=1, wires_level=50, tension_cents=300, position=90, tone_level=100, tone_pitch_cents=700), note=50, seconds=0.4, rate=44100),
		h.Job(params=voice(release_mode=1, release_ms=40)).hit(62, 0.7).render(0.1).off().render(0.1).pitch(67).render(0.05).hit(55, 0.4).render(0.2),
		h.Job(params=voice(model=1, release_mode=1, stiffness=40)).hit(48, 0.9).render(0.15, blocks=[5, 77]).off().render(0.05).hit(60, 0.5).render(0.2),
		hit(voice(decay_ms=60, noise_decay_ms=200), seconds=0.6),
	]
	cpp = h.run(jobs)
	reference = model.run(jobs)
	worst = max(float(np.abs(c - model.ints(m)).max()) for c, m in zip(cpp, reference))
	return worst <= 1.0, f"{len(jobs)} renders, worst difference {worst:.0f} pipe units (limit 1 = 1/8192)"


def t1_tuning(engine: Engine):
	"""Fundamental within 1 cent: both models, 44.1 and 48 kHz, filters at their extremes.

	Cut and Low Cut take their loss once per period, so at the fundamental they end the
	note within a few periods. Their extremes here are the strongest settings that still
	ring: Low Cut on the fundamental (the most phase lead), and the deepest Cut three
	octaves up. The measurement starts 2 ms after the hit, past the strike. A center
	hit leaves only the m = 0 modes, so a fundamental that dies early is not masked by
	the (1,1) mode outliving it.
	"""
	def extremes(note):
		f1 = note_hz(note)
		cut = {"cut_db": -24, "cut_hz": min(12000, round(8 * f1))}
		return {
			"default": {},
			"damping 0": {"damping": 0},
			"damping 100": {"damping": 100},
			"stiffness 100": {"stiffness": 100},
			"low cut at f1": {"low_cut_hz": round(f1)},
			"cut -24 at 8 f1": cut,
			"all": {"damping": 100, "stiffness": 100, "low_cut_hz": round(f1), **cut},
		}
	cases = []
	jobs = []
	for model_index in (0, 1):
		for rate in (44100, 48000):
			for note in engine.notes:
				for name, overrides in extremes(note).items():
					params = voice(model=model_index, decay_ms=4000, noise_level=0, direct=0, position=0, **overrides)
					cases.append((model_index, rate, note, name))
					jobs.append(hit(params, note=note, seconds=0.6, rate=rate))
	worst = (0.0, None)
	for case, y in zip(cases, renders(engine, jobs)):
		error = cents(frequency(y, case[1], note_hz(case[2]), start=0.002, periods=2.0), note_hz(case[2]))
		if abs(error) > abs(worst[0]):
			worst = (error, case)
	return abs(worst[0]) <= 1.0, f"{len(jobs)} renders, worst {worst[0]:+.3f} cents at model {worst[1][0]}, {worst[1][1]} Hz, note {worst[1][2]}, {worst[1][3]}"


def t2_membrane_ratios(engine: Engine):
	"""The first 8 mode peaks within 0.5 % of the Bessel table."""
	f1 = note_hz(60)
	y = renders(engine, [hit(voice(position=80, damping=0, decay_ms=4000, noise_level=0, direct=0), seconds=1.2)])[0]
	worst = 0.0
	for ratio, _ in model.MODES[:8]:
		measured = spectral_peak(y, RATE, ratio * f1, 0.008) / f1
		worst = max(worst, abs(measured / ratio - 1))
	return worst <= 0.005, f"worst ratio error {100 * worst:.3f} %"


def t3_decay(engine: Engine):
	"""The fundamental's T60 within 10 % of Decay, notes 36-84, both models."""
	jobs, cases = [], []
	for model_index in (0, 1):
		for note in engine.notes[::4] if engine is CPP else engine.notes:
			jobs.append(hit(voice(model=model_index, noise_level=0, direct=0), note=note, seconds=1.2))
			cases.append((model_index, note))
	worst = (0.0, None)
	for case, y in zip(cases, renders(engine, jobs)):
		error = t60(y, RATE, note_hz(case[1])) / 0.8 - 1
		if abs(error) > abs(worst[0]):
			worst = (error, case)
	return abs(worst[0]) <= 0.10, f"{len(jobs)} renders, worst {100 * worst[0]:+.1f} % at model {worst[1][0]}, note {worst[1][1]}"


def t4_passivity(engine: Engine):
	"""The loop's gain is at most 1 at every frequency and param corner; a max-Decay hit stays bounded and idles in time."""
	talus = model.Talus()
	talus.initialize(RATE)
	worst = 0.0
	corners = 0
	w = np.linspace(1e-4, np.pi, 4096)
	z1 = np.exp(-1j * w)
	for note in (0, 36, 60, 96, 127):
		for tune in (-48, 0, 48):
			for damping in (0, 100):
				for decay in (20, 10000):
					for low_cut in (1, 2000):
						for cut_hz, cut_db in ((100, -24), (12000, -24), (2000, 0)):
							for stiffness in (0, 100):
								talus.set_params(voice(model=1, tune_semitones=tune, damping=damping, decay_ms=decay, low_cut_hz=low_cut, cut_hz=cut_hz, cut_db=cut_db, stiffness=stiffness))
								talus.pitch = note * 64
								talus.retune()
								c, q = talus.lowpass_coeff, 1 - talus.lowpass_coeff
								hp = 1 - talus.highpass_coeff / (1 - (1 - talus.highpass_coeff) * z1)
								b0, b1, b2, a1, a2 = talus.cut
								cut = (b0 + b1 * z1 + b2 * z1 ** 2) / (1 + a1 * z1 + a2 * z1 ** 2)
								gain = talus.loop_gain * np.abs(c / (1 - q * z1) * hp * cut)
								worst = max(worst, float(gain.max()))
								corners += 1
	jobs = [h.Job(params=voice(model=m, decay_ms=10000, damping=0)).hit(60).until_idle(30.0, 480) for m in (0, 1)]
	if engine is CPP:
		outputs, lines = h.run_with_report(jobs)
		idle = [int(v) / RATE for v in h.report_values(lines, "IDLE")]
	else:
		outputs = model.run(jobs)
		idle = [len(y) / RATE if len(y) < 30 * RATE else -1 for y in outputs]
	bounded = all(np.abs(y[5 * RATE:]).max() <= np.abs(y[:5 * RATE]).max() for y in outputs)
	in_time = all(0 < t <= 2 * 10.0 + 0.05 for t in idle)
	return worst <= 1.0 and bounded and in_time, f"max loop gain {worst:.6f} over {corners} corners; idle after {', '.join(f'{t:.2f}' for t in idle)} s (limit 20.05); bounded {bounded}"


def t5_position(engine: Engine):
	"""Center hit: (1,1) and (2,1) below -60 dB re (0,1). At Position 70 within 20 dB."""
	f1 = note_hz(60)
	ratios = [mode[0] for mode in model.MODES[:3]]
	center, outer = renders(engine, [hit(voice(position=p, noise_level=0, direct=0, decay_ms=2000), seconds=0.6) for p in (0, 70)])
	def levels(y):
		base = mode_level(y, RATE, f1)
		return [20 * math.log10(mode_level(y, RATE, r * f1) / base + 1e-30) for r in ratios[1:]]
	at_center, at_outer = levels(center), levels(outer)
	passed = max(at_center) < -60 and min(at_outer) > -20
	return passed, f"center {at_center[0]:.0f}/{at_center[1]:.0f} dB, position 70 {at_outer[0]:.1f}/{at_outer[1]:.1f} dB"


def t6_brightness(engine: Engine):
	"""The first 50 ms grow brighter with every velocity step from 0.1 to 1.0."""
	velocities = [v / 10 for v in range(1, 11)]
	outputs = renders(engine, [hit(voice(strike_velocity=100), velocity=v, seconds=0.05) for v in velocities])
	centroids = [centroid(y, RATE) for y in outputs]
	rising = all(b > a for a, b in zip(centroids, centroids[1:]))
	return rising, "centroid " + " ".join(f"{c:.0f}" for c in centroids) + " Hz"


def t7_ghost_wires(engine: Engine):
	"""The wires-to-head energy ratio of a ghost note (v 0.2) is above a full hit's."""
	def ratio(velocity):
		head, wires = renders(engine, [
			hit(voice(strike_velocity=100, wires_level=60, head=100, direct=0), velocity=velocity, seconds=0.5),
			hit(voice(strike_velocity=100, wires_level=60, head=0, direct=0), velocity=velocity, seconds=0.5),
		])
		head_only = renders(engine, [hit(voice(strike_velocity=100, wires_level=0, direct=0), velocity=velocity, seconds=0.5)])[0]
		return float(np.sum(wires ** 2) / np.sum(head_only ** 2))
	ghost, full = ratio(0.2), ratio(1.0)
	return ghost > full, f"wires/head energy: v 0.2 {ghost:.3f}, v 1.0 {full:.3f}"


def t8_tension(engine: Engine):
	"""Tension 600: a full hit is 300+ cents sharp at 10 ms; a v 0.2 hit glides under a tenth of that."""
	f1 = note_hz(60)
	def pitch_at_10ms(tension, velocity):
		params = voice(tension_cents=tension, strike_velocity=100, position=0, noise_level=0, direct=0)
		y = renders(engine, [hit(params, velocity=velocity, seconds=0.7)])[0]
		inst = instantaneous_cents(y, RATE, f1, 0.8, 1.9)
		return float(np.mean(inst[int(0.008 * RATE):int(0.012 * RATE)])) - float(np.mean(inst[int(0.5 * RATE):int(0.6 * RATE)]))
	# The same measurement at Tension 0 is the band-pass's own error at 10 ms.
	glides = [pitch_at_10ms(600, v) - pitch_at_10ms(0, v) for v in (1.0, 0.2)]
	return glides[0] >= 300 and glides[1] < 0.1 * glides[0], f"glide at 10 ms: v 1.0 {glides[0]:.0f} cents, v 0.2 {glides[1]:.1f} cents"


def t9_restrike(engine: Engine):
	"""With Tension and Wires off, two hits equal the sum of each alone (a zero-force hit keeps the second's place)."""
	worst = 0.0
	for model_index in (0, 1):
		params = voice(model=model_index, strike_velocity=100, tone_level=40)
		gap = int(0.15 * RATE)
		both, first, second = renders(engine, [
			h.Job(params=params).hit(60, 0.8).render(samples=gap).hit(60, 0.6).render(0.5),
			h.Job(params=params).hit(60, 0.8).render(samples=gap).render(0.5),
			h.Job(params=params).hit(60, 0.0).render(samples=gap).hit(60, 0.6).render(0.5),
		])
		error = np.abs(both - (first + second)).max()
		worst = max(worst, float(error if engine.quantized else error / np.abs(both).max()))
	if engine.quantized:
		return worst <= 2, f"worst difference {worst:.0f} pipe units (each render truncates to whole units, so 2 is exact)"
	return worst <= 1e-9, f"worst difference {worst:.2e} of the peak"


def t10_sample_rate(engine: Engine):
	"""44.1 and 48 kHz agree: f1 within 1 cent, T60 within 10 %, the wire release within 10 %.

	The wire attack (0.3 ms) is shorter than any envelope the noisy output can resolve,
	so the release is the envelope time measured: a line fit to the wires' level from
	-5 to -35 dB, with the head's decay short enough that the envelope's release sets it.
	"""
	results = {}
	for rate in (44100, 48000):
		tuned, decayed, wires = renders(engine, [
			hit(voice(model=1, decay_ms=4000, damping=80, stiffness=50, noise_level=0, direct=0), note=50, seconds=0.6, rate=rate),
			hit(voice(noise_level=0, direct=0), note=55, seconds=1.2, rate=rate),
			hit(voice(head=0, direct=0, wires_level=100, wires_decay_ms=300, decay_ms=100), seconds=1.0, rate=rate),
		])
		window = int(0.01 * rate)
		rms = np.sqrt(np.convolve(wires ** 2, np.ones(window) / window, mode="valid"))
		level = 20 * np.log10(rms / rms.max() + 1e-12)
		after = level[int(np.argmax(level)):]
		first, last = int(np.argmax(after <= -5)), int(np.argmax(after <= -35))
		release = -60.0 / np.polyfit(np.arange(first, last) / rate, after[first:last], 1)[0]
		results[rate] = (frequency(tuned, rate, note_hz(50)), t60(decayed, rate, note_hz(55)), release)
	a, b = results[44100], results[48000]
	f_cents = cents(a[0], b[0])
	t60_error = a[1] / b[1] - 1
	release_error = a[2] / b[2] - 1
	passed = abs(f_cents) <= 1 and abs(t60_error) <= 0.1 and abs(release_error) <= 0.1
	return passed, f"f1 {f_cents:+.3f} cents, T60 {100 * t60_error:+.1f} %, wire release {1000 * a[2]:.0f}/{1000 * b[2]:.0f} ms ({100 * release_error:+.1f} %)"


def t11_segments(engine: Engine):
	"""Blocks of [1, 7, 64, 333, rest] render bit-identically to one block."""
	def job(model_index, blocks):
		params = voice(model=model_index, wires_level=50, tension_cents=300, decay_ms=150, variation=40, drive=30, release_mode=1)
		return (h.Job(params=params).hit(60, 0.9).render(0.2, blocks=blocks).hit(64, 0.5).render(0.1, blocks=blocks)
			.off().render(0.6, blocks=blocks).hit(57, 0.7).render(0.3, blocks=blocks))
	pairs = renders(engine, [job(m, b) for m in (0, 1) for b in (None, [1, 7, 64, 333])])
	identical = [np.array_equal(pairs[i], pairs[i + 1]) for i in (0, 2)]
	return all(identical), f"membrane {identical[0]}, loop {identical[1]}"


def t12_determinism(engine: Engine):
	"""Two renders from initialize are bit-identical, Variation and noise included."""
	jobs = [h.Job(params=h.talus_params(wires_level=40)).hit(60).render(0.2).hit(62, 0.5).render(0.3) for _ in range(2)]
	a, b = renders(engine, jobs)
	return np.array_equal(a, b), f"{len(a)} samples"


def t13_level(engine: Engine):
	"""A default full-force hit peaks within 1 dB of Iron's single-note level, -7 dBFS."""
	peaks = [20 * math.log10(np.abs(y).max() / h.PIPE_PEAK) for y in renders(engine, [hit(voice(model=m), seconds=0.5) for m in (0, 1)])]
	return all(abs(p + 7) <= 1 for p in peaks), f"membrane {peaks[0]:.2f} dBFS, loop {peaks[1]:.2f} dBFS"


def t14_cpu(engine: Engine):
	"""A 48-mode Membrane voice and a Loop voice each cost no more than an Iron voice.

	Timing on a laptop is noisy, so the three voices run interleaved and each keeps its
	best of five runs.
	"""
	timings = {}
	for _ in range(5):
		measured = h.run_timing([
			h.Job(params=voice(decay_ms=10000)).hit(60).time("membrane", 10.0),
			h.Job(params=voice(model=1, decay_ms=10000)).hit(60).time("loop", 10.0),
			h.Job("iron").hit(40).time("iron", 10.0),
		])
		for label, value in measured.items():
			timings[label] = min(timings.get(label, value), value)
	passed = timings["membrane"] <= timings["iron"] and timings["loop"] <= timings["iron"]
	return passed, ", ".join(f"{label} {ns:.0f} ns/sample ({100 * ns * RATE / 1e9:.2f} % of a core)" for label, ns in timings.items())


def iron_identity(engine: Engine):
	"""Iron renders bit-identically to its baseline build."""
	jobs = [
		h.Job("iron").hit(40, 1.0).render(1.0),
		h.Job("iron", params=[2, 80, 40, 60, 50, 6000, 80, 70, 50, 90, 200, 4, -3, 6, 1, 70, 0, 0]).hit(45, 0.6).render(0.5).off().render(0.3),
		h.Job("iron", rate=44100).hit(52, 0.8).render(0.4).pitch(55).render(0.2),
	]
	current = h.run(jobs)
	baseline = h.run(jobs, h.IRON_BASELINE)
	identical = all(np.array_equal(a, b) for a, b in zip(current, baseline))
	return identical, f"{len(jobs)} renders"


CASES = {
	"T0": (t0_model_match, ("cpp",)),
	"T1": (t1_tuning, ("cpp", "model")),
	"T2": (t2_membrane_ratios, ("cpp", "model")),
	"T3": (t3_decay, ("cpp", "model")),
	"T4": (t4_passivity, ("cpp", "model")),
	"T5": (t5_position, ("cpp", "model")),
	"T6": (t6_brightness, ("cpp", "model")),
	"T7": (t7_ghost_wires, ("cpp", "model")),
	"T8": (t8_tension, ("cpp", "model")),
	"T9": (t9_restrike, ("cpp", "model")),
	"T10": (t10_sample_rate, ("cpp", "model")),
	"T11": (t11_segments, ("cpp", "model")),
	"T12": (t12_determinism, ("cpp", "model")),
	"T13": (t13_level, ("cpp", "model")),
	"T14": (t14_cpu, ("cpp",)),
	"IRON": (iron_identity, ("cpp",)),
}


def main() -> None:
	parser = argparse.ArgumentParser()
	parser.add_argument("--engine", choices=["cpp", "model", "both"], default="both")
	parser.add_argument("--case", nargs="*", default=list(CASES))
	args = parser.parse_args()
	engines = [e for e in (CPP, MODEL) if args.engine in (e.name, "both")]
	failures = 0
	total = 0
	for name in args.case:
		test, supported = CASES[name]
		for engine in engines:
			if engine.name not in supported:
				continue
			total += 1
			start = time.time()
			try:
				passed, detail = test(engine)
			except Exception as error:  # A measurement that cannot run is a failure, reported as such.
				passed, detail = False, f"{type(error).__name__}: {error}"
			failures += not passed
			print(f"{'PASS' if passed else 'FAIL'} {name:4s} {engine.name:5s} {detail} ({time.time() - start:.1f} s)", flush=True)
	print(f"{total - failures}/{total} passed")
	sys.exit(1 if failures else 0)


if __name__ == "__main__":
	main()
