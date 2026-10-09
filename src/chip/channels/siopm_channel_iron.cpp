#include "siopm_channel_iron.h"

#include <cmath>
#include <godot_cpp/core/class_db.hpp>
#include "chip/siopm_channel_params.h"
#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "dsp/fast_tanh.h"
#include "dsp/lfo.h"
#include "templates/singly_linked_list.h"

using sion::dsp::allpass_phase_delay;
using sion::dsp::BIQUAD_HIGH_PASS;
using sion::dsp::BIQUAD_HIGH_SHELF;
using sion::dsp::BIQUAD_LOW_PASS;
using sion::dsp::BIQUAD_LOW_SHELF;
using sion::dsp::BIQUAD_PEAK;
using sion::dsp::compute_biquad_coefficients;
using sion::dsp::fast_tanh;
using sion::dsp::HALFBAND_STEEP;
using sion::dsp::one_pole_coeff;
using sion::dsp::one_pole_coeff_for_magnitude;
using sion::dsp::one_pole_magnitude;
using sion::dsp::one_pole_phase_delay;

static constexpr double PIPE_PEAK = 8192.0;
static constexpr double LN2_PER_CENT = 0.000577622650466621; // ln(2) / 1200

// Sized once for every sample rate, so nothing allocates after construction.
static constexpr double DELAY_LINE_SAMPLES = 8191.0;
// Both pickup taps read within this many periods of the write head.
static constexpr double PICKUP_REACH = 1.3;
static constexpr double SHORTEST_PERIOD = 8.0;

static constexpr double SILENCE_LEVEL = 1e-5;
static constexpr double RETRIGGER_FADE_SECONDS = 0.003;
static constexpr double ATTACK_FADE_SECONDS = 0.0005;
static constexpr double DELAY_SMOOTHING_SECONDS = 0.002;
static constexpr double LOOP_SMOOTHING_SECONDS = 0.003;
static constexpr double BLOOM_SECONDS = 0.06;
static constexpr double STRUM_SECONDS = 0.0008;

static constexpr double PICK_SCRAPE = 0.12;
static constexpr double STRING_REFERENCE_HZ = 82.4068892282175; // Open low E (MIDI 40).
static constexpr double PICK_SOFT_HZ = 900.0;
static constexpr double PICK_HARD_RATIO = 14.0;
static constexpr double CLICK_SECONDS = 0.0007;
static constexpr double CLICK_HIGHPASS_HZ = 2500.0;
static constexpr double CLICK_FLOOR = 1e-4;

static constexpr double MUTED_LOOP_CUTOFF_HZ = 380.0;
static constexpr double MUTED_T60_SECONDS = 0.16;
static constexpr double RELEASED_LOOP_CUTOFF_HZ = 1800.0;
static constexpr double PALM_MUTE_SHARP_CENTS = 5.0;
static constexpr double HUMANIZE_DETUNE_CENTS = 5.0;

static constexpr double PICKUP_RESONANCE_HZ = 5500.0;
static constexpr double PICKUP_RESONANCE_Q = 1.2;
static constexpr double MAX_GAIN_DB = 66.0;
static constexpr double STAGE_GAIN_SHARE[4] = { 0.28, 0.26, 0.24, 0.22 };
// Alternating bias makes each stage clip asymmetrically, adding even harmonics.
static constexpr double STAGE_BIAS[4] = { 0.22, -0.15, 0.08, -0.05 };
static constexpr double STAGE_INPUT_LOWPASS_HZ[4] = { 12000.0, 8000.0, 8000.0, 8000.0 };
static constexpr double STAGE_COUPLING_HZ[4] = { 30.0, 60.0, 40.0, 50.0 };
static constexpr double STAGE_OUTPUT_LOWPASS_HZ[4] = { 9000.0, 8000.0, 8000.0, 8000.0 };
static constexpr double DC_BLOCK_HZ = 8.0;

struct VoicingString {
	int semitones;
	double level;
};

struct Voicing {
	int count;
	VoicingString strings[3];
};

static constexpr Voicing VOICINGS[] = {
	{ 1, { { 0, 1.0 } } },
	{ 2, { { 0, 1.0 }, { 7, 0.85 } } },
	{ 3, { { 0, 1.0 }, { 7, 0.85 }, { 12, 0.7 } } },
	{ 2, { { 0, 1.0 }, { 12, 0.75 } } },
};
static constexpr int VOICING_COUNT = sizeof(VOICINGS) / sizeof(VOICINGS[0]);

struct Cabinet {
	double highpass_hz;
	double thump_hz;
	double thump_db;
	double presence_hz;
	double presence_db;
	double lowpass_hz;
};

static constexpr Cabinet CABINETS[] = {
	{ 75.0, 110.0, 4.0, 3000.0, 6.0, 8000.0 }, // Modern closed-back 4x12.
	{ 85.0, 120.0, 3.0, 1800.0, 4.0, 6500.0 }, // Vintage 4x12.
	{ 100.0, 140.0, 1.0, 2500.0, 3.0, 9000.0 }, // Open-back 2x12.
};
static constexpr int CABINET_DIRECT = sizeof(CABINETS) / sizeof(CABINETS[0]);

// The derivative of the mirrored pluck displacement, in velocity-wave units.
// Normalize its slope jump to one so pick strength sets the velocity level;
// moving the pick changes the spectrum without overdriving the clean amp.
static double pick_velocity(double p_phase, double p_position) {
	const double x = p_phase < 0.5 ? 2.0 * p_phase : 2.0 - 2.0 * p_phase;
	return x < p_position ? 1.0 - p_position : -p_position;
}

// ---------------------------------------------------------------------------
// Randomness
// ---------------------------------------------------------------------------

double SiOPMChannelIron::_random() {
	return sion::dsp::lfo_random(_random_step++);
}

double SiOPMChannelIron::_random_unit() {
	return 0.5 + 0.5 * _random();
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

void SiOPMChannelIron::_tune_string(GuitarString &r_string) {
	const double note = r_string.note + r_string.semitones + r_string.cents * 0.01;
	const double longest_period = (r_string.delay.mask - 4) / PICKUP_REACH;
	r_string.period = CLAMP(_sample_rate / (440.0 * std::pow(2.0, (note - 69.0) / 12.0)), SHORTEST_PERIOD, longest_period);
	const double w0 = Math_TAU / r_string.period;

	// Thinner strings, higher up, are brighter and die sooner.
	const double high = CLAMP((note - 40.0) / 48.0, 0.0, 1.0);
	const double mute = std::pow(CLAMP(_palm_mute + r_string.mute_offset, 0.0, 1.0), 0.7);
	const double open_cutoff = 4200.0 + 2500.0 * high;
	const double open_t60 = _sustain_seconds * (1.0 - 0.5 * high);
	// Muting blends both toward a palm-muted string on a log scale.
	double cutoff = open_cutoff * std::pow(MUTED_LOOP_CUTOFF_HZ / open_cutoff, mute);
	double t60 = open_t60 * std::pow(MUTED_T60_SECONDS / open_t60, mute);
	if (r_string.released) {
		cutoff = MIN(cutoff, RELEASED_LOOP_CUTOFF_HZ);
		t60 = MIN(t60, _release_seconds);
	}

	// Each period the fundamental may lose `decay`. The lowpass takes |H(w0)| of it and the broadband
	// gain the rest, but the loop's DC mode decays by that gain alone. Holding |H(w0)| at or above
	// sqrt(decay) lets DC die within twice the T60, so a high note's loop never parks a DC residue
	// that keeps the channel from idling. Where the cutoff is darker than that, brighten it.
	const double decay = std::pow(0.001, r_string.period / (t60 * _sample_rate));
	// Loss is distributed along a string: a shorter round trip must lose less.
	// Match the low-E loop's loss per second at corresponding partials rather
	// than reapplying a fixed-Hz filter more times per second as pitch rises.
	const double reference_period = _sample_rate / STRING_REFERENCE_HZ;
	const double reference_magnitude = one_pole_magnitude(one_pole_coeff(cutoff, _sample_rate), Math_TAU / reference_period);
	const double lowpass_magnitude = MAX(std::pow(reference_magnitude, r_string.period / reference_period), std::sqrt(decay));
	const double lowpass_coeff = one_pole_coeff_for_magnitude(lowpass_magnitude, w0);
	r_string.loop_lowpass_coeff_target = lowpass_coeff;
	r_string.loop_gain_target = decay / one_pole_magnitude(lowpass_coeff, w0);

	// Stiffness: two allpasses whose phase delay falls across the first partials, so the upper ones run sharp.
	double allpass_coeff = 0.0;
	if (_stiffness > 0.0) {
		allpass_coeff = -(1.0 - MIN(0.95, 12.0 * std::pow(6.0, 1.0 - _stiffness) * w0));
	}
	r_string.allpass_coeff = allpass_coeff;

	r_string.read_delay_target = r_string.period - one_pole_phase_delay(lowpass_coeff, w0) - 2.0 * allpass_phase_delay(allpass_coeff, w0);
	r_string.pickup_delay = _pickup_position * r_string.period;
}

void SiOPMChannelIron::_pluck(GuitarString &r_string) {
	const Pick &pick = r_string.pending;
	r_string.pending_samples = -1;
	r_string.note = _current_pitch / 64.0;
	r_string.semitones = pick.semitones;
	r_string.cents = pick.cents;
	r_string.mute_offset = pick.mute_offset;
	r_string.released = !_is_note_on;
	_tune_string(r_string);
	r_string.read_delay = r_string.read_delay_target;
	r_string.loop_lowpass_coeff = r_string.loop_lowpass_coeff_target;
	r_string.loop_gain = r_string.loop_gain_target;
	r_string.loop_lowpass = {};
	for (sion::dsp::Allpass &allpass : r_string.allpasses) {
		allpass = {};
	}
	r_string.bloom_cents = pick.bloom_cents;

	// A magnetic pickup senses velocity, so seed the waveguide with the derivative
	// of the mirrored triangular displacement. Differentiation commutes with the
	// linear string loop; the scrape stays in velocity units as well.
	sion::dsp::FractionalDelay &delay = r_string.delay;
	delay.clear();
	double *buffer = delay.buffer.data();
	const int mask = delay.mask;
	const int length = (int)(r_string.read_delay + r_string.pickup_delay) + 4;
	const int start = delay.write_index - length;
	const double scrape = PICK_SCRAPE * pick.hardness;
	for (int i = 0; i < length; i++) {
		double phase = (i - length + r_string.read_delay) / r_string.period;
		phase -= std::floor(phase);
		const double shape = pick_velocity(phase, pick.position);
		buffer[(start + i) & mask] = shape + scrape * _random();
	}

	// A softer pick rounds the corners. Warm the filter on the original excitation
	// before writing its output; filtering the first pass again doubles the damping.
	const double rounding_coeff = one_pole_coeff(PICK_SOFT_HZ * std::pow(PICK_HARD_RATIO, pick.hardness), _sample_rate);
	sion::dsp::OnePole rounding;
	for (int i = 0; i < length; i++) {
		rounding.lowpass(rounding_coeff, buffer[(start + i) & mask]);
	}
	double sum = 0.0;
	for (int i = 0; i < length; i++) {
		double &sample = buffer[(start + i) & mask];
		sample = rounding.lowpass(rounding_coeff, sample);
		sum += sample;
	}
	const double mean = sum / length;
	const double amplitude = 0.5 * (0.35 + 0.65 * pick.velocity) * pick.level;
	for (int i = 0; i < length; i++) {
		double &sample = buffer[(start + i) & mask];
		sample = (sample - mean) * amplitude;
	}

	r_string.click_level = 1.0;
	r_string.click_amplitude = 0.12 * pick.hardness * pick.velocity * amplitude;
	r_string.click_lowpass = {};
	r_string.fade = 0.0;
	r_string.fade_step = 1.0 / _attack_fade_samples;
	r_string.quiet_samples = 0;
	r_string.active = true;
}

double SiOPMChannelIron::_tick_string(GuitarString &r_string) {
	r_string.read_delay += (r_string.read_delay_target - r_string.read_delay) * _delay_smoothing;
	r_string.loop_lowpass_coeff += (r_string.loop_lowpass_coeff_target - r_string.loop_lowpass_coeff) * _loop_smoothing;
	r_string.loop_gain += (r_string.loop_gain_target - r_string.loop_gain) * _loop_smoothing;
	r_string.bloom_cents *= _bloom_decay;

	// A hard pick stretches the string sharp; the bloom shortens the period by 2^(-cents/1200)
	// to first order, within 0.2 cents at full bloom.
	const double read_delay = r_string.read_delay * (1.0 - r_string.bloom_cents * LN2_PER_CENT);
	const double bridge = r_string.delay.read_cubic(read_delay);
	// The pickup hears the wave minus its reflection: a comb notching the harmonics its position nodes.
	double pickup = bridge - r_string.delay.read_cubic(read_delay + r_string.pickup_delay);

	double loop = r_string.loop_lowpass.lowpass(r_string.loop_lowpass_coeff, bridge);
	for (sion::dsp::Allpass &allpass : r_string.allpasses) {
		loop = allpass.tick(r_string.allpass_coeff, loop);
	}
	r_string.delay.write(loop * r_string.loop_gain);

	if (r_string.click_level > CLICK_FLOOR) {
		const double noise = _random() * r_string.click_level * r_string.click_amplitude;
		r_string.click_level *= _click_decay;
		pickup += r_string.click_lowpass.highpass(_click_lowpass_coeff, noise);
	}

	if (r_string.fade_step != 0.0) {
		r_string.fade += r_string.fade_step;
		if (r_string.fade >= 1.0) {
			r_string.fade = 1.0;
			r_string.fade_step = 0.0;
		} else if (r_string.fade <= 0.0) {
			r_string.fade = 0.0;
			r_string.fade_step = 0.0;
			if (r_string.pending_samples < 0) {
				r_string.active = false;
			}
		}
	}

	// A string quiet for a whole period has rung out; fade the last of it rather than cut it.
	if (std::abs(bridge) < SILENCE_LEVEL) {
		r_string.quiet_samples++;
		if (r_string.quiet_samples > r_string.period && r_string.fade_step == 0.0 && r_string.pending_samples < 0) {
			r_string.fade_step = -1.0 / _retrigger_fade_samples;
		}
	} else {
		r_string.quiet_samples = 0;
	}

	return pickup * r_string.fade;
}

// ---------------------------------------------------------------------------
// Amp
// ---------------------------------------------------------------------------

double SiOPMChannelIron::_tick_stages(double p_input) {
	double value = p_input;
	for (int k = 0; k < AMP_STAGE_COUNT; k++) {
		AmpStage &stage = _stages[k];
		value = stage.input_lowpass.lowpass(_stage_input_coeff[k], value);
		value = fast_tanh(value * _stage_gain[k] + STAGE_BIAS[k]) - _stage_bias_offset[k];
		value = stage.coupling.highpass(_stage_coupling_coeff[k], value);
		value = stage.output_lowpass.lowpass(_stage_output_coeff[k], value);
	}
	return value;
}

double SiOPMChannelIron::_tick_amp(double p_input) {
	double value = _pickup_state.tick(_pickup_coeffs, p_input);
	value = _tight_state.tick(_tight_coeffs, value);
	value = _hump_state.tick(_hump_coeffs, value);

	double up0, up1;
	_upsampler.upsample(HALFBAND_STEEP, value, up0, up1);
	// The stages carry state, so the two oversampled samples must run in order.
	const double stage0 = _tick_stages(up0);
	const double stage1 = _tick_stages(up1);
	value = _downsampler.downsample(HALFBAND_STEEP, stage0, stage1);
	value = _dc.highpass(_dc_coeff, value);

	for (int i = 0; i < TONE_BIQUAD_COUNT; i++) {
		value = _tone_states[i].tick(_tone_coeffs[i], value);
	}
	for (int i = 0; i < _cabinet_biquad_count; i++) {
		value = _cabinet_states[i].tick(_cabinet_coeffs[i], value);
	}
	return value;
}

void SiOPMChannelIron::_clear_amp() {
	_pickup_state = {};
	_tight_state = {};
	_hump_state = {};
	for (AmpStage &stage : _stages) {
		stage = {};
	}
	_upsampler = {};
	_downsampler = {};
	_dc = {};
	for (sion::dsp::BiquadState &state : _tone_states) {
		state = {};
	}
	for (sion::dsp::BiquadState &state : _cabinet_states) {
		state = {};
	}
}

// ---------------------------------------------------------------------------
// Params
// ---------------------------------------------------------------------------

void SiOPMChannelIron::set_iron_params(const IronParams &p_params) {
	_params = p_params;
	const int *values = p_params.values;

	_voicing = CLAMP(values[IronParams::VOICING], 0, VOICING_COUNT - 1);
	_pick_attack = CLAMP(values[IronParams::PICK_ATTACK], 0, 100) * 0.01;
	_pick_position = 0.04 + 0.30 * CLAMP(values[IronParams::PICK_POSITION], 0, 100) * 0.01;
	_palm_mute = CLAMP(values[IronParams::PALM_MUTE], 0, 100) * 0.01;
	_mute_velocity = CLAMP(values[IronParams::MUTE_VELOCITY], 0, 100) * 0.01;
	_sustain_seconds = CLAMP(values[IronParams::SUSTAIN_MS], 200, 12000) * 0.001;
	_release_seconds = CLAMP(values[IronParams::RELEASE_MS], 5, 500) * 0.001;
	_stiffness = CLAMP(values[IronParams::STIFFNESS], 0, 100) * 0.01;
	_pickup_position = 0.06 + 0.22 * CLAMP(values[IronParams::PICKUP_POSITION], 0, 100) * 0.01;
	_humanize = CLAMP(values[IronParams::HUMANIZE], 0, 100) * 0.01;
	_timing_seconds = CLAMP(values[IronParams::TIMING_MS], 0, 30) * 0.001;

	// Gain spreads over the four stages. The trim keeps a single note's peaks near -7 dBFS
	// across the range, with the compressed high-gain end near -18 dBFS RMS.
	const double gain = CLAMP(values[IronParams::GAIN], 0, 100) * 0.01;
	const double gain_db = MAX_GAIN_DB * std::pow(gain, 1.2);
	for (int k = 0; k < AMP_STAGE_COUNT; k++) {
		_stage_gain[k] = std::pow(10.0, gain_db * STAGE_GAIN_SHARE[k] / 20.0);
	}
	_output_trim = std::pow(10.0, (-2.7 - 17.6 * (1.0 - std::pow(1.0 - gain, 2.2))) / 20.0);

	_tight_coeffs = compute_biquad_coefficients(BIQUAD_HIGH_PASS, CLAMP(values[IronParams::TIGHT_HZ], 20, 1000), 0.6, 0.0, _sample_rate);
	_tone_coeffs[0] = compute_biquad_coefficients(BIQUAD_LOW_SHELF, 120.0, 0.7, CLAMP(values[IronParams::BASS_DB], -12, 12), _sample_rate);
	_tone_coeffs[1] = compute_biquad_coefficients(BIQUAD_PEAK, 650.0, 0.9, CLAMP(values[IronParams::MID_DB], -12, 12), _sample_rate);
	_tone_coeffs[2] = compute_biquad_coefficients(BIQUAD_HIGH_SHELF, 3200.0, 0.7, CLAMP(values[IronParams::TREBLE_DB], -12, 12), _sample_rate);

	const int cabinet = CLAMP(values[IronParams::CABINET], 0, CABINET_DIRECT);
	const int cabinet_biquad_count = cabinet == CABINET_DIRECT ? 0 : CABINET_BIQUAD_COUNT;
	if (cabinet_biquad_count != _cabinet_biquad_count) {
		for (sion::dsp::BiquadState &state : _cabinet_states) {
			state = {};
		}
		_cabinet_biquad_count = cabinet_biquad_count;
	}
	if (cabinet != CABINET_DIRECT) {
		const Cabinet &speaker = CABINETS[cabinet];
		// Off-axis, the mic hears a darker speaker with a softer presence peak.
		const double mic = CLAMP(values[IronParams::MIC], 0, 100) * 0.01;
		const double lowpass_hz = speaker.lowpass_hz * std::pow(2.0, -1.1 * mic);
		_cabinet_coeffs[0] = compute_biquad_coefficients(BIQUAD_HIGH_PASS, speaker.highpass_hz, 0.9, 0.0, _sample_rate);
		_cabinet_coeffs[1] = compute_biquad_coefficients(BIQUAD_PEAK, speaker.thump_hz, 1.4, speaker.thump_db, _sample_rate);
		_cabinet_coeffs[2] = compute_biquad_coefficients(BIQUAD_PEAK, speaker.presence_hz, 1.4, speaker.presence_db * (1.0 - 0.7 * mic), _sample_rate);
		_cabinet_coeffs[3] = compute_biquad_coefficients(BIQUAD_LOW_PASS, lowpass_hz, 0.8, 0.0, _sample_rate);
		_cabinet_coeffs[4] = compute_biquad_coefficients(BIQUAD_LOW_PASS, lowpass_hz * 1.15, 0.6, 0.0, _sample_rate);
	}

	for (GuitarString &string : _strings) {
		if (string.active) {
			_tune_string(string);
		}
	}
}

void SiOPMChannelIron::set_pitch(int p_value) {
	_current_pitch = p_value;
	_pitch_changed = true;
}

void SiOPMChannelIron::get_channel_params(const Ref<SiOPMChannelParams> &p_params) const {
	p_params->set_operator_count(1);
	for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
		p_params->set_master_volume(i, _volumes[i]);
	}
	p_params->set_instrument_gain_db(get_instrument_gain_db());
	p_params->set_pan(_pan);
}

void SiOPMChannelIron::set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation) {
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

void SiOPMChannelIron::offset_volume(int p_expression, int p_velocity) {
	_expression = (double)p_expression * 0.0078125;
}

void SiOPMChannelIron::note_on() {
	// The key velocity arrives as the expression set just before the key-on.
	const double velocity = CLAMP(_expression * (1.0 + _humanize * 0.2 * _random()), 0.0, 1.0);
	const double mute_offset = _mute_velocity * (1.0 - velocity) + _humanize * 0.06 * _random();
	const double mute = CLAMP(_palm_mute + mute_offset, 0.0, 1.0);
	const double onset = _timing_seconds * _random_unit();
	const double strum = (STRUM_SECONDS + _timing_seconds * 0.25) * (0.6 + 0.4 * _random_unit());

	const Voicing &voicing = VOICINGS[_voicing];
	for (int i = 0; i < STRING_COUNT; i++) {
		GuitarString &string = _strings[i];
		if (i >= voicing.count) {
			// A string the voicing leaves out is let go, like a hand moving to a new shape.
			string.pending_samples = -1;
			if (string.active) {
				string.released = true;
				_tune_string(string);
			}
			continue;
		}

		Pick &pick = string.pending;
		pick.semitones = voicing.strings[i].semitones;
		pick.level = voicing.strings[i].level;
		pick.velocity = velocity;
		pick.mute_offset = mute_offset;
		pick.position = CLAMP(_pick_position * (1.0 + _humanize * 0.35 * _random()), 0.02, 0.45);
		pick.hardness = CLAMP(_pick_attack * (0.55 + 0.45 * velocity) + _humanize * 0.12 * _random(), 0.0, 1.0);
		pick.cents = _humanize * HUMANIZE_DETUNE_CENTS * _random() + PALM_MUTE_SHARP_CENTS * mute;
		pick.bloom_cents = (3.0 + 12.0 * velocity * velocity) * (0.4 + 0.6 * _pick_attack) * (1.0 - 0.6 * mute) * (1.0 + 0.5 * _humanize * _random());

		// A ringing string fades out before the new pick replaces it.
		const int onset_samples = (int)((onset + i * strum) * _sample_rate);
		string.pending_samples = string.active ? MAX(onset_samples, _retrigger_fade_samples) : onset_samples;
	}

	_is_source_idling = false;
	SiOPMChannelBase::note_on();
}

void SiOPMChannelIron::note_off() {
	for (GuitarString &string : _strings) {
		if (string.active) {
			string.released = true;
			_tune_string(string);
		}
	}
	SiOPMChannelBase::note_off();
}

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

void SiOPMChannelIron::_process_iron(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe = _out_pipe->get();

	if (_pitch_changed) {
		_pitch_changed = false;
		// Bends, glides and legato slurs move the held strings. A string waiting on a new pick
		// keeps its old note while it fades.
		for (GuitarString &string : _strings) {
			if (string.active && !string.released && string.pending_samples < 0) {
				string.note = _current_pitch / 64.0;
				_tune_string(string);
			}
		}
	}

	const double gain = _expression * _output_trim * PIPE_PEAK;
	double peak = 0.0;
	for (int i = 0; i < p_length; i++) {
		double pickup = 0.0;
		for (GuitarString &string : _strings) {
			if (string.pending_samples == 0) {
				_pluck(string);
			} else if (string.pending_samples > 0) {
				if (string.active && string.fade_step >= 0.0 && string.pending_samples <= _retrigger_fade_samples) {
					string.fade_step = -string.fade / string.pending_samples;
				}
				string.pending_samples--;
			}
			if (string.active) {
				pickup += _tick_string(string);
			}
		}

		const double output = _tick_amp(pickup);
		peak = MAX(peak, std::abs(output));
		out_pipe->value = (int)(output * gain) + base_pipe->value;

		in_pipe = in_pipe->next();
		base_pipe = base_pipe->next();
		out_pipe = out_pipe->next();
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);

	// Once every string has rung out, the amp's filters settle within a few ms of silence.
	if (peak < SILENCE_LEVEL) {
		for (const GuitarString &string : _strings) {
			if (string.active || string.pending_samples >= 0) {
				return;
			}
		}
		_clear_amp();
		_is_source_idling = true;
	}
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SiOPMChannelIron::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	SiOPMChannelBase::initialize(p_prev, p_buffer_index);

	_sample_rate = _table->sampling_rate;
	_delay_smoothing = 1.0 - std::exp(-1.0 / (DELAY_SMOOTHING_SECONDS * _sample_rate));
	_loop_smoothing = 1.0 - std::exp(-1.0 / (LOOP_SMOOTHING_SECONDS * _sample_rate));
	_bloom_decay = std::exp(-1.0 / (BLOOM_SECONDS * _sample_rate));
	_click_decay = std::exp(-1.0 / (CLICK_SECONDS * _sample_rate));
	_click_lowpass_coeff = one_pole_coeff(CLICK_HIGHPASS_HZ, _sample_rate);
	_dc_coeff = one_pole_coeff(DC_BLOCK_HZ, _sample_rate);
	_retrigger_fade_samples = (int)(RETRIGGER_FADE_SECONDS * _sample_rate);
	_attack_fade_samples = (int)(ATTACK_FADE_SECONDS * _sample_rate);

	// Pickup inductance against the cable, then the classic tight-boost voicing into the amp.
	_pickup_coeffs = compute_biquad_coefficients(BIQUAD_LOW_PASS, PICKUP_RESONANCE_HZ, PICKUP_RESONANCE_Q, 0.0, _sample_rate);
	_hump_coeffs = compute_biquad_coefficients(BIQUAD_PEAK, 750.0, 0.7, 6.0, _sample_rate);
	// The gain stages run at twice the rate. Lowpasses on both sides of each shaper keep its
	// edges soft enough that 2x holds aliasing under -45 dB even at full gain.
	const double oversampled_rate = _sample_rate * 2.0;
	for (int k = 0; k < AMP_STAGE_COUNT; k++) {
		_stage_bias_offset[k] = fast_tanh(STAGE_BIAS[k]);
		_stage_input_coeff[k] = one_pole_coeff(STAGE_INPUT_LOWPASS_HZ[k], oversampled_rate);
		_stage_coupling_coeff[k] = one_pole_coeff(STAGE_COUPLING_HZ[k], oversampled_rate);
		_stage_output_coeff[k] = one_pole_coeff(STAGE_OUTPUT_LOWPASS_HZ[k], oversampled_rate);
	}
	// A fixed presence lift answers the saturated waveform's 6 dB/oct fall.
	_tone_coeffs[3] = compute_biquad_coefficients(BIQUAD_HIGH_SHELF, 1800.0, 0.6, 8.0, _sample_rate);

	_current_pitch = 0;
	_pitch_changed = false;
	_expression = 1.0;
	_random_step = _random_seed;

	reset();
	set_iron_params(IronParams());

	_process_function = Callable(this, "_process_iron");
}

void SiOPMChannelIron::reset() {
	for (GuitarString &string : _strings) {
		string.active = false;
		string.pending_samples = -1;
		string.click_level = 0.0;
	}
	_clear_amp();

	SiOPMChannelBase::reset();
}

String SiOPMChannelIron::_to_string() const {
	String params;
	params += "voicing=" + itos(_voicing) + ", ";
	params += "gain=" + itos(_params.values[IronParams::GAIN]) + ", ";
	params += "mute=" + itos(_params.values[IronParams::PALM_MUTE]) + ", ";
	params += "cabinet=" + itos(_params.values[IronParams::CABINET]) + ", ";
	params += "vol=" + rtos(_volumes[0]) + ", ";
	params += "pan=" + itos(_pan - 64);
	return "SiOPMChannelIron: " + params;
}

void SiOPMChannelIron::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_process_iron", "length"), &SiOPMChannelIron::_process_iron);
}

SiOPMChannelIron::SiOPMChannelIron(SiOPMSoundChip *p_chip) :
		SiOPMChannelBase(p_chip) {
	// Each channel walks its own stretch of the hash, so pooled voices never pick in lockstep.
	static uint32_t next_random_seed = 0;
	_random_seed = next_random_seed;
	next_random_seed += 0x9E3779B9u;

	for (GuitarString &string : _strings) {
		string.delay.prepare(DELAY_LINE_SAMPLES);
	}
	_process_function = Callable(this, "_process_iron");
}
