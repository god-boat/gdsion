/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_bloom_reverb.h"

#include <cmath>

namespace {
const double TANK_BASE_DELAY_MS[4] = { 29.7, 37.1, 41.9, 53.3 };
const double TANK_BASE_MOD_RATES[4] = { 0.07, 0.11, 0.17, 0.23 };
const double TANK_MOD_DEPTH_SCALE[4] = { 0.85, 1.0, 1.15, 1.3 };
const double TANK_PHASE_OFFSETS[4] = { 0.00, 0.37, 0.71, 0.19 };
const double EARLY_LEFT_MS[4] = { 7.1, 13.5, 21.9, 31.2 };
const double EARLY_RIGHT_MS[4] = { 9.3, 15.7, 24.6, 34.8 };
const double EARLY_GAINS[4] = { 0.32, 0.24, 0.18, 0.12 };
const double DIFFUSER_LEFT_MS[2] = { 4.8, 8.3 };
const double DIFFUSER_RIGHT_MS[2] = { 5.6, 9.7 };
const double AIR_LEFT_MS[2] = { 11.3, 17.9 };
const double AIR_RIGHT_MS[2] = { 13.7, 19.5 };
const double AIR_MOD_RATES[2] = { 0.09, 0.14 };
const double AIR_MOD_DEPTH_SCALE[2] = { 0.35, 0.5 };
const double AIR_PHASE_LEFT[2] = { 0.15, 0.63 };
const double AIR_PHASE_RIGHT[2] = { 0.42, 0.88 };
}

void SiEffectBloomReverb::FractionalDelay::resize_samples(int p_length) {
	int clamped = MAX(p_length, 2);
	buffer.resize(clamped);
	buffer.fill(0.0);
	write_index = 0;
}

void SiEffectBloomReverb::FractionalDelay::reset() {
	if (!buffer.is_empty()) {
		buffer.fill(0.0);
	}
	write_index = 0;
}

void SiEffectBloomReverb::FractionalDelay::write(double p_sample) {
	if (buffer.is_empty()) {
		return;
	}
	buffer.write[write_index] = p_sample;
	write_index++;
	if (write_index >= buffer.size()) {
		write_index = 0;
	}
}

double SiEffectBloomReverb::FractionalDelay::read_samples(double p_delay_samples) const {
	if (buffer.size() < 2) {
		return 0.0;
	}
	double delay = CLAMP(p_delay_samples, 1.0, (double)(buffer.size() - 2));
	double read_pos = (double)write_index - delay;
	const double size = (double)buffer.size();
	while (read_pos < 0.0) {
		read_pos += size;
	}
	while (read_pos >= size) {
		read_pos -= size;
	}
	int index_a = (int)read_pos;
	int index_b = index_a + 1;
	if (index_b >= buffer.size()) {
		index_b = 0;
	}
	double frac = read_pos - (double)index_a;
	return buffer[index_a] + (buffer[index_b] - buffer[index_a]) * frac;
}

void SiEffectBloomReverb::OnePoleLowPass::set_cutoff(double p_hz, double p_sample_rate) {
	double clamped = CLAMP(p_hz, 10.0, p_sample_rate * 0.45);
	alpha = Math::exp(-2.0 * M_PI * clamped / p_sample_rate);
}

double SiEffectBloomReverb::OnePoleLowPass::process(double p_input) {
	z = (1.0 - alpha) * p_input + alpha * z;
	return z;
}

void SiEffectBloomReverb::OnePoleLowPass::reset() {
	z = 0.0;
}

void SiEffectBloomReverb::OnePoleHighPass::set_cutoff(double p_hz, double p_sample_rate) {
	double clamped = CLAMP(p_hz, 10.0, p_sample_rate * 0.45);
	alpha = Math::exp(-2.0 * M_PI * clamped / p_sample_rate);
}

double SiEffectBloomReverb::OnePoleHighPass::process(double p_input) {
	double output = alpha * (y1 + p_input - x1);
	x1 = p_input;
	y1 = output;
	return output;
}

void SiEffectBloomReverb::OnePoleHighPass::reset() {
	x1 = 0.0;
	y1 = 0.0;
}

void SiEffectBloomReverb::BloomEnvelope::set_attack_ms(double p_ms, double p_sample_rate) {
	double sample_rate = MAX(p_sample_rate, 1.0);
	double slow_attack_s = MAX(p_ms * 0.001, 0.001);
	double fast_attack_s = 0.003;
	double fast_release_s = 0.045;
	double slow_release_s = MAX(slow_attack_s * 1.6, 0.160);

	fast_attack_coeff = 1.0 - Math::exp(-1.0 / (fast_attack_s * sample_rate));
	fast_release_coeff = 1.0 - Math::exp(-1.0 / (fast_release_s * sample_rate));
	slow_attack_coeff = 1.0 - Math::exp(-1.0 / (slow_attack_s * sample_rate));
	slow_release_coeff = 1.0 - Math::exp(-1.0 / (slow_release_s * sample_rate));
}

double SiEffectBloomReverb::BloomEnvelope::process(double p_input) {
	double level = Math::abs(p_input);
	double fast_coeff = level > fast_env ? fast_attack_coeff : fast_release_coeff;
	fast_env += (level - fast_env) * fast_coeff;
	double slow_coeff = level > slow_env ? slow_attack_coeff : slow_release_coeff;
	slow_env += (level - slow_env) * slow_coeff;
	return CLAMP(slow_env / MAX(fast_env, 1.0e-8), 0.0, 1.0);
}

void SiEffectBloomReverb::BloomEnvelope::reset() {
	fast_env = 0.0;
	slow_env = 0.0;
}

double SiEffectBloomReverb::Lfo::process(double p_rate_hz, double p_depth_samples, double p_sample_rate) {
	phase += p_rate_hz / p_sample_rate;
	if (phase >= 1.0) {
		phase -= Math::floor(phase);
	}
	double radians = 2.0 * M_PI * (phase + phase_offset);
	return Math::sin(radians) * p_depth_samples;
}

void SiEffectBloomReverb::Lfo::reset(double p_phase_offset) {
	phase = 0.0;
	phase_offset = p_phase_offset;
}

double SiEffectBloomReverb::AllpassStage::process(double p_input, double p_delay_samples, double p_feedback) {
	double delayed = delay.read_samples(p_delay_samples);
	double output = delayed - p_feedback * p_input;
	delay.write(p_input + p_feedback * output);
	return output;
}

void SiEffectBloomReverb::AllpassStage::reset() {
	delay.reset();
}

void SiEffectBloomReverb::AirSide::reset() {
	for (int i = 0; i < AIR_DELAY_COUNT; i++) {
		delays[i].reset();
	}
	diffuser_a.reset();
	diffuser_b.reset();
	hp.reset();
}

double SiEffectBloomReverb::_clamp01(double p_value) {
	return CLAMP(p_value, 0.0, 1.0);
}

double SiEffectBloomReverb::_exp_lerp(double p_min, double p_max, double p_t) {
	if (p_min <= 0.0 || p_max <= 0.0) {
		return Math::lerp(p_min, p_max, p_t);
	}
	return p_min * Math::pow(p_max / p_min, p_t);
}

double SiEffectBloomReverb::_softclip(double p_input) {
	return p_input / (1.0 + Math::abs(p_input));
}

void SiEffectBloomReverb::_copy_params(BloomParams &r_dst, const BloomParams &p_src) {
	r_dst = p_src;
}

void SiEffectBloomReverb::_ensure_delay_buffers() {
	int sample_rate = MAX((int)Math::round(_get_sampling_rate()), 1);
	if (_cached_sample_rate == sample_rate) {
		return;
	}
	_cached_sample_rate = sample_rate;

	_predelay_left.resize_samples((int)Math::ceil(sample_rate * 0.30) + 4);
	_predelay_right.resize_samples((int)Math::ceil(sample_rate * 0.30) + 4);
	_early_left.resize_samples((int)Math::ceil(sample_rate * 0.09) + 4);
	_early_right.resize_samples((int)Math::ceil(sample_rate * 0.09) + 4);

	for (int i = 0; i < INPUT_DIFFUSER_COUNT; i++) {
		_diffusers_left[i].delay.resize_samples((int)Math::ceil(sample_rate * 0.03) + 4);
		_diffusers_right[i].delay.resize_samples((int)Math::ceil(sample_rate * 0.03) + 4);
	}
	for (int i = 0; i < TANK_LINE_COUNT; i++) {
		_tank[i].delay.resize_samples((int)Math::ceil(sample_rate * 0.18) + 8);
		_tank[i].mod.reset(TANK_PHASE_OFFSETS[i]);
	}
	for (int i = 0; i < AIR_DELAY_COUNT; i++) {
		_air_left.delays[i].resize_samples((int)Math::ceil(sample_rate * 0.04) + 4);
		_air_right.delays[i].resize_samples((int)Math::ceil(sample_rate * 0.04) + 4);
		_air_left.mods[i].reset(AIR_PHASE_LEFT[i]);
		_air_right.mods[i].reset(AIR_PHASE_RIGHT[i]);
	}
	_air_left.diffuser_a.delay.resize_samples((int)Math::ceil(sample_rate * 0.015) + 4);
	_air_left.diffuser_b.delay.resize_samples((int)Math::ceil(sample_rate * 0.015) + 4);
	_air_right.diffuser_a.delay.resize_samples((int)Math::ceil(sample_rate * 0.015) + 4);
	_air_right.diffuser_b.delay.resize_samples((int)Math::ceil(sample_rate * 0.015) + 4);

	_reset_signal_state();
}

void SiEffectBloomReverb::_reset_signal_state() {
	_predelay_left.reset();
	_predelay_right.reset();
	_early_left.reset();
	_early_right.reset();
	for (int i = 0; i < INPUT_DIFFUSER_COUNT; i++) {
		_diffusers_left[i].reset();
		_diffusers_right[i].reset();
	}
	for (int i = 0; i < TANK_LINE_COUNT; i++) {
		_tank[i].delay.reset();
		_tank[i].hp.reset();
		_tank[i].lp.reset();
		_tank[i].mod.reset(TANK_PHASE_OFFSETS[i]);
	}
	_air_left.reset();
	_air_right.reset();
	_input_hp_left.reset();
	_input_hp_right.reset();
	_wet_lp_left.reset();
	_wet_lp_right.reset();
	_bloom_envelope_left.reset();
	_bloom_envelope_right.reset();
	_duck_env = 0.0;
}

void SiEffectBloomReverb::_derive_params(const BloomParams &p_params, DerivedParams &r_out) const {
	double size = _clamp01(p_params.size);
	double decay = _clamp01(p_params.decay);
	double bloom = _clamp01(p_params.bloom);
	double tone = _clamp01(p_params.tone);
	double air = _clamp01(p_params.air);
	double motion = _clamp01(p_params.motion);
	double width = _clamp01(p_params.width);
	double mix = _clamp01(p_params.mix);
	double duck = _clamp01(p_params.duck);
	double freeze = _clamp01(p_params.freeze);
	double bloom_curve = Math::pow(bloom, 0.7);
	double bloom_depth = bloom * bloom;

	r_out.size_scale = Math::lerp(0.55, 2.75, Math::pow(size, 1.7));
	r_out.decay_seconds = _exp_lerp(0.35, 18.0, decay);
	r_out.predelay_samples = MAX(0.0, p_params.predelay_ms) * _get_samples_per_ms();
	r_out.bloom_attack_ms = Math::lerp(8.0, 520.0, bloom_depth);
	r_out.direct_late_amount = Math::lerp(1.0, 0.05, bloom_curve);
	r_out.bloom_amount = Math::lerp(0.0, 3.25, bloom_depth);
	r_out.diffusion_feedback = Math::lerp(0.38, 0.88, bloom_curve);
	r_out.early_gain = Math::lerp(0.9, 0.03, bloom_curve);
	r_out.late_gain = Math::lerp(0.8, 2.2, bloom_depth);
	r_out.drive = Math::lerp(1.0, 1.05, bloom);
	r_out.high_damp_hz = MIN(_exp_lerp(1800.0, 14000.0, tone), MAX(p_params.high_cut_hz, 2500.0));
	r_out.low_cut_hz = CLAMP(p_params.low_cut_hz, 20.0, 800.0);
	r_out.feedback_low_cut_hz = CLAMP(r_out.low_cut_hz * 0.65, 40.0, 420.0);
	r_out.high_cut_hz = CLAMP(p_params.high_cut_hz, 1000.0, 20000.0);
	double motion_depth_ms = Math::lerp(0.0, 8.0, Math::pow(motion, 1.5)) + Math::lerp(0.0, 9.0, bloom_depth);
	r_out.mod_depth_samples = (_get_sampling_rate() * motion_depth_ms) / 1000.0;
	r_out.width_gain = Math::lerp(0.3, 1.35, width) * Math::lerp(0.85, 1.65, bloom_curve);
	r_out.air_gain = Math::pow(air, 1.4) * 0.35 + bloom_depth * 0.22;
	r_out.air_hp_hz = _exp_lerp(1800.0, 6500.0, 1.0 - air);
	r_out.wet_makeup_gain = 1.8;
	r_out.duck = duck;
	r_out.freeze = freeze;

	double duck_attack_s = Math::lerp(0.005, 0.020, duck);
	double duck_release_s = Math::lerp(0.150, 0.900, duck);
	r_out.duck_attack_coeff = 1.0 - Math::exp(-1.0 / MAX(duck_attack_s * _get_sampling_rate(), 1.0));
	r_out.duck_release_coeff = 1.0 - Math::exp(-1.0 / MAX(duck_release_s * _get_sampling_rate(), 1.0));

	_calculate_constant_power_gains(mix, r_out.dry_gain, r_out.wet_gain);

	for (int i = 0; i < TANK_LINE_COUNT; i++) {
		double delay_seconds = (TANK_BASE_DELAY_MS[i] * r_out.size_scale) * 0.001;
		r_out.tank_delay_samples[i] = delay_seconds * _get_sampling_rate();
		double feedback_gain = Math::pow(10.0, (-3.0 * delay_seconds) / r_out.decay_seconds);
		double feedback_ceiling = Math::lerp(0.985, 0.9995, freeze);
		r_out.tank_feedback_gain[i] = MIN(feedback_gain, feedback_ceiling);
		r_out.tank_mod_rates[i] = TANK_BASE_MOD_RATES[i] * Math::lerp(0.6, 2.0, motion);
	}

	double spacing_scale = Math::lerp(0.7, 1.8, size);
	double diffuser_scale = Math::lerp(0.85, 1.25, size);
	for (int i = 0; i < EARLY_TAP_COUNT; i++) {
		r_out.early_tap_left_samples[i] = EARLY_LEFT_MS[i] * spacing_scale * _get_samples_per_ms();
		r_out.early_tap_right_samples[i] = EARLY_RIGHT_MS[i] * spacing_scale * _get_samples_per_ms();
	}
	for (int i = 0; i < INPUT_DIFFUSER_COUNT; i++) {
		r_out.diffuser_left_samples[i] = DIFFUSER_LEFT_MS[i] * diffuser_scale * _get_samples_per_ms();
		r_out.diffuser_right_samples[i] = DIFFUSER_RIGHT_MS[i] * diffuser_scale * _get_samples_per_ms();
	}
	for (int i = 0; i < AIR_DELAY_COUNT; i++) {
		r_out.air_delay_left_samples[i] = AIR_LEFT_MS[i] * _get_samples_per_ms();
		r_out.air_delay_right_samples[i] = AIR_RIGHT_MS[i] * _get_samples_per_ms();
		r_out.air_mod_rates[i] = AIR_MOD_RATES[i] * Math::lerp(0.7, 1.9, motion);
	}
}

void SiEffectBloomReverb::_begin_block_smoothing(int p_length) {
	int count = MAX(p_length, 1);
	_block_steps.size = (_target_params.size - _current_params.size) / count;
	_block_steps.decay = (_target_params.decay - _current_params.decay) / count;
	_block_steps.bloom = (_target_params.bloom - _current_params.bloom) / count;
	_block_steps.tone = (_target_params.tone - _current_params.tone) / count;
	_block_steps.air = (_target_params.air - _current_params.air) / count;
	_block_steps.motion = (_target_params.motion - _current_params.motion) / count;
	_block_steps.width = (_target_params.width - _current_params.width) / count;
	_block_steps.mix = (_target_params.mix - _current_params.mix) / count;
	_block_steps.predelay_ms = (_target_params.predelay_ms - _current_params.predelay_ms) / count;
	_block_steps.duck = (_target_params.duck - _current_params.duck) / count;
	_block_steps.low_cut_hz = (_target_params.low_cut_hz - _current_params.low_cut_hz) / count;
	_block_steps.high_cut_hz = (_target_params.high_cut_hz - _current_params.high_cut_hz) / count;
	_block_steps.freeze = (_target_params.freeze - _current_params.freeze) / count;
}

void SiEffectBloomReverb::_advance_smoothed_params(int p_segment_length) {
	double step = (double)p_segment_length;
	_current_params.size += _block_steps.size * step;
	_current_params.decay += _block_steps.decay * step;
	_current_params.bloom += _block_steps.bloom * step;
	_current_params.tone += _block_steps.tone * step;
	_current_params.air += _block_steps.air * step;
	_current_params.motion += _block_steps.motion * step;
	_current_params.width += _block_steps.width * step;
	_current_params.mix += _block_steps.mix * step;
	_current_params.predelay_ms += _block_steps.predelay_ms * step;
	_current_params.duck += _block_steps.duck * step;
	_current_params.low_cut_hz += _block_steps.low_cut_hz * step;
	_current_params.high_cut_hz += _block_steps.high_cut_hz * step;
	_current_params.freeze += _block_steps.freeze * step;
}

void SiEffectBloomReverb::_set_arg_value(int p_arg_index, double p_value, bool p_snap_current) {
	switch (p_arg_index) {
		case ARG_SIZE:
			_target_params.size = _clamp01(p_value);
			break;
		case ARG_DECAY:
			_target_params.decay = _clamp01(p_value);
			break;
		case ARG_BLOOM:
			_target_params.bloom = _clamp01(p_value);
			break;
		case ARG_TONE:
			_target_params.tone = _clamp01(p_value);
			break;
		case ARG_AIR:
			_target_params.air = _clamp01(p_value);
			break;
		case ARG_MOTION:
			_target_params.motion = _clamp01(p_value);
			break;
		case ARG_WIDTH:
			_target_params.width = _clamp01(p_value);
			break;
		case ARG_MIX:
			_target_params.mix = _clamp01(p_value);
			break;
		case ARG_PREDELAY_MS:
			_target_params.predelay_ms = CLAMP(p_value, 0.0, 250.0);
			break;
		case ARG_DUCK:
			_target_params.duck = _clamp01(p_value);
			break;
		case ARG_LOW_CUT_HZ:
			_target_params.low_cut_hz = CLAMP(p_value, 20.0, 800.0);
			break;
		case ARG_HIGH_CUT_HZ:
			_target_params.high_cut_hz = CLAMP(p_value, 1000.0, 20000.0);
			break;
		case ARG_FREEZE:
			_target_params.freeze = _clamp01(p_value);
			break;
		default:
			break;
	}
	if (p_snap_current) {
		_copy_params(_current_params, _target_params);
	}
}

double SiEffectBloomReverb::_process_air_side(
		AirSide &r_side,
		double p_input,
		double p_hp_hz,
		const double *p_delay_samples,
		const double *p_rates,
		double p_mod_depth_samples) {
	r_side.hp.set_cutoff(p_hp_hz, _get_sampling_rate());
	double band = r_side.hp.process(p_input);
	double stage_a = band;
	for (int i = 0; i < AIR_DELAY_COUNT; i++) {
		double mod = r_side.mods[i].process(p_rates[i], p_mod_depth_samples * AIR_MOD_DEPTH_SCALE[i], _get_sampling_rate());
		double delayed = r_side.delays[i].read_samples(p_delay_samples[i] + mod);
		r_side.delays[i].write(stage_a + delayed * 0.18);
		stage_a = delayed + stage_a * 0.35;
	}
	stage_a = r_side.diffuser_a.process(stage_a, 6.1 * _get_samples_per_ms(), 0.45);
	stage_a = r_side.diffuser_b.process(stage_a, 9.2 * _get_samples_per_ms(), 0.38);
	return stage_a;
}

int SiEffectBloomReverb::prepare_process() {
	_ensure_delay_buffers();
	_reset_signal_state();
	return 2;
}

int SiEffectBloomReverb::process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	_ensure_delay_buffers();
	if (p_length <= 0) {
		return 2;
	}

	_begin_block_smoothing(p_length);

	const bool stereo = (p_channels == 2);
	const int start_index = p_start_index << 1;
	int processed = 0;

	while (processed < p_length) {
		int segment_length = MIN(PARAM_UPDATE_INTERVAL, p_length - processed);
		DerivedParams params;
		_derive_params(_current_params, params);

		_input_hp_left.set_cutoff(params.low_cut_hz, _get_sampling_rate());
		_input_hp_right.set_cutoff(params.low_cut_hz, _get_sampling_rate());
		_wet_lp_left.set_cutoff(params.high_cut_hz, _get_sampling_rate());
		_wet_lp_right.set_cutoff(params.high_cut_hz, _get_sampling_rate());
		_bloom_envelope_left.set_attack_ms(params.bloom_attack_ms, _get_sampling_rate());
		_bloom_envelope_right.set_attack_ms(params.bloom_attack_ms, _get_sampling_rate());

		for (int line = 0; line < TANK_LINE_COUNT; line++) {
			_tank[line].hp.set_cutoff(params.feedback_low_cut_hz, _get_sampling_rate());
			_tank[line].lp.set_cutoff(params.high_damp_hz, _get_sampling_rate());
		}

		for (int sample = 0; sample < segment_length; sample++) {
			int frame_index = processed + sample;
			int buffer_index = start_index + (frame_index << 1);
			double dry_l = (*r_buffer)[buffer_index];
			double dry_r = stereo ? (*r_buffer)[buffer_index + 1] : dry_l;

			double conditioned_l = _input_hp_left.process(dry_l);
			double conditioned_r = _input_hp_right.process(dry_r);

			double predelayed_l = _predelay_left.read_samples(params.predelay_samples);
			double predelayed_r = _predelay_right.read_samples(params.predelay_samples);
			_predelay_left.write(conditioned_l);
			_predelay_right.write(conditioned_r);

			double early_l = 0.0;
			double early_r = 0.0;
			for (int tap = 0; tap < EARLY_TAP_COUNT; tap++) {
				early_l += _early_left.read_samples(params.early_tap_left_samples[tap]) * EARLY_GAINS[tap];
				early_r += _early_right.read_samples(params.early_tap_right_samples[tap]) * EARLY_GAINS[tap];
			}
			_early_left.write(predelayed_l);
			_early_right.write(predelayed_r);
			early_l *= params.early_gain;
			early_r *= params.early_gain;

			double bloom_gain_l = Math::pow(_bloom_envelope_left.process(predelayed_l), 1.7);
			double bloom_gain_r = Math::pow(_bloom_envelope_right.process(predelayed_r), 1.7);
			double bloomed_l = (predelayed_l * 0.65 + predelayed_r * 0.35) * bloom_gain_l;
			double bloomed_r = (predelayed_r * 0.65 + predelayed_l * 0.35) * bloom_gain_r;

			double tank_input_l = predelayed_l * params.direct_late_amount + bloomed_l * params.bloom_amount;
			double tank_input_r = predelayed_r * params.direct_late_amount + bloomed_r * params.bloom_amount;
			double freeze_input_scale = Math::lerp(1.0, 0.05, params.freeze);
			tank_input_l = _softclip(tank_input_l * params.drive) / params.drive * params.late_gain * freeze_input_scale;
			tank_input_r = _softclip(tank_input_r * params.drive) / params.drive * params.late_gain * freeze_input_scale;

			double diffused_l = tank_input_l;
			double diffused_r = tank_input_r;
			for (int stage = 0; stage < INPUT_DIFFUSER_COUNT; stage++) {
				diffused_l = _diffusers_left[stage].process(diffused_l, params.diffuser_left_samples[stage], params.diffusion_feedback);
				diffused_r = _diffusers_right[stage].process(diffused_r, params.diffuser_right_samples[stage], params.diffusion_feedback);
			}

			double tank_reads[TANK_LINE_COUNT] = {};
			double tank_filtered[TANK_LINE_COUNT] = {};
			for (int line = 0; line < TANK_LINE_COUNT; line++) {
				double mod = _tank[line].mod.process(
						params.tank_mod_rates[line],
						params.mod_depth_samples * TANK_MOD_DEPTH_SCALE[line],
						_get_sampling_rate());
				tank_reads[line] = _tank[line].delay.read_samples(params.tank_delay_samples[line] + mod);
				double filtered = _tank[line].hp.process(tank_reads[line]);
				tank_filtered[line] = _tank[line].lp.process(filtered);
			}

			double mix0 = (tank_filtered[0] + tank_filtered[1] + tank_filtered[2] + tank_filtered[3]) * 0.5;
			double mix1 = (tank_filtered[0] - tank_filtered[1] + tank_filtered[2] - tank_filtered[3]) * 0.5;
			double mix2 = (tank_filtered[0] + tank_filtered[1] - tank_filtered[2] - tank_filtered[3]) * 0.5;
			double mix3 = (tank_filtered[0] - tank_filtered[1] - tank_filtered[2] + tank_filtered[3]) * 0.5;
			double matrix[TANK_LINE_COUNT] = { mix0, mix1, mix2, mix3 };

			double injected[TANK_LINE_COUNT] = {
				diffused_l + 0.25 * diffused_r,
				diffused_r + 0.25 * diffused_l,
				0.60 * diffused_l - 0.30 * diffused_r,
				0.60 * diffused_r - 0.30 * diffused_l,
			};

			for (int line = 0; line < TANK_LINE_COUNT; line++) {
				double feedback_gain = Math::lerp(params.tank_feedback_gain[line], 0.9995, params.freeze);
				double write_value = injected[line] + matrix[line] * feedback_gain;
				write_value = CLAMP(write_value, -3.0, 3.0);
				_tank[line].delay.write(write_value);
			}

			double wet_l =
					0.70 * tank_filtered[0] +
					0.45 * tank_filtered[1] +
					0.60 * tank_filtered[2] -
					0.35 * tank_filtered[3];
			double wet_r =
					0.45 * tank_filtered[0] +
					0.70 * tank_filtered[1] -
					0.35 * tank_filtered[2] +
					0.60 * tank_filtered[3];

			double air_l = _process_air_side(_air_left, wet_l, params.air_hp_hz, params.air_delay_left_samples, params.air_mod_rates, params.mod_depth_samples);
			double air_r = _process_air_side(_air_right, wet_r, params.air_hp_hz, params.air_delay_right_samples, params.air_mod_rates, params.mod_depth_samples);
			wet_l += air_l * params.air_gain;
			wet_r += air_r * params.air_gain;

			double mid = (wet_l + wet_r) * 0.5;
			double side = (wet_l - wet_r) * 0.5 * params.width_gain;
			wet_l = mid + side;
			wet_r = mid - side;

			double wet_total_l = _wet_lp_left.process((wet_l + early_l) * params.wet_makeup_gain);
			double wet_total_r = _wet_lp_right.process((wet_r + early_r) * params.wet_makeup_gain);

			double detector = MAX(Math::abs(dry_l), Math::abs(dry_r));
			double duck_coeff = detector > _duck_env ? params.duck_attack_coeff : params.duck_release_coeff;
			_duck_env += (detector - _duck_env) * duck_coeff;
			double duck_reduction = params.duck * CLAMP(_duck_env * 2.5, 0.0, 1.0);
			double duck_gain = Math::pow(10.0, (-18.0 * duck_reduction) / 20.0);

			r_buffer->write[buffer_index] = dry_l * params.dry_gain + (wet_total_l * duck_gain) * params.wet_gain;
			r_buffer->write[buffer_index + 1] = dry_r * params.dry_gain + (wet_total_r * duck_gain) * params.wet_gain;
		}

		_advance_smoothed_params(segment_length);
		processed += segment_length;
	}

	_copy_params(_current_params, _target_params);
	return 2;
}

void SiEffectBloomReverb::set_by_mml(Vector<double> p_args) {
	BloomParams parsed = _target_params;
	for (int i = 0; i < ARG_COUNT; i++) {
		double value = _get_mml_arg(p_args, i, NAN);
		if (!Math::is_nan(value)) {
			_target_params = parsed;
			_set_arg_value(i, value, false);
			parsed = _target_params;
		}
	}
	_current_params = parsed;
	_target_params = parsed;
}

bool SiEffectBloomReverb::set_arg(int p_arg_index, double p_value) {
	if (p_arg_index < 0 || p_arg_index >= ARG_COUNT) {
		return false;
	}
	_set_arg_value(p_arg_index, p_value, false);
	return true;
}

void SiEffectBloomReverb::reset() {
	_current_params = BloomParams();
	_target_params = BloomParams();
	_block_steps = BloomParams();
	_reset_signal_state();
}

void SiEffectBloomReverb::_bind_methods() {
}

SiEffectBloomReverb::SiEffectBloomReverb() :
		SiEffectBase() {
	reset();
}
