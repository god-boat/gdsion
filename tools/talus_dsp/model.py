"""A reference model of the Talus channel in Python and numpy, written from the
design (docs/TalusPhysicalModelDesign.md in the app repo), not from the C++.

It reads the same job scripts as render_main.cpp. Tuning constants (the
calibrated levels, ranges and times) are read from siopm_channel_talus.cpp, so
recalibrating never desyncs the two; every formula is written out here again.
With Variation at 0 the model and the C++ must agree to one pipe unit.

model.run(jobs) returns float pipe values before the int cast; ints(x) applies
the C++ truncation.
"""

import math
import re

import numpy as np

import harness as h

_SOURCE = (h.GDSION / "src" / "chip" / "channels" / "siopm_channel_talus.cpp").read_text(encoding="utf-8")
_MODES_SOURCE = (h.GDSION / "src" / "dsp" / "membrane_modes.h").read_text(encoding="utf-8")


def _constant(name: str) -> float:
	match = re.search(rf"static constexpr (?:double|int|uint32_t) {name} = ([0-9.eu+-]+);", _SOURCE)
	return float(match.group(1).rstrip("u"))


C = {name: _constant(name) for name in [
	"PIPE_PEAK", "LN2_PER_CENT", "LN1000", "NEPERS_PER_DB", "REFERENCE_RATE", "UPDATE_INTERVAL",
	"SOFT_CONTACT_SECONDS", "HARD_CONTACT_SECONDS", "FORCE_FLOOR", "POSITION_REACH", "NOISE_Q",
	"TONE_DRIVE_SINE", "TONE_DRIVE_SQUARE", "EXCITER_FLOOR", "NOISE_FORCE", "TONE_FORCE", "DIRECT_GAIN",
	"WIRES_GAIN", "VARIATION_POSITION", "VARIATION_HARDNESS", "VARIATION_FORCE_DB", "VARIATION_TUNE_CENTS",
	"VARIATION_TONE_CENTS", "MODE_CEILING_HZ", "MODE_CEILING_FRACTION", "CUT_Q", "SEMITONE_SQUARED",
	"STIFFNESS_PARTIAL", "DELAY_LINE_SAMPLES", "LOOP_REACH", "SHORTEST_PERIOD", "MIN_READ_DELAY",
	"LOSS_FIT_RATIO", "ALLPASS_LIMIT", "ALLPASS_FIT_STEPS", "POLE_STEPS", "DELAY_SMOOTHING_SECONDS", "DRIVE_MAX",
	"TENSION_NORMAL", "TENSION_ENERGY_MAX", "TENSION_ATTACK_SECONDS", "TENSION_RELEASE_SECONDS",
	"TENSION_RELEASE_PERIODS", "TENSION_STEP_CENTS", "WIRE_HEAD_REFERENCE", "WIRE_KNEE_RANGE_DB", "WIRE_Q",
	"WIRE_ATTACK_SECONDS", "DIRECT_HIGHPASS_HZ", "SILENCE_PIPE_UNITS",
]}
C["HIT_STRIDE"] = 1 << 20
C["LOSS_FIT_LIMIT"] = 0.9 * math.pi
OUTPUT_TRIM = [float(v) for v in re.search(r"OUTPUT_TRIM\[\] = \{ ([^}]*) \}", _SOURCE).group(1).split(",")]

MODES = []
for ratio, m, n, weights in re.findall(r"\{ ([0-9.]+), (\d+), (\d+), \{ ([^}]*) \} \}", _MODES_SOURCE):
	MODES.append((float(ratio), [float(w) for w in weights.split(",")]))
POSITION_POINTS = len(MODES[0][1])
STIFFNESS_PARTIAL = int(C["STIFFNESS_PARTIAL"])
_R8 = MODES[STIFFNESS_PARTIAL - 1][0]
MEMBRANE_STIFFNESS_MAX = (C["SEMITONE_SQUARED"] - 1.0) / (_R8 * _R8 - C["SEMITONE_SQUARED"])
LOOP_STIFFNESS_MAX = (C["SEMITONE_SQUARED"] - 1.0) / (STIFFNESS_PARTIAL * STIFFNESS_PARTIAL - C["SEMITONE_SQUARED"])

S = {name: index for index, name in enumerate(h.TALUS_SLOTS)}


# ---------------------------------------------------------------------------
# Building blocks
# ---------------------------------------------------------------------------

def lfo_random(step: int) -> float:
	"""lowbias32 of the step, mapped to [-1, 1)."""
	x = (step ^ 0x9E3779B9) & 0xFFFFFFFF
	x ^= x >> 16
	x = (x * 0x7FEB352D) & 0xFFFFFFFF
	x ^= x >> 15
	x = (x * 0x846CA68B) & 0xFFFFFFFF
	x ^= x >> 16
	return x * (2.0 / 4294967296.0) - 1.0


def fast_tanh(x: float) -> float:
	if x >= 3.0:
		return 1.0
	if x <= -3.0:
		return -1.0
	x2 = x * x
	return x * (27.0 + x2) / (27.0 + 9.0 * x2)


def clamp(value, low, high):
	return low if value < low else (high if value > high else value)


def one_pole_coeff(hz: float, rate: float) -> float:
	return 1.0 - math.exp(-2.0 * math.pi * hz / rate)


def z_inverse(w: float, radius: float = 1.0) -> complex:
	"""z^-1 at angle w and the given radius."""
	return complex(math.cos(w), -math.sin(w)) / radius


def lowpass_response(coeff: float, w: float, radius: float = 1.0) -> complex:
	return coeff / (1.0 - (1.0 - coeff) * z_inverse(w, radius))


def highpass_response(coeff: float, w: float, radius: float = 1.0) -> complex:
	return 1.0 - lowpass_response(coeff, w, radius)


def allpass_response(coeff: float, w: float, radius: float = 1.0) -> complex:
	z1 = z_inverse(w, radius)
	return (coeff + z1) / (1.0 + coeff * z1)


def phase_delay(response: complex, w: float) -> float:
	return -math.atan2(response.imag, response.real) / w


def biquad(kind: str, hz: float, q: float, gain_db: float, rate: float) -> tuple:
	"""RBJ cookbook coefficients (b0, b1, b2, a1, a2), normalized."""
	hz = clamp(hz, 10.0, rate * 0.5 * 0.99)
	omega = 2.0 * math.pi * hz / rate
	sin_w, cos_w = math.sin(omega), math.cos(omega)
	alpha = sin_w / (2.0 * max(q, 0.01))
	if kind == "peak":
		a = 10.0 ** (gain_db / 40.0)
		raw = (1.0 + alpha * a, -2.0 * cos_w, 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * cos_w, 1.0 - alpha / a)
	else:  # band-pass, 0 dB peak
		raw = (alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cos_w, 1.0 - alpha)
	b0, b1, b2, a0, a1, a2 = raw
	return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def biquad_response(coeffs: tuple, w: float, radius: float = 1.0) -> complex:
	b0, b1, b2, a1, a2 = coeffs
	z1 = z_inverse(w, radius)
	z2 = z1 * z1
	return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2)


class Biquad:
	def __init__(self):
		self.x1 = self.x2 = self.y1 = self.y2 = 0.0

	def tick(self, c: tuple, x: float) -> float:
		y = c[0] * x + c[1] * self.x1 + c[2] * self.x2 - c[3] * self.y1 - c[4] * self.y2
		self.x2, self.x1, self.y2, self.y1 = self.x1, x, self.y1, y
		return y


class Delay:
	"""Power-of-two delay line with four-point Lagrange reads (1 = last written)."""

	def __init__(self, max_samples: float):
		size = 1
		while size < int(max_samples) + 1:
			size *= 2
		self.buffer = [0.0] * size
		self.mask = size - 1
		self.write_index = 0

	def clear(self):
		self.buffer = [0.0] * (self.mask + 1)
		self.write_index = 0

	def read_cubic(self, delay: float) -> float:
		position = (self.write_index + self.mask + 1) - delay
		index = int(position)
		f = position - index
		buf, m = self.buffer, self.mask
		a, b, c, d = buf[(index - 1) & m], buf[index & m], buf[(index + 1) & m], buf[(index + 2) & m]
		return b + f * ((c - b) - (1.0 - f) / 6.0 * ((2.0 - f) * (a - 2.0 * b + c) + (1.0 + f) * (b - 2.0 * c + d)))

	def write(self, x: float):
		self.buffer[self.write_index] = x
		self.write_index = (self.write_index + 1) & self.mask


class Exciter:
	def __init__(self):
		self.remaining = 0
		self.pulse_samples = 0
		self.pulse_step = 0.0
		self.pulse = 0.0
		self.pulse_previous = 0.0
		self.noise_step = 0
		self.noise_level = 0.0
		self.noise_band = Biquad()
		self.tone_cos, self.tone_sin = 1.0, 0.0
		self.tone_re, self.tone_im = 1.0, 0.0
		self.tone_level = 0.0


# ---------------------------------------------------------------------------
# The channel
# ---------------------------------------------------------------------------

class Talus:
	def __init__(self, seed: int = 0):
		self.seed = seed
		self.loop = Delay(C["DELAY_LINE_SAMPLES"])
		self.model = 0
		self.read_target = self.read_delay = 183.0

	def initialize(self, rate: float):
		self.rate = rate
		self.resonator_input = C["REFERENCE_RATE"] / rate
		self.direct_coeff = one_pole_coeff(C["DIRECT_HIGHPASS_HZ"], rate)
		self.wire_attack = 1.0 - math.exp(-1.0 / (C["WIRE_ATTACK_SECONDS"] * rate))
		self.tension_attack = 1.0 - math.exp(-1.0 / (C["TENSION_ATTACK_SECONDS"] * rate))
		self.delay_smoothing = 1.0 - math.exp(-1.0 / (C["DELAY_SMOOTHING_SECONDS"] * rate))
		self.pitch = 60 * 64
		self.pitch_changed = False
		self.expression = 1.0
		self.position = 0.3
		self.idling = True
		self.reset()
		self.set_params(list(h.TALUS_DEFAULTS))

	def reset(self):
		self.clear()
		self.hit_index = 0
		self.hit_position = C["POSITION_REACH"] * self.position
		self.hit_cents = 0.0
		self.muted = False
		self.countdown = 1
		self.wire_step = self.seed ^ 0x80000000
		self.idling = True

	def clear(self):
		self.exciters = [Exciter(), Exciter()]
		self.state = np.zeros(len(MODES), dtype=complex)
		self.loop.clear()
		self.loop_lowpass = 0.0
		self.loop_highpass = 0.0
		self.loop_cut = Biquad()
		self.allpasses = [[0.0, 0.0], [0.0, 0.0]]
		self.quiet = 0
		self.power = 0.0
		self.tension_applied = 0.0
		self.motion = 0.0
		self.wire_envelope = 0.0
		self.wire_band = Biquad()
		self.direct_highpass = 0.0

	# Params -------------------------------------------------------------

	def set_params(self, values: list[int]):
		v = lambda name, low, high: clamp(values[S[name]], low, high)
		rate = self.rate
		self.strike_level = v("STRIKE_LEVEL", 0, 100) * 0.01
		self.hardness = v("STRIKE_HARDNESS", 0, 100) * 0.01
		self.velocity_amount = v("STRIKE_VELOCITY", 0, 100) * 0.01
		self.noise_level = v("NOISE_LEVEL", 0, 100) * 0.01
		self.noise_decay = math.exp(-C["LN1000"] / (v("NOISE_DECAY_MS", 1, 200) * 0.001 * rate))
		self.noise_band = biquad("band", v("NOISE_COLOR_HZ", 200, 16000), C["NOISE_Q"], 0.0, rate)
		self.tone_level = v("TONE_LEVEL", 0, 100) * 0.01
		self.tone_cents = v("TONE_PITCH_CENTS", -2400, 2400)
		self.tone_drive = C["TONE_DRIVE_SINE"] + (C["TONE_DRIVE_SQUARE"] - C["TONE_DRIVE_SINE"]) * v("TONE_SHAPE", 0, 100) * 0.01
		self.tone_normal = 1.0 / fast_tanh(self.tone_drive)
		self.tone_decay = math.exp(-C["LN1000"] / (v("TONE_DECAY_MS", 1, 200) * 0.001 * rate))
		self.variation = v("VARIATION", 0, 100) * 0.01
		model = v("MODEL", 0, 1)
		self.tune = v("TUNE_SEMITONES", -48, 48) + v("FINE_CENTS", -100, 100) * 0.01
		self.keytrack = v("KEYTRACK", 0, 100) * 0.01
		self.decay = v("DECAY_MS", 20, 10000) * 0.001
		self.damping_power = 3.0 * v("DAMPING", 0, 100) * 0.01
		self.position = v("POSITION", 0, 100) * 0.01
		self.stiffness = v("STIFFNESS", 0, 100) * 0.01
		self.low_cut = v("LOW_CUT_HZ", 1, 2000)
		self.cut = biquad("peak", v("CUT_HZ", 100, 12000), C["CUT_Q"], v("CUT_DB", -24, 0), rate)
		self.tension_cents = v("TENSION_CENTS", 0, 1200)
		self.drive = C["DRIVE_MAX"] * v("DRIVE", 0, 100) * 0.01
		self.inverse_drive = 1.0 / self.drive if self.drive > 0.0 else 1.0
		self.wires_level = v("WIRES_LEVEL", 0, 100) * 0.01
		wire_tension = v("WIRES_TENSION", 0, 100) * 0.01
		self.wire_inverse_knee = 1.0 / (C["WIRE_HEAD_REFERENCE"] * math.exp((wire_tension - 1.0) * C["WIRE_KNEE_RANGE_DB"] * C["NEPERS_PER_DB"]))
		self.wire_tone = biquad("band", v("WIRES_TONE_HZ", 1000, 12000), C["WIRE_Q"], 0.0, rate)
		self.wire_release = 1.0 - math.exp(-C["LN1000"] / (v("WIRES_DECAY_MS", 5, 800) * 0.001 * rate))
		self.head_level = v("HEAD", 0, 100) * 0.01
		self.direct_level = v("DIRECT", 0, 100) * 0.01
		self.release_mode = v("RELEASE_MODE", 0, 1)
		self.release = v("RELEASE_MS", 5, 2000) * 0.001
		self.output_gain = OUTPUT_TRIM[model] * C["PIPE_PEAK"]
		self.head_silence = C["SILENCE_PIPE_UNITS"] / self.output_gain
		if model != self.model:
			self.model = model
			self.clear()
		self.retune()

	# Tuning -------------------------------------------------------------

	def retune(self):
		note = 60.0 + (self.pitch / 64.0 - 60.0) * self.keytrack + self.tune + self.hit_cents * 0.01
		self.f1 = 440.0 * 2.0 ** ((note - 69.0) / 12.0)
		self.motion_coeff = one_pole_coeff(4.0 * self.f1, self.rate)
		self.tension_release = 1.0 - math.exp(-1.0 / (max(C["TENSION_RELEASE_SECONDS"], C["TENSION_RELEASE_PERIODS"] / self.f1) * self.rate))
		if self.model == 0:
			self.retune_membrane()
		else:
			self.retune_loop()
		self.apply_tension(self.tension_applied)

	def retune_membrane(self):
		"""Each mode: frequency from the ratio and stiffness, decay from Decay, Damping, Cut and Low Cut."""
		rate = self.rate
		ceiling = min(C["MODE_CEILING_FRACTION"] * rate, C["MODE_CEILING_HZ"]) * 2.0 ** (-C["TENSION_ENERGY_MAX"] * self.tension_cents / 1200.0)
		b = self.stiffness * MEMBRANE_STIFFNESS_MAX
		highpass = one_pole_coeff(self.low_cut, rate)
		x = self.hit_position * (POSITION_POINTS - 1)
		index = min(int(x), POSITION_POINTS - 2)
		fraction = x - index
		w, radius, gain = [], [], []
		for ratio0, weights in MODES:
			ratio = ratio0 * math.sqrt((1.0 + b * ratio0 * ratio0) / (1.0 + b))
			hz = self.f1 * ratio
			if hz >= ceiling:
				break
			wk = 2.0 * math.pi * hz / rate
			t60 = self.decay * ratio ** -self.damping_power
			if self.muted:
				t60 = min(t60, self.release)
			# Cut and Low Cut: their loss in nepers, once per fundamental period.
			kept = abs(highpass_response(highpass, wk)) * abs(biquad_response(self.cut, wk))
			w.append(wk)
			radius.append(math.exp((math.log(kept) * self.f1 - C["LN1000"] / t60) / rate))
			gain.append(weights[index] + (weights[index + 1] - weights[index]) * fraction)
		count = len(w)
		self.state[count:] = 0.0
		self.mode_w = np.array(w)
		self.mode_radius = np.array(radius)
		self.mode_gain = np.array(gain)

	def retune_loop(self):
		rate = self.rate
		self.period = clamp(rate / self.f1, C["SHORTEST_PERIOD"], (self.loop.mask - 4) / C["LOOP_REACH"])
		p = self.period
		w1 = 2.0 * math.pi / p
		# At most 60 dB per pass: a period longer than the T60 rings once.
		t60 = max(min(self.decay, self.release) if self.muted else self.decay, p / rate)
		decay = math.exp(-C["LN1000"] * p / (t60 * rate))
		# Lowpass slope from the loss curve between w1 and w_fit; quadratic in the pole.
		w_fit = min(C["LOSS_FIT_RATIO"] * w1, C["LOSS_FIT_LIMIT"])
		kept = math.exp(-C["LN1000"] * ((w_fit / w1) ** self.damping_power - 1.0) * p / (t60 * rate))
		qa = kept * kept - 1.0
		qb = math.cos(w1) - kept * kept * math.cos(w_fit)
		disc = qb * qb - qa * qa
		pole = qa / (-qb - math.sqrt(disc)) if disc > 0.0 else 1.0
		coeff = 1.0 - pole
		floor = math.sqrt(decay)
		if abs(lowpass_response(coeff, w1)) < floor:
			# The coefficient whose |H(w1)| is the floor: |H|^2 = c^2 / (1 - 2 q cos w + q^2), solved for c.
			m2 = floor * floor
			bb = 1.0 - m2
			aa = 1.0 - m2 * math.cos(w1)
			coeff = 1.0 - bb / (aa + math.sqrt(aa * aa - bb * bb))
		self.lowpass_coeff = coeff
		self.loop_gain = decay / abs(lowpass_response(coeff, w1))
		self.highpass_coeff = one_pole_coeff(self.low_cut, rate)
		# Tune at the fundamental's pole, inside the unit circle. The pole satisfies
		# g F(z) z^-D = 1: the phase sets the read delay D, and the radius is where one
		# pass keeps exactly r^D. Start from the circle's magnitudes, then iterate.
		kept_per_pass = decay * abs(highpass_response(self.highpass_coeff, w1)) * abs(biquad_response(self.cut, w1))
		radius = kept_per_pass ** (1.0 / p)
		self.allpass_coeff = self.fit_allpass(w1, radius)
		for _ in range(int(C["POLE_STEPS"])):
			allpass = phase_delay(allpass_response(self.allpass_coeff, w1, radius), w1)
			read_delay = p - self.filter_delay(w1, radius) - 2.0 * allpass
			loop = (lowpass_response(self.lowpass_coeff, w1, radius) * highpass_response(self.highpass_coeff, w1, radius)
					* biquad_response(self.cut, w1, radius) * allpass_response(self.allpass_coeff, w1, radius) ** 2)
			radius = (self.loop_gain * abs(loop)) ** (1.0 / read_delay)
		self.loop_phase_delay = self.filter_delay(w1, radius) + 2.0 * phase_delay(allpass_response(self.allpass_coeff, w1, radius), w1)
		self.loop_input_scale = 0.5 * p
		self.tap_fraction = 0.5 * (1.0 - self.hit_position)

	def filter_delay(self, w: float, radius: float) -> float:
		return (phase_delay(lowpass_response(self.lowpass_coeff, w, radius), w)
				+ phase_delay(highpass_response(self.highpass_coeff, w, radius), w)
				+ phase_delay(biquad_response(self.cut, w, radius), w))

	def fit_allpass(self, w1: float, radius: float) -> float:
		if self.stiffness == 0.0:
			return 0.0
		k = min(float(STIFFNESS_PARTIAL), math.floor(C["MODE_CEILING_FRACTION"] * self.period))
		b = self.stiffness * LOOP_STIFFNESS_MAX
		w_target = w1 * k * math.sqrt((1.0 + b * k * k) / (1.0 + b))
		filters = self.filter_delay(w1, radius)
		fixed = self.period - filters + self.filter_delay(w_target, radius)
		sharp, flat = C["ALLPASS_LIMIT"], 0.0
		for _ in range(int(C["ALLPASS_FIT_STEPS"])):
			coeff = 0.5 * (sharp + flat)
			at_w1 = 2.0 * phase_delay(allpass_response(coeff, w1, radius), w1)
			phase = w_target * (fixed - at_w1 + 2.0 * phase_delay(allpass_response(coeff, w_target, radius), w_target)) - 2.0 * math.pi * k
			if phase < 0.0 or self.period - filters - at_w1 < C["MIN_READ_DELAY"]:
				sharp = coeff
			else:
				flat = coeff
		return flat

	def apply_tension(self, cents: float):
		self.tension_applied = cents
		scale = math.exp(cents * C["LN2_PER_CENT"])
		if self.model == 0:
			angle = self.mode_w * scale
			self.mode_z = self.mode_radius * np.cos(angle) + 1j * (self.mode_radius * np.sin(angle))
		else:
			self.read_target = max(C["MIN_READ_DELAY"], self.period / scale - self.loop_phase_delay)

	# Notes --------------------------------------------------------------

	def note_on(self):
		waking = self.idling
		self.pitch_changed = False
		self.strike(clamp(self.expression, 0.0, 1.0))
		if waking:
			self.read_delay = self.read_target
			self.countdown = 1
		self.idling = False

	def note_off(self):
		if self.release_mode == 1:
			self.muted = True
			self.retune()

	def strike(self, velocity: float):
		base = (self.seed + self.hit_index * C["HIT_STRIDE"]) & 0xFFFFFFFF
		exciter = self.exciters[self.hit_index % 2]
		self.hit_index += 1
		draws = [lfo_random((base + i) & 0xFFFFFFFF) * self.variation for i in range(5)]

		force = (1.0 - self.velocity_amount * (1.0 - velocity)) * math.exp(C["VARIATION_FORCE_DB"] * C["NEPERS_PER_DB"] * draws[2])
		self.hit_position = C["POSITION_REACH"] * clamp(self.position + C["VARIATION_POSITION"] * draws[0], 0.0, 1.0)
		self.hit_cents = C["VARIATION_TUNE_CENTS"] * draws[3]
		self.muted = False
		self.retune()

		# Half-sine contact pulse, sampled at half-sample offsets, with unit area at the resonator.
		hardness = clamp(self.hardness + C["VARIATION_HARDNESS"] * draws[1], 0.0, 1.0)
		contact = C["SOFT_CONTACT_SECONDS"] * (C["HARD_CONTACT_SECONDS"] / C["SOFT_CONTACT_SECONDS"]) ** hardness * max(force, C["FORCE_FLOOR"]) ** -0.2
		samples = contact * self.rate
		step = math.pi / samples
		exciter.pulse_samples = math.ceil(samples - 0.5)
		area = sum(math.sin(step * (n + 0.5)) for n in range(exciter.pulse_samples))
		amplitude = self.strike_level * force / (area * self.resonator_input)
		exciter.pulse = amplitude * math.sin(0.5 * step)
		exciter.pulse_previous = -exciter.pulse
		exciter.pulse_step = 2.0 * math.cos(step)

		exciter.noise_step = (base + 5) & 0xFFFFFFFF
		exciter.noise_level = self.noise_level * force * C["NOISE_FORCE"]

		tone_w = 2.0 * math.pi * self.f1 * 2.0 ** ((self.tone_cents + C["VARIATION_TONE_CENTS"] * draws[4]) / 1200.0) / self.rate
		exciter.tone_cos, exciter.tone_sin = math.cos(tone_w), math.sin(tone_w)
		exciter.tone_re, exciter.tone_im = 1.0, 0.0
		exciter.tone_level = self.tone_level * force * C["TONE_FORCE"]
		exciter.remaining = max(exciter.pulse_samples, self.samples_above_floor(exciter.noise_level, self.noise_decay), self.samples_above_floor(exciter.tone_level, self.tone_decay))

	@staticmethod
	def samples_above_floor(level: float, decay: float) -> int:
		floor = C["EXCITER_FLOOR"]
		return math.ceil(math.log(floor / level) / math.log(decay)) if level > floor else 0

	def tick_exciter(self, e: Exciter) -> float:
		e.remaining -= 1
		value = 0.0
		if e.pulse_samples > 0:
			value = e.pulse
			e.pulse, e.pulse_previous = e.pulse_step * e.pulse - e.pulse_previous, e.pulse
			e.pulse_samples -= 1
		value += e.noise_band.tick(self.noise_band, lfo_random(e.noise_step)) * e.noise_level
		e.noise_step = (e.noise_step + 1) & 0xFFFFFFFF
		e.noise_level *= self.noise_decay
		re = e.tone_re
		e.tone_re = e.tone_cos * re - e.tone_sin * e.tone_im
		e.tone_im = e.tone_sin * re + e.tone_cos * e.tone_im
		value += fast_tanh(self.tone_drive * e.tone_im) * self.tone_normal * e.tone_level
		e.tone_level *= self.tone_decay
		return value

	# Process ------------------------------------------------------------

	def update(self) -> bool:
		if self.silent():
			self.clear()
			self.idling = True
			return True
		cents = self.tension_cents * min(self.power * C["TENSION_NORMAL"], C["TENSION_ENERGY_MAX"])
		if abs(cents - self.tension_applied) > C["TENSION_STEP_CENTS"]:
			self.apply_tension(cents)
		return False

	def silent(self) -> bool:
		if any(e.remaining > 0 for e in self.exciters):
			return False
		if self.wire_envelope * C["WIRES_GAIN"] > self.head_silence:
			return False
		if self.model == 0:
			count = len(self.mode_w)
			return float(np.sum(np.abs(self.state[:count]) ** 2)) < self.head_silence * self.head_silence
		return self.quiet > self.read_delay + C["MIN_READ_DELAY"]

	def tick_loop(self, x: float) -> float:
		self.read_delay += (self.read_target - self.read_delay) * self.delay_smoothing
		bridge = self.loop.read_cubic(self.read_delay)
		tap = self.loop.read_cubic(max(C["MIN_READ_DELAY"], self.tap_fraction * (self.read_delay + self.loop_phase_delay)))
		self.loop_lowpass += self.lowpass_coeff * (bridge - self.loop_lowpass)
		value = self.loop_lowpass
		self.loop_highpass += self.highpass_coeff * (value - self.loop_highpass)
		value = value - self.loop_highpass
		value = self.loop_cut.tick(self.cut, value)
		a = self.allpass_coeff
		for state in self.allpasses:
			y = a * value + state[0] - a * state[1]
			state[0], state[1] = value, y
			value = y
		if self.drive > 0.0:
			value = fast_tanh(self.drive * value) * self.inverse_drive
		sample = x * self.loop_input_scale + self.loop_gain * value
		self.loop.write(sample)
		self.quiet = self.quiet + 1 if abs(sample) < self.head_silence else 0
		return 0.5 * (sample - tap)

	def render(self, length: int) -> np.ndarray:
		out = np.zeros(length)
		if self.idling:
			return out
		if self.pitch_changed:
			self.pitch_changed = False
			self.retune()
		count = len(self.mode_w) if self.model == 0 else 0
		for i in range(length):
			self.countdown -= 1
			if self.countdown == 0:
				self.countdown = int(C["UPDATE_INTERVAL"])
				if self.update():
					break
				count = len(self.mode_w) if self.model == 0 else 0
			excitation = 0.0
			for e in self.exciters:
				if e.remaining > 0:
					excitation += self.tick_exciter(e)
			x = excitation * self.resonator_input
			if self.model == 0:
				state = self.mode_z * self.state[:count] + self.mode_gain * x
				self.state[:count] = state
				head = float(np.sum(state.imag))
			else:
				head = self.tick_loop(x)
			if self.tension_cents > 0.0:
				power = head * head
				self.power += (power - self.power) * (self.tension_attack if power > self.power else self.tension_release)
			self.direct_highpass += self.direct_coeff * (excitation - self.direct_highpass)
			output = self.head_level * head + self.direct_level * C["DIRECT_GAIN"] * (excitation - self.direct_highpass)
			if self.wires_level > 0.0:
				self.motion += self.motion_coeff * (head - self.motion)
				drive = abs(self.motion) * self.wire_inverse_knee
				target = drive / (1.0 + drive)
				self.wire_envelope += (target - self.wire_envelope) * (self.wire_attack if target > self.wire_envelope else self.wire_release)
				noise = lfo_random(self.wire_step)
				self.wire_step = (self.wire_step + 1) & 0xFFFFFFFF
				output += self.wires_level * C["WIRES_GAIN"] * (self.wire_band.tick(self.wire_tone, noise) * self.wire_envelope)
			out[i] = output * self.output_gain
		return out


def ints(x: np.ndarray) -> np.ndarray:
	"""The C++ (int) cast: truncation toward zero."""
	return np.trunc(x)


def run(jobs: list[h.Job]) -> list[np.ndarray]:
	"""Runs the same scripts as the renderer; returns float pipe values per job."""
	outputs = []
	talus = Talus(seed=0)
	for job in jobs:
		rendered = []
		for line in job.lines:
			words = line.split()
			command, args = words[0], words[1:]
			if command == "rate":
				rate = float(args[0])
			elif command == "channel":
				assert args[0] == "talus"
				talus.initialize(rate)
			elif command == "params":
				talus.set_params([int(a) for a in args])
			elif command == "pitch":
				talus.pitch = int(args[0])
				talus.pitch_changed = True
			elif command == "expression":
				talus.expression = int(args[0]) * 0.0078125
			elif command == "on":
				talus.note_on()
			elif command == "off":
				talus.note_off()
			elif command == "render":
				remaining = int(args[0])
				for block in [int(b) for b in args[1:]]:
					block = min(block, remaining)
					rendered.append(talus.render(block))
					remaining -= block
				if remaining > 0:
					rendered.append(talus.render(remaining))
			elif command == "until_idle":
				limit, block = int(args[0]), int(args[1])
				done = 0
				while done < limit and not talus.idling:
					rendered.append(talus.render(block))
					done += block
			else:
				raise ValueError(command)
		outputs.append(np.concatenate(rendered) if rendered else np.zeros(0))
	return outputs
