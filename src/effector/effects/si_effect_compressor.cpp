/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_compressor.h"

void SiEffectCompressor::set_params(double p_threshold_db, double p_ratio,
		double p_attack_ms, double p_release_ms,
		double p_knee_db, double p_makeup_db,
		double p_mix, double p_detector_blend) {
	_params.threshold_db.store((float)CLAMP(p_threshold_db, -60.0, 0.0), std::memory_order_relaxed);
	_params.ratio.store((float)CLAMP(p_ratio, MIN_RATIO, MAX_RATIO), std::memory_order_relaxed);
	_params.attack_ms.store((float)CLAMP(p_attack_ms, 0.1, 100.0), std::memory_order_relaxed);
	_params.release_ms.store((float)CLAMP(p_release_ms, 10.0, 1000.0), std::memory_order_relaxed);
	_params.knee_db.store((float)CLAMP(p_knee_db, 0.0, 24.0), std::memory_order_relaxed);
	_params.makeup_db.store((float)CLAMP(p_makeup_db, -24.0, 24.0), std::memory_order_relaxed);
	_params.mix.store((float)CLAMP(p_mix, 0.0, 1.0), std::memory_order_relaxed);
	_params.detector_blend.store((float)CLAMP(p_detector_blend, 0.0, 1.0), std::memory_order_relaxed);
}

bool SiEffectCompressor::set_arg(int p_arg_index, double p_value) {
	switch (p_arg_index) {
		case 0: _params.threshold_db.store((float)CLAMP(p_value, -60.0, 0.0), std::memory_order_relaxed); return true;
		case 1: _params.ratio.store((float)CLAMP(p_value, MIN_RATIO, MAX_RATIO), std::memory_order_relaxed); return true;
		case 2: _params.attack_ms.store((float)CLAMP(p_value, 0.1, 100.0), std::memory_order_relaxed); return true;
		case 3: _params.release_ms.store((float)CLAMP(p_value, 10.0, 1000.0), std::memory_order_relaxed); return true;
		case 4: _params.knee_db.store((float)CLAMP(p_value, 0.0, 24.0), std::memory_order_relaxed); return true;
		case 5: _params.makeup_db.store((float)CLAMP(p_value, -24.0, 24.0), std::memory_order_relaxed); return true;
		case 6: _params.mix.store((float)CLAMP(p_value, 0.0, 1.0), std::memory_order_relaxed); return true;
		case 7: _params.detector_blend.store((float)CLAMP(p_value, 0.0, 1.0), std::memory_order_relaxed); return true;
		default: return false;
	}
}

int SiEffectCompressor::prepare_process() {
	_rms_env = 0.0;
	_gain_db = 0.0;
	_current_gr_db.store(0.0f, std::memory_order_relaxed);
	return 2;
}

int SiEffectCompressor::process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	double sample_rate = _get_sampling_rate();
	if (sample_rate <= 0.0) {
		return p_channels;
	}

	// Handle RT-safe state reset request from main thread.
	if (_reset_state_requested.exchange(false, std::memory_order_acq_rel)) {
		_rms_env = 0.0;
		_gain_db = 0.0;
	}

	// Snapshot atomic params at block boundary (no lock needed).
	double threshold_db    = _params.threshold_db.load(std::memory_order_relaxed);
	double ratio           = MAX(_params.ratio.load(std::memory_order_relaxed), MIN_RATIO);
	double attack_ms       = MAX(_params.attack_ms.load(std::memory_order_relaxed), MIN_ATTACK_MS);
	double release_ms      = MAX(_params.release_ms.load(std::memory_order_relaxed), MIN_RELEASE_MS);
	double knee_db         = _params.knee_db.load(std::memory_order_relaxed);
	double makeup_db       = _params.makeup_db.load(std::memory_order_relaxed);
	double mix             = _params.mix.load(std::memory_order_relaxed);
	double detector_blend  = _params.detector_blend.load(std::memory_order_relaxed);

	// Recompute coefficients for this block.
	_attack_coeff = _compute_coeff(attack_ms, sample_rate);
	_release_coeff = _compute_coeff(release_ms, sample_rate);
	_rms_coeff = _compute_coeff(DEFAULT_RMS_MS, sample_rate);

	int start_index = p_start_index << 1;
	int length = p_length << 1;

	for (int i = start_index; i < (start_index + length); i += 2) {
		double l = (*r_buffer)[i];
		double r = (*r_buffer)[i + 1];
		double dry_l = l;
		double dry_r = r;

		// Stereo-linked detector.
		double peak = MAX(Math::abs(l), Math::abs(r));
		double energy = 0.5 * (l * l + r * r);
		_rms_env = _rms_coeff * _rms_env + (1.0 - _rms_coeff) * energy;
		double rms = std::sqrt(_rms_env + EPSILON);

		double detector = Math::lerp(peak, rms, detector_blend);
		double input_db = _linear_to_db(detector);

		// Gain computer (soft knee).
		double over_db = input_db - threshold_db;
		double target_gr_db = _soft_knee_gr(over_db, knee_db, ratio);

		// Gain smoothing (one-pole). More negative = more reduction = attack.
		double coeff = (target_gr_db < _gain_db) ? _attack_coeff : _release_coeff;
		_gain_db = coeff * _gain_db + (1.0 - coeff) * target_gr_db;

		double gain = _db_to_linear(_gain_db + makeup_db);

		// Apply gain and mix.
		double wet_l = l * gain;
		double wet_r = r * gain;

		r_buffer->write[i] = Math::lerp(dry_l, wet_l, mix);
		r_buffer->write[i + 1] = Math::lerp(dry_r, wet_r, mix);
	}

	// Publish metering (positive value for UI).
	_current_gr_db.store((float)(-_gain_db), std::memory_order_relaxed);

	return p_channels;
}

void SiEffectCompressor::set_by_mml(Vector<double> p_args) {
	double threshold_db   = _get_mml_arg(p_args, 0, -18.0);
	double ratio          = _get_mml_arg(p_args, 1, 4.0);
	double attack_ms      = _get_mml_arg(p_args, 2, 10.0);
	double release_ms     = _get_mml_arg(p_args, 3, 120.0);
	double knee_db        = _get_mml_arg(p_args, 4, 6.0);
	double makeup_db      = _get_mml_arg(p_args, 5, 0.0);
	double mix            = _get_mml_arg(p_args, 6, 1.0);
	double detector_blend = _get_mml_arg(p_args, 7, 0.65);

	set_params(threshold_db, ratio, attack_ms, release_ms, knee_db, makeup_db, mix, detector_blend);
}

void SiEffectCompressor::reset() {
	set_params();
	_reset_state_requested.store(true, std::memory_order_release);
}

void SiEffectCompressor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_params", "threshold_db", "ratio", "attack_ms", "release_ms", "knee_db", "makeup_db", "mix", "detector_blend"),
			&SiEffectCompressor::set_params,
			DEFVAL(-18.0), DEFVAL(4.0), DEFVAL(10.0), DEFVAL(120.0), DEFVAL(6.0), DEFVAL(0.0), DEFVAL(1.0), DEFVAL(0.65));

	ClassDB::bind_method(D_METHOD("get_gain_reduction_db"), &SiEffectCompressor::get_gain_reduction_db);
}

SiEffectCompressor::SiEffectCompressor(double p_threshold_db, double p_ratio,
		double p_attack_ms, double p_release_ms,
		double p_knee_db, double p_makeup_db,
		double p_mix, double p_detector_blend) :
		SiEffectBase() {
	set_params(p_threshold_db, p_ratio, p_attack_ms, p_release_ms, p_knee_db, p_makeup_db, p_mix, p_detector_blend);
}
