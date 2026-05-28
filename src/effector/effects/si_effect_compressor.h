/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_COMPRESSOR_H
#define SI_EFFECT_COMPRESSOR_H

#include "effector/si_effect_base.h"

#include <atomic>
#include <cmath>

class SiEffectCompressor : public SiEffectBase {
	GDCLASS(SiEffectCompressor, SiEffectBase)

	static constexpr double EPSILON = 1e-12;
	static constexpr double MIN_RATIO = 1.0;
	static constexpr double MAX_RATIO = 20.0;
	static constexpr double MIN_ATTACK_MS = 0.05;
	static constexpr double MIN_RELEASE_MS = 1.0;
	static constexpr double DEFAULT_RMS_MS = 10.0;

	// Thread-safe parameter storage (written from main thread, read from audio thread).
	struct Params {
		std::atomic<float> threshold_db{-18.0f};
		std::atomic<float> ratio{4.0f};
		std::atomic<float> attack_ms{10.0f};
		std::atomic<float> release_ms{120.0f};
		std::atomic<float> knee_db{6.0f};
		std::atomic<float> makeup_db{0.0f};
		std::atomic<float> mix{1.0f};
		std::atomic<float> detector_blend{0.65f};
	};

	Params _params;

	// RT-safe state reset (set from main thread, consumed on audio thread).
	std::atomic<bool> _reset_state_requested{false};

	// DSP state (audio-thread-owned, never touched from the main thread).
	double _rms_env = 0.0;
	double _gain_db = 0.0;
	double _attack_coeff = 0.0;
	double _release_coeff = 0.0;
	double _rms_coeff = 0.0;

	// Metering (written on the audio thread, readable from main thread).
	std::atomic<float> _current_gr_db{0.0f};

	static _FORCE_INLINE_ double _linear_to_db(double p_linear) {
		return 20.0 * std::log10(p_linear + EPSILON);
	}

	static _FORCE_INLINE_ double _db_to_linear(double p_db) {
		return std::pow(10.0, p_db / 20.0);
	}

	static _FORCE_INLINE_ double _compute_coeff(double p_time_ms, double p_sample_rate) {
		if (p_time_ms <= 0.0 || p_sample_rate <= 0.0) {
			return 0.0;
		}
		return std::exp(-1.0 / (p_time_ms * 0.001 * p_sample_rate));
	}

	static _FORCE_INLINE_ double _soft_knee_gr(double p_over_db, double p_knee_db, double p_ratio) {
		if (p_knee_db <= 0.0) {
			if (p_over_db <= 0.0) {
				return 0.0;
			}
			return -p_over_db * (1.0 - 1.0 / p_ratio);
		}

		double half_knee = p_knee_db * 0.5;
		if (p_over_db <= -half_knee) {
			return 0.0;
		} else if (p_over_db >= half_knee) {
			return -p_over_db * (1.0 - 1.0 / p_ratio);
		}

		double y = p_over_db + half_knee;
		return -(1.0 - 1.0 / p_ratio) * y * y / (2.0 * p_knee_db);
	}

protected:
	static void _bind_methods();

public:
	void set_params(double p_threshold_db = -18.0, double p_ratio = 4.0,
			double p_attack_ms = 10.0, double p_release_ms = 120.0,
			double p_knee_db = 6.0, double p_makeup_db = 0.0,
			double p_mix = 1.0, double p_detector_blend = 0.65);

	virtual int prepare_process() override;
	virtual int process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;
	virtual bool set_arg(int p_arg_index, double p_value) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual void reset() override;

	double get_gain_reduction_db() const { return _current_gr_db.load(std::memory_order_relaxed); }

	SiEffectCompressor(double p_threshold_db = -18.0, double p_ratio = 4.0,
			double p_attack_ms = 10.0, double p_release_ms = 120.0,
			double p_knee_db = 6.0, double p_makeup_db = 0.0,
			double p_mix = 1.0, double p_detector_blend = 0.65);
	~SiEffectCompressor() {}
};

#endif // SI_EFFECT_COMPRESSOR_H
