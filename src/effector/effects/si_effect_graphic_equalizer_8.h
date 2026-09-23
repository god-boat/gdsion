/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_GRAPHIC_EQUALIZER_8_H
#define SI_EFFECT_GRAPHIC_EQUALIZER_8_H

#include "dsp/biquad.h"
#include "effector/si_effect_base.h"

class SiEffectGraphicEqualizer8 : public SiEffectBase {
	GDCLASS(SiEffectGraphicEqualizer8, SiEffectBase)

public:
	static const int NUM_BANDS = 8;
	static const int PARAMS_PER_BAND = 5;

	enum FilterType {
		FILTER_PEAK = sion::dsp::BIQUAD_PEAK,
		FILTER_LOW_PASS = sion::dsp::BIQUAD_LOW_PASS,
		FILTER_HIGH_PASS = sion::dsp::BIQUAD_HIGH_PASS,
		FILTER_BAND_PASS = sion::dsp::BIQUAD_BAND_PASS,
		FILTER_NOTCH = sion::dsp::BIQUAD_NOTCH,
		FILTER_LOW_SHELF = sion::dsp::BIQUAD_LOW_SHELF,
		FILTER_HIGH_SHELF = sion::dsp::BIQUAD_HIGH_SHELF,
		FILTER_ALL_PASS = sion::dsp::BIQUAD_ALL_PASS,
		FILTER_TYPE_MAX = sion::dsp::BIQUAD_TYPE_MAX,
	};

private:
	static const double DENORMAL_THRESHOLD;
	static const double DEFAULT_FREQS[NUM_BANDS];

	struct Band {
		int type = FILTER_PEAK;
		bool enabled = true;
		double freq_hz = 1000.0;
		double gain_db = 0.0;
		double q = 1.0;

		sion::dsp::BiquadCoeffs current;
		sion::dsp::BiquadCoeffs target;
		sion::dsp::BiquadCoeffs step;
		bool dirty = false;

		sion::dsp::BiquadState left;
		sion::dsp::BiquadState right;

		void clear_state() {
			left = {};
			right = {};
			dirty = false;
			step = sion::dsp::BiquadCoeffs{ 0, 0, 0, 0, 0 };
		}
	};

	static _FORCE_INLINE_ void _flush_denormals(sion::dsp::BiquadState &r_state) {
		if (::fabs(r_state.y1) < DENORMAL_THRESHOLD) {
			r_state = {};
		}
	}

	Band _bands[NUM_BANDS];
	double _output_gain = 1.0;
	double _target_output_gain = 1.0;
	double _output_gain_step = 0.0;
	bool _output_gain_dirty = false;
	bool _initialized = false;

	void _recompute_band(int p_band);
	void _apply_band_params(int p_band, int p_type, bool p_enabled, double p_freq_hz, double p_gain_db, double p_q);
	void _snap_all();

protected:
	static void _bind_methods();

public:
	void set_band_params(int p_band, int p_type, bool p_enabled, double p_freq_hz, double p_gain_db, double p_q);
	void set_output_gain_db(double p_gain_db);

	virtual int prepare_process() override;
	virtual int process(const ProcessContext &p_context, int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual bool set_arg(int p_arg_index, double p_value) override;
	virtual void reset() override;

	SiEffectGraphicEqualizer8();
	~SiEffectGraphicEqualizer8() {}
};

VARIANT_ENUM_CAST(SiEffectGraphicEqualizer8::FilterType);

#endif // SI_EFFECT_GRAPHIC_EQUALIZER_8_H
