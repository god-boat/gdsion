#include "siopm_channel_talus.h"

#include <cmath>
#include <complex>
#include <godot_cpp/core/class_db.hpp>
#include "chip/siopm_channel_params.h"
#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "dsp/fast_tanh.h"
#include "dsp/lfo.h"
#include "templates/singly_linked_list.h"

using sion::dsp::allpass_response;
using sion::dsp::BIQUAD_BAND_PASS;
using sion::dsp::BIQUAD_PEAK;
using sion::dsp::biquad_response;
using sion::dsp::compute_biquad_coefficients;
using sion::dsp::fast_tanh;
using sion::dsp::lfo_random;
using sion::dsp::MEMBRANE_MODES;
using sion::dsp::MEMBRANE_POSITION_POINTS;
using sion::dsp::one_pole_coeff;
using sion::dsp::one_pole_coeff_for_magnitude;
using sion::dsp::one_pole_magnitude;
using sion::dsp::one_pole_response;

static constexpr double PIPE_PEAK = 8192.0;
static constexpr double LN2_PER_CENT = 0.000577622650466621; // ln(2) / 1200
static constexpr double LN1000 = 6.907755278982137; // A T60 falls ln(1000) nepers.
static constexpr double NEPERS_PER_DB = 0.11512925464970229; // ln(10) / 20

// Exciter levels are forces at this rate. The resonator scales its input by
// REFERENCE_RATE / rate, so a strike rings the same at any sample rate.
static constexpr double REFERENCE_RATE = 48000.0;
// Glides, finished exciters and silence are checked this often, counted from
// the key-on, so a render does not depend on how the host splits it.
static constexpr int UPDATE_INTERVAL = 32;
static constexpr uint32_t HIT_STRIDE = 1u << 20;
static constexpr int VARIATION_DRAWS = 5;

// Strike.
static constexpr double SOFT_CONTACT_SECONDS = 0.006;
static constexpr double HARD_CONTACT_SECONDS = 0.00015;
// A zero-force hit excites nothing; the floor only keeps its contact time finite.
static constexpr double FORCE_FLOOR = 0.01;
static constexpr double POSITION_REACH = 0.95;
static constexpr double NOISE_Q = 0.7;
static constexpr double TONE_DRIVE_SINE = 0.05;
static constexpr double TONE_DRIVE_SQUARE = 3.0;
static constexpr double EXCITER_FLOOR = 1e-6;

// Calibrated with tools/talus_dsp/calibrate.py on the default voice at C3:
// Noise or Tone at 100 % rings the head as hard as the strike at 100 %, Direct at
// 100 % peaks with the head, and saturated Wires at 100 % peak with the head.
static constexpr double NOISE_FORCE = 0.6278;
static constexpr double TONE_FORCE = 0.05923;
static constexpr double DIRECT_GAIN = 141.6;
static constexpr double WIRES_GAIN = 2.729;

// Variation, at 100 %.
static constexpr double VARIATION_POSITION = 0.15;
static constexpr double VARIATION_HARDNESS = 0.2;
static constexpr double VARIATION_FORCE_DB = 1.0;
static constexpr double VARIATION_TUNE_CENTS = 3.0;
static constexpr double VARIATION_TONE_CENTS = 5.0;

// Resonator.
static constexpr double MODE_CEILING_HZ = 20000.0;
static constexpr double MODE_CEILING_FRACTION = 0.45;
static constexpr double CUT_Q = 1.4142135623730951; // One octave wide.
// Stiffness at 100 % puts the eighth partial a semitone sharp:
// R sqrt((1 + B R^2) / (1 + B)) = R 2^(1/12) at the eighth ratio R.
static constexpr double SEMITONE_SQUARED = 1.122462048309373; // 2^(1/6)
static constexpr int STIFFNESS_PARTIAL = 8;
static constexpr double MEMBRANE_STIFFNESS_MAX = (SEMITONE_SQUARED - 1.0) /
		(MEMBRANE_MODES[STIFFNESS_PARTIAL - 1].ratio * MEMBRANE_MODES[STIFFNESS_PARTIAL - 1].ratio - SEMITONE_SQUARED);
static constexpr double LOOP_STIFFNESS_MAX = (SEMITONE_SQUARED - 1.0) / (STIFFNESS_PARTIAL * STIFFNESS_PARTIAL - SEMITONE_SQUARED);

// Loop. Sized once for every sample rate, so nothing allocates after construction.
static constexpr double DELAY_LINE_SAMPLES = 8191.0;
// The read delay is the period minus the filters' phase delay, and a leading
// highpass or cut can make it up to half a period longer.
static constexpr double LOOP_REACH = 1.5;
static constexpr double SHORTEST_PERIOD = 8.0;
static constexpr double MIN_READ_DELAY = 2.0; // read_cubic needs a sample on either side.
static constexpr double LOSS_FIT_RATIO = 4.0;
static constexpr double LOSS_FIT_LIMIT = 0.9 * Math_PI;
static constexpr double ALLPASS_LIMIT = -0.95;
static constexpr int ALLPASS_FIT_STEPS = 30;
static constexpr int POLE_STEPS = 4;
static constexpr double DELAY_SMOOTHING_SECONDS = 0.002;
static constexpr double DRIVE_MAX = 4.0;

// Tension: a full-force default hit's head power peaks near TENSION_NORMAL^-1.
static constexpr double TENSION_NORMAL = 0.4438;
// The glide stops growing at twice a default hit's energy, which also bounds how
// far it can raise a mode toward the ceiling.
static constexpr double TENSION_ENERGY_MAX = 2.0;
static constexpr double TENSION_ATTACK_SECONDS = 0.001;
static constexpr double TENSION_RELEASE_SECONDS = 0.005;
// Never release faster than this many periods, so the power doesn't ripple at 2 f1.
static constexpr double TENSION_RELEASE_PERIODS = 2.0;
static constexpr double TENSION_STEP_CENTS = 0.05;

// Wires. The knee is relative to a full-force default hit's head motion.
static constexpr double WIRE_HEAD_REFERENCE = 2.354;
static constexpr double WIRE_KNEE_RANGE_DB = 36.0;
static constexpr double WIRE_Q = 0.8;
static constexpr double WIRE_ATTACK_SECONDS = 0.0003;

// Output.
static constexpr double DIRECT_HIGHPASS_HZ = 100.0;
// Puts a full-force default hit's peak at Iron's single-note level, -7 dBFS.
static constexpr double OUTPUT_TRIM[] = { 0.1138, 0.06415 };
// The channel idles once nothing it renders can reach this many pipe units.
static constexpr double SILENCE_PIPE_UNITS = 0.25;

// ---------------------------------------------------------------------------
// Exciter
// ---------------------------------------------------------------------------

void SiOPMChannelTalus::_strike(double p_velocity) {
	// Each hit reads its own stretch of the hash: its variation draws, then its noise.
	const uint32_t hit_base = _random_seed + _hit_index * HIT_STRIDE;
	Exciter &exciter = _exciters[_hit_index % EXCITER_COUNT];
	_hit_index++;

	double draws[VARIATION_DRAWS];
	for (int i = 0; i < VARIATION_DRAWS; i++) {
		draws[i] = lfo_random(hit_base + i) * _variation;
	}

	const double force = (1.0 - _velocity_amount * (1.0 - p_velocity)) * std::exp(VARIATION_FORCE_DB * NEPERS_PER_DB * draws[2]);
	_hit_position = POSITION_REACH * CLAMP(_position + VARIATION_POSITION * draws[0], 0.0, 1.0);
	_hit_cents = VARIATION_TUNE_CENTS * draws[3];
	_muted = false;
	_retune();

	// A half-sine force pulse. Harder sticks and harder hits shorten the contact
	// (Hertz: time ~ force^-1/5), and the shorter pulse reaches higher modes.
	const double hardness = CLAMP(_hardness + VARIATION_HARDNESS * draws[1], 0.0, 1.0);
	const double contact = SOFT_CONTACT_SECONDS * std::pow(HARD_CONTACT_SECONDS / SOFT_CONTACT_SECONDS, hardness) * std::pow(MAX(force, FORCE_FLOOR), -0.2);
	const double samples = contact * _sample_rate;
	const double step = Math_PI / samples;
	exciter.pulse_samples = (int)std::ceil(samples - 0.5);
	// The impulse, not the peak, follows the force, so a softer stick is no louder.
	const double half_angle = 0.5 * exciter.pulse_samples * step;
	const double area = std::sin(half_angle) * std::sin(half_angle) / std::sin(0.5 * step);
	const double amplitude = _strike_level * force / (area * _resonator_input);
	exciter.pulse = amplitude * std::sin(0.5 * step);
	exciter.pulse_previous = -exciter.pulse;
	exciter.pulse_step = 2.0 * std::cos(step);

	exciter.noise_step = hit_base + VARIATION_DRAWS;
	exciter.noise_level = _noise_level * force * NOISE_FORCE;

	const double tone_w = Math_TAU * _f1 * std::exp((_tone_cents + VARIATION_TONE_CENTS * draws[4]) * LN2_PER_CENT) / _sample_rate;
	exciter.tone_cos = std::cos(tone_w);
	exciter.tone_sin = std::sin(tone_w);
	exciter.tone_re = 1.0;
	exciter.tone_im = 0.0;
	exciter.tone_level = _tone_level * force * TONE_FORCE;

	// The hit's length is fixed now, so it ends on the same sample however the hits around it fall.
	exciter.remaining = MAX(exciter.pulse_samples, MAX(_samples_above_floor(exciter.noise_level, _noise_decay), _samples_above_floor(exciter.tone_level, _tone_decay)));
}

// Samples until a level that falls by p_decay each sample drops below EXCITER_FLOOR.
int SiOPMChannelTalus::_samples_above_floor(double p_level, double p_decay) {
	return p_level > EXCITER_FLOOR ? (int)std::ceil(std::log(EXCITER_FLOOR / p_level) / std::log(p_decay)) : 0;
}

double SiOPMChannelTalus::_tick_exciter(Exciter &r_exciter) {
	r_exciter.remaining--;
	double value = 0.0;
	if (r_exciter.pulse_samples > 0) {
		value = r_exciter.pulse;
		const double next = r_exciter.pulse_step * r_exciter.pulse - r_exciter.pulse_previous;
		r_exciter.pulse_previous = r_exciter.pulse;
		r_exciter.pulse = next;
		r_exciter.pulse_samples--;
	}

	value += r_exciter.noise_band.tick(_noise_band_coeffs, lfo_random(r_exciter.noise_step++)) * r_exciter.noise_level;
	r_exciter.noise_level *= _noise_decay;

	const double tone_re = r_exciter.tone_re;
	r_exciter.tone_re = r_exciter.tone_cos * tone_re - r_exciter.tone_sin * r_exciter.tone_im;
	r_exciter.tone_im = r_exciter.tone_sin * tone_re + r_exciter.tone_cos * r_exciter.tone_im;
	value += fast_tanh(_tone_drive * r_exciter.tone_im) * _tone_normal * r_exciter.tone_level;
	r_exciter.tone_level *= _tone_decay;
	return value;
}

// ---------------------------------------------------------------------------
// Resonator
// ---------------------------------------------------------------------------

void SiOPMChannelTalus::_retune() {
	const double note = 60.0 + (_current_pitch / 64.0 - 60.0) * _keytrack + _tune + _hit_cents * 0.01;
	_f1 = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
	_wire_motion_coeff = one_pole_coeff(4.0 * _f1, _sample_rate);
	_tension_release = 1.0 - std::exp(-1.0 / (MAX(TENSION_RELEASE_SECONDS, TENSION_RELEASE_PERIODS / _f1) * _sample_rate));
	if (_model == MODEL_MEMBRANE) {
		_retune_membrane();
	} else {
		_retune_loop();
	}
	_apply_tension(_tension_applied);
}

void SiOPMChannelTalus::_retune_membrane() {
	// Leave out modes a full glide could push past the ceiling.
	const double ceiling = MIN(MODE_CEILING_FRACTION * _sample_rate, MODE_CEILING_HZ) * std::exp(-TENSION_ENERGY_MAX * _tension_cents * LN2_PER_CENT);
	const double stiffness = _stiffness * MEMBRANE_STIFFNESS_MAX;
	const double highpass_coeff = one_pole_coeff(_low_cut_hz, _sample_rate);
	const double position = _hit_position * (MEMBRANE_POSITION_POINTS - 1);
	const int position_index = MIN((int)position, MEMBRANE_POSITION_POINTS - 2);
	const double position_fraction = position - position_index;

	int count = 0;
	for (; count < MODE_MAX; count++) {
		const sion::dsp::MembraneMode &mode = MEMBRANE_MODES[count];
		const double ratio = mode.ratio * std::sqrt((1.0 + stiffness * mode.ratio * mode.ratio) / (1.0 + stiffness));
		const double hz = _f1 * ratio;
		if (hz >= ceiling) {
			break;
		}
		const double w = Math_TAU * hz / _sample_rate;
		double t60 = _decay_seconds * std::pow(ratio, -_damping_power);
		if (_muted) {
			t60 = MIN(t60, _release_seconds);
		}
		// Cut and Low Cut take their attenuation at this mode once per period of the fundamental.
		const std::complex<double> circle = std::polar(1.0, -w);
		const double kept = std::abs(1.0 - one_pole_response(highpass_coeff, circle)) * std::abs(biquad_response(_cut_coeffs, circle));
		_modes.w[count] = w;
		_modes.radius[count] = std::exp((std::log(kept) * _f1 - LN1000 / t60) / _sample_rate);
		_modes.gain[count] = mode.weights[position_index] + (mode.weights[position_index + 1] - mode.weights[position_index]) * position_fraction;
	}
	_mode_state.clear(count);
	_modes.count = count;
}

void SiOPMChannelTalus::_retune_loop() {
	_period = CLAMP(_sample_rate / _f1, SHORTEST_PERIOD, (_loop.mask - 4) / LOOP_REACH);
	const double w1 = Math_TAU / _period;
	// A loop loses at most 60 dB per pass, so a period longer than the T60 rings for one pass.
	const double t60 = MAX(_muted ? MIN(_decay_seconds, _release_seconds) : _decay_seconds, _period / _sample_rate);
	// Each period the fundamental keeps `decay`.
	const double decay = std::exp(-LN1000 * _period / (t60 * _sample_rate));

	// Fit the lowpass to the loss curve: what a partial LOSS_FIT_RATIO times higher keeps
	// relative to the fundamental. |H(w_fit)| / |H(w1)| = kept is quadratic in the pole,
	// and the root inside the unit circle is the filter.
	const double w_fit = MIN(LOSS_FIT_RATIO * w1, LOSS_FIT_LIMIT);
	const double kept = std::exp(-LN1000 * (std::pow(w_fit / w1, _damping_power) - 1.0) * _period / (t60 * _sample_rate));
	const double a = kept * kept - 1.0;
	const double b = std::cos(w1) - kept * kept * std::cos(w_fit);
	const double discriminant = b * b - a * a;
	// Past the steepest slope a one-pole has, the DC rule below sets the filter instead.
	const double pole = discriminant > 0.0 ? a / (-b - std::sqrt(discriminant)) : 1.0;
	double lowpass_coeff = 1.0 - pole;
	// The loop's DC mode decays by the gain alone. Holding |H(w1)| at or above sqrt(decay)
	// keeps the gain at or below sqrt(decay), so nothing outlasts twice the T60.
	const double magnitude_floor = std::sqrt(decay);
	if (one_pole_magnitude(lowpass_coeff, w1) < magnitude_floor) {
		lowpass_coeff = one_pole_coeff_for_magnitude(magnitude_floor, w1);
	}
	_loop_lowpass_coeff = lowpass_coeff;
	// The lowpass's loss at the fundamental is compensated. Low Cut, Cut and the allpasses
	// have |H| <= 1 and stay uncompensated: they are loss.
	_loop_gain = decay / one_pole_magnitude(lowpass_coeff, w1);
	_loop_highpass_coeff = one_pole_coeff(_low_cut_hz, _sample_rate);

	// The fundamental decays, so its pole sits inside the unit circle by what a pass keeps.
	// Where a filter's gain slopes, the pole's angle follows the filter's phase there, not
	// on the circle: tune at the pole, or a lossy loop runs sharp. Start from what a pass
	// keeps on the circle, then settle the radius where it keeps exactly the pole's decay.
	const std::complex<double> circle = std::polar(1.0, -w1);
	const double kept_per_pass = decay * std::abs(1.0 - one_pole_response(_loop_highpass_coeff, circle)) * std::abs(biquad_response(_cut_coeffs, circle));
	double radius = std::pow(kept_per_pass, 1.0 / _period);
	_allpass_coeff = _fit_loop_allpass(w1, radius);
	for (int i = 0; i < POLE_STEPS; i++) {
		const double read_delay = _period - _loop_filter_delay(w1, radius) - 2.0 * _allpass_delay(_allpass_coeff, w1, radius);
		radius = std::pow(_loop_kept(w1, radius), 1.0 / read_delay);
	}
	_loop_phase_delay = _loop_filter_delay(w1, radius) + 2.0 * _allpass_delay(_allpass_coeff, w1, radius);

	// A strike's impulse puts 2 / period into the fundamental of the loop, so scale it to
	// ring the fundamental as hard as the membrane's.
	_loop_input_scale = 0.5 * _period;
	// A strike a fraction d of a period from the end of the loop cancels the partials
	// whose multiple of d is whole; at the center, d = 1/2 leaves the odd ones.
	_tap_fraction = 0.5 * (1.0 - _hit_position);
}

// The phase delay of the lowpass, Low Cut and Cut at angle p_w, radius p_radius.
double SiOPMChannelTalus::_loop_filter_delay(double p_w, double p_radius) const {
	const std::complex<double> z_inverse = std::polar(1.0 / p_radius, -p_w);
	const std::complex<double> lowpass = one_pole_response(_loop_lowpass_coeff, z_inverse);
	const std::complex<double> highpass = 1.0 - one_pole_response(_loop_highpass_coeff, z_inverse);
	return -std::arg(lowpass * highpass * biquad_response(_cut_coeffs, z_inverse)) / p_w;
}

double SiOPMChannelTalus::_allpass_delay(double p_coeff, double p_w, double p_radius) {
	return -std::arg(allpass_response(p_coeff, std::polar(1.0 / p_radius, -p_w))) / p_w;
}

// What one pass of the loop keeps at angle p_w, radius p_radius, delay aside.
double SiOPMChannelTalus::_loop_kept(double p_w, double p_radius) const {
	const std::complex<double> z_inverse = std::polar(1.0 / p_radius, -p_w);
	const std::complex<double> allpass = allpass_response(_allpass_coeff, z_inverse);
	return _loop_gain * std::abs(one_pole_response(_loop_lowpass_coeff, z_inverse) * (1.0 - one_pole_response(_loop_highpass_coeff, z_inverse)) * biquad_response(_cut_coeffs, z_inverse) * allpass * allpass);
}

// Stiffness: two allpasses whose phase delay falls with frequency, so the upper
// partials run sharp. Bisect for the coefficient that puts the eighth partial
// (or the highest one below the ceiling) where the stiffness formula does.
double SiOPMChannelTalus::_fit_loop_allpass(double p_w1, double p_radius) const {
	if (_stiffness == 0.0) {
		return 0.0;
	}
	const double partial = MIN((double)STIFFNESS_PARTIAL, std::floor(MODE_CEILING_FRACTION * _period));
	const double stiffness = _stiffness * LOOP_STIFFNESS_MAX;
	const double w_target = p_w1 * partial * std::sqrt((1.0 + stiffness * partial * partial) / (1.0 + stiffness));
	const double filter_delay = _loop_filter_delay(p_w1, p_radius);
	const double fixed_delay = _period - filter_delay + _loop_filter_delay(w_target, p_radius);

	// The loop's phase at w_target, less the partial's whole turns, falls as the coefficient
	// goes negative. Past the limit, or where the read delay runs out, it stops short.
	double sharp = ALLPASS_LIMIT;
	double flat = 0.0;
	for (int i = 0; i < ALLPASS_FIT_STEPS; i++) {
		const double coeff = 0.5 * (sharp + flat);
		const double delay_at_w1 = 2.0 * _allpass_delay(coeff, p_w1, p_radius);
		const double phase = w_target * (fixed_delay - delay_at_w1 + 2.0 * _allpass_delay(coeff, w_target, p_radius)) - Math_TAU * partial;
		if (phase < 0.0 || _period - filter_delay - delay_at_w1 < MIN_READ_DELAY) {
			sharp = coeff;
		} else {
			flat = coeff;
		}
	}
	return flat;
}

void SiOPMChannelTalus::_apply_tension(double p_cents) {
	_tension_applied = p_cents;
	const double scale = std::exp(p_cents * LN2_PER_CENT);
	if (_model == MODEL_MEMBRANE) {
		_modes.set_pitch_scale(scale);
	} else {
		_read_delay_target = MAX(MIN_READ_DELAY, _period / scale - _loop_phase_delay);
	}
}

template <int MODEL, bool DRIVEN>
double SiOPMChannelTalus::_tick_resonator(double p_input, bool p_excited) {
	if constexpr (MODEL == MODEL_MEMBRANE) {
		return p_excited ? _mode_state.tick(_modes, p_input) : _mode_state.tick_free(_modes);
	} else {
		_read_delay += (_read_delay_target - _read_delay) * _delay_smoothing;
		const double bridge = _loop.read_cubic(_read_delay);
		const double tap = _loop.read_cubic(MAX(MIN_READ_DELAY, _tap_fraction * (_read_delay + _loop_phase_delay)));

		double value = _loop_lowpass.lowpass(_loop_lowpass_coeff, bridge);
		value = _loop_highpass.highpass(_loop_highpass_coeff, value);
		value = _loop_cut.tick(_cut_coeffs, value);
		for (sion::dsp::Allpass &allpass : _loop_allpasses) {
			value = allpass.tick(_allpass_coeff, value);
		}
		if constexpr (DRIVEN) {
			// |tanh(kx) / k| <= |x|: loud passes compress and crack, and the loop stays passive.
			value = fast_tanh(_drive * value) * _inverse_drive;
		}
		const double sample = p_input * _loop_input_scale + _loop_gain * value;
		_loop.write(sample);
		_loop_quiet_samples = std::abs(sample) < _head_silence ? _loop_quiet_samples + 1 : 0;
		// The head hears the loop less itself a strike-position fraction of a period ago.
		return 0.5 * (sample - tap);
	}
}

// ---------------------------------------------------------------------------
// Params
// ---------------------------------------------------------------------------

void SiOPMChannelTalus::set_talus_params(const TalusParams &p_params) {
	_params = p_params;
	const int *values = p_params.values;

	_strike_level = CLAMP(values[TalusParams::STRIKE_LEVEL], 0, 100) * 0.01;
	_hardness = CLAMP(values[TalusParams::STRIKE_HARDNESS], 0, 100) * 0.01;
	_velocity_amount = CLAMP(values[TalusParams::STRIKE_VELOCITY], 0, 100) * 0.01;
	_noise_level = CLAMP(values[TalusParams::NOISE_LEVEL], 0, 100) * 0.01;
	_noise_decay = std::exp(-LN1000 / (CLAMP(values[TalusParams::NOISE_DECAY_MS], 1, 200) * 0.001 * _sample_rate));
	_noise_band_coeffs = compute_biquad_coefficients(BIQUAD_BAND_PASS, CLAMP(values[TalusParams::NOISE_COLOR_HZ], 200, 16000), NOISE_Q, 0.0, _sample_rate);
	_tone_level = CLAMP(values[TalusParams::TONE_LEVEL], 0, 100) * 0.01;
	_tone_cents = CLAMP(values[TalusParams::TONE_PITCH_CENTS], -2400, 2400);
	_tone_drive = TONE_DRIVE_SINE + (TONE_DRIVE_SQUARE - TONE_DRIVE_SINE) * CLAMP(values[TalusParams::TONE_SHAPE], 0, 100) * 0.01;
	_tone_normal = 1.0 / fast_tanh(_tone_drive);
	_tone_decay = std::exp(-LN1000 / (CLAMP(values[TalusParams::TONE_DECAY_MS], 1, 200) * 0.001 * _sample_rate));
	_variation = CLAMP(values[TalusParams::VARIATION], 0, 100) * 0.01;

	const int model = CLAMP(values[TalusParams::MODEL], 0, MODEL_COUNT - 1);
	_tune = CLAMP(values[TalusParams::TUNE_SEMITONES], -48, 48) + CLAMP(values[TalusParams::FINE_CENTS], -100, 100) * 0.01;
	_keytrack = CLAMP(values[TalusParams::KEYTRACK], 0, 100) * 0.01;
	_decay_seconds = CLAMP(values[TalusParams::DECAY_MS], 20, 10000) * 0.001;
	_damping_power = 3.0 * CLAMP(values[TalusParams::DAMPING], 0, 100) * 0.01;
	_position = CLAMP(values[TalusParams::POSITION], 0, 100) * 0.01;
	_stiffness = CLAMP(values[TalusParams::STIFFNESS], 0, 100) * 0.01;
	_low_cut_hz = CLAMP(values[TalusParams::LOW_CUT_HZ], 1, 2000);
	_cut_coeffs = compute_biquad_coefficients(BIQUAD_PEAK, CLAMP(values[TalusParams::CUT_HZ], 100, 12000), CUT_Q, CLAMP(values[TalusParams::CUT_DB], -24, 0), _sample_rate);
	_tension_cents = CLAMP(values[TalusParams::TENSION_CENTS], 0, 1200);
	_drive = DRIVE_MAX * CLAMP(values[TalusParams::DRIVE], 0, 100) * 0.01;
	_inverse_drive = _drive > 0.0 ? 1.0 / _drive : 1.0;

	_wires_level = CLAMP(values[TalusParams::WIRES_LEVEL], 0, 100) * 0.01;
	// Loose wires (0 %) rattle WIRE_KNEE_RANGE_DB below a full-force hit; tight ones need the full hit.
	const double wire_tension = CLAMP(values[TalusParams::WIRES_TENSION], 0, 100) * 0.01;
	_wire_inverse_knee = 1.0 / (WIRE_HEAD_REFERENCE * std::exp((wire_tension - 1.0) * WIRE_KNEE_RANGE_DB * NEPERS_PER_DB));
	_wire_band_coeffs = compute_biquad_coefficients(BIQUAD_BAND_PASS, CLAMP(values[TalusParams::WIRES_TONE_HZ], 1000, 12000), WIRE_Q, 0.0, _sample_rate);
	_wire_release = 1.0 - std::exp(-LN1000 / (CLAMP(values[TalusParams::WIRES_DECAY_MS], 5, 800) * 0.001 * _sample_rate));

	_head_level = CLAMP(values[TalusParams::HEAD], 0, 100) * 0.01;
	_direct_level = CLAMP(values[TalusParams::DIRECT], 0, 100) * 0.01;
	_release_mode = CLAMP(values[TalusParams::RELEASE_MODE], 0, 1);
	_release_seconds = CLAMP(values[TalusParams::RELEASE_MS], 5, 2000) * 0.001;

	_output_gain = OUTPUT_TRIM[model] * PIPE_PEAK;
	_head_silence = SILENCE_PIPE_UNITS / _output_gain;
	if (model != _model) {
		// The other resonator starts silent.
		_model = model;
		_clear();
	}
	_retune();
}

void SiOPMChannelTalus::set_pitch(int p_value) {
	_current_pitch = p_value;
	_pitch_changed = true;
}

void SiOPMChannelTalus::get_channel_params(const Ref<SiOPMChannelParams> &p_params) const {
	p_params->set_operator_count(1);
	for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
		p_params->set_master_volume(i, _volumes[i]);
	}
	p_params->set_instrument_gain_db(get_instrument_gain_db());
	p_params->set_pan(_pan);
}

void SiOPMChannelTalus::set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation) {
	if (p_with_volume) {
		for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			_volumes.write[i] = p_params->get_master_volume(i);
		}

		_has_effect_send = false;
		for (int i = 1; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			if (_volumes[i] > 0) {
				_has_effect_send = true;
				break;
			}
		}

		_pan = p_params->get_pan();
	}
	set_instrument_gain_db(p_params->get_instrument_gain_db());

	_filter_type = p_params->get_filter_type();
	set_sv_filter(
			p_params->get_filter_cutoff(),
			p_params->get_filter_resonance(),
			p_params->get_filter_attack_rate(),
			p_params->get_filter_decay_rate1(),
			p_params->get_filter_decay_rate2(),
			p_params->get_filter_release_rate(),
			p_params->get_filter_decay_offset1(),
			p_params->get_filter_decay_offset2(),
			p_params->get_filter_sustain_offset(),
			p_params->get_filter_release_offset());
}

// ---------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------

void SiOPMChannelTalus::offset_volume(int p_expression, int p_velocity) {
	_expression = (double)p_expression * 0.0078125;
}

void SiOPMChannelTalus::note_on() {
	// A hit adds to whatever rings: a new note retunes in place and nothing is cleared.
	// The key velocity arrives as the expression set just before the key-on.
	const bool waking = _is_source_idling;
	_pitch_changed = false;
	_strike(CLAMP(_expression, 0.0, 1.0));
	if (waking) {
		_read_delay = _read_delay_target;
		_update_countdown = 1;
	}
	_is_source_idling = false;
	SiOPMChannelBase::note_on();
}

void SiOPMChannelTalus::note_off() {
	// Mute is a hand on the head: every T60 shortens to the release time.
	if (_release_mode == RELEASE_MUTE) {
		_muted = true;
		_retune();
	}
	SiOPMChannelBase::note_off();
}

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

// Every UPDATE_INTERVAL samples: idle once silent, and follow tension.
bool SiOPMChannelTalus::_update() {
	if (_is_silent()) {
		_clear();
		_is_source_idling = true;
		return true;
	}

	// Head tension rises with the square of displacement, so a loud hit starts sharp.
	const double cents = _tension_cents * MIN(_tension_power * TENSION_NORMAL, TENSION_ENERGY_MAX);
	if (std::abs(cents - _tension_applied) > TENSION_STEP_CENTS) {
		_apply_tension(cents);
	}
	return false;
}

bool SiOPMChannelTalus::_is_silent() const {
	for (const Exciter &exciter : _exciters) {
		if (exciter.remaining > 0) {
			return false;
		}
	}
	if (_wire_envelope * WIRES_GAIN > _head_silence) {
		return false;
	}
	if (_model == MODEL_MEMBRANE) {
		return _mode_state.energy(_modes.count) < _head_silence * _head_silence;
	}
	// The loop has been quiet for longer than it takes to go around.
	return _loop_quiet_samples > _read_delay + MIN_READ_DELAY;
}

void SiOPMChannelTalus::_clear() {
	for (Exciter &exciter : _exciters) {
		exciter = {};
	}
	_mode_state.clear();
	_loop.clear();
	_loop_lowpass = {};
	_loop_highpass = {};
	_loop_cut = {};
	for (sion::dsp::Allpass &allpass : _loop_allpasses) {
		allpass = {};
	}
	_loop_quiet_samples = 0;
	_tension_power = 0.0;
	// The next retune applies it.
	_tension_applied = 0.0;
	_wire_motion = {};
	_wire_envelope = 0.0;
	_wire_band = {};
	_direct_highpass = {};
}

template <int MODEL, bool DRIVEN>
void SiOPMChannelTalus::_render(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe = _out_pipe->get();

	bool silent = false;
	for (int i = 0; i < p_length; i++) {
		if (!silent && --_update_countdown == 0) {
			_update_countdown = UPDATE_INTERVAL;
			silent = _update();
		}
		if (silent) {
			out_pipe->value = base_pipe->value;
		} else {
			double excitation = 0.0;
			bool excited = false;
			for (Exciter &exciter : _exciters) {
				if (exciter.remaining > 0) {
					excitation += _tick_exciter(exciter);
					excited = true;
				}
			}

			const double head = _tick_resonator<MODEL, DRIVEN>(excitation * _resonator_input, excited);
			const double power = head * head;
			_tension_power += (power - _tension_power) * (power > _tension_power ? _tension_attack : _tension_release);

			double output = _head_level * head + _direct_level * DIRECT_GAIN * _direct_highpass.highpass(_direct_coeff, excitation);
			if (_wires_level > 0.0) {
				// The wires follow the head's motion and saturate, so a ghost note keeps much of
				// its rattle. They buzz at the drum's pitch and ring as long as the head.
				const double motion = _wire_motion.lowpass(_wire_motion_coeff, head);
				const double drive = std::abs(motion) * _wire_inverse_knee;
				const double target = drive / (1.0 + drive);
				_wire_envelope += (target - _wire_envelope) * (target > _wire_envelope ? _wire_attack : _wire_release);
				const double wires = _wire_band.tick(_wire_band_coeffs, lfo_random(_wire_step++)) * _wire_envelope;
				output += _wires_level * WIRES_GAIN * wires;
			}
			out_pipe->value = (int)(output * _output_gain) + base_pipe->value;
		}

		in_pipe = in_pipe->next();
		base_pipe = base_pipe->next();
		out_pipe = out_pipe->next();
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

void SiOPMChannelTalus::_process_talus(int p_length) {
	if (_pitch_changed) {
		// Bends, glides and legato slurs retune the ringing head.
		_pitch_changed = false;
		_retune();
	}
	if (_model == MODEL_MEMBRANE) {
		_render<MODEL_MEMBRANE, false>(p_length);
	} else if (_drive > 0.0) {
		_render<MODEL_LOOP, true>(p_length);
	} else {
		_render<MODEL_LOOP, false>(p_length);
	}
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SiOPMChannelTalus::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	SiOPMChannelBase::initialize(p_prev, p_buffer_index);

	_sample_rate = _table->sampling_rate;
	_resonator_input = REFERENCE_RATE / _sample_rate;
	_direct_coeff = one_pole_coeff(DIRECT_HIGHPASS_HZ, _sample_rate);
	_wire_attack = 1.0 - std::exp(-1.0 / (WIRE_ATTACK_SECONDS * _sample_rate));
	_tension_attack = 1.0 - std::exp(-1.0 / (TENSION_ATTACK_SECONDS * _sample_rate));
	_delay_smoothing = 1.0 - std::exp(-1.0 / (DELAY_SMOOTHING_SECONDS * _sample_rate));

	_current_pitch = 60 * 64;
	_pitch_changed = false;
	_expression = 1.0;

	reset();
	set_talus_params(TalusParams());

	_process_function = Callable(this, "_process_talus");
}

void SiOPMChannelTalus::reset() {
	_clear();
	_hit_index = 0;
	_hit_position = POSITION_REACH * _position;
	_hit_cents = 0.0;
	_muted = false;
	_update_countdown = 1;
	_wire_step = _random_seed ^ 0x80000000u;

	SiOPMChannelBase::reset();
}

String SiOPMChannelTalus::_to_string() const {
	String params;
	params += "model=" + itos(_model) + ", ";
	params += "tune=" + itos(_params.values[TalusParams::TUNE_SEMITONES]) + ", ";
	params += "decay=" + itos(_params.values[TalusParams::DECAY_MS]) + ", ";
	params += "vol=" + rtos(_volumes[0]) + ", ";
	params += "pan=" + itos(_pan - 64);
	return "SiOPMChannelTalus: " + params;
}

void SiOPMChannelTalus::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_process_talus", "length"), &SiOPMChannelTalus::_process_talus);
}

SiOPMChannelTalus::SiOPMChannelTalus(SiOPMSoundChip *p_chip) :
		SiOPMChannelBase(p_chip) {
	// Each channel walks its own stretch of the hash, so pooled voices never hit in lockstep.
	static uint32_t next_random_seed = 0;
	_random_seed = next_random_seed;
	next_random_seed += 0x9E3779B9u;

	_loop.prepare(DELAY_LINE_SAMPLES);
	_process_function = Callable(this, "_process_talus");
}
