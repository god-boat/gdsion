/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_GRAPHIC_EQUALIZER_8_H
#define SI_EFFECT_GRAPHIC_EQUALIZER_8_H

#include "effector/si_effect_base.h"
#include "effector/components/biquad_coefficients.h"

class SiEffectGraphicEqualizer8 : public SiEffectBase {
	GDCLASS(SiEffectGraphicEqualizer8, SiEffectBase)

public:
	static const int NUM_BANDS = 8;
	static const int PARAMS_PER_BAND = 5;

	enum FilterType {
		FILTER_PEAK = BIQUAD_PEAK,
		FILTER_LOW_PASS = BIQUAD_LOW_PASS,
		FILTER_HIGH_PASS = BIQUAD_HIGH_PASS,
		FILTER_BAND_PASS = BIQUAD_BAND_PASS,
		FILTER_NOTCH = BIQUAD_NOTCH,
		FILTER_LOW_SHELF = BIQUAD_LOW_SHELF,
		FILTER_HIGH_SHELF = BIQUAD_HIGH_SHELF,
		FILTER_ALL_PASS = BIQUAD_ALL_PASS,
		FILTER_TYPE_MAX = BIQUAD_TYPE_MAX,
	};

private:
	static const double DENORMAL_THRESHOLD;
	static const double DEFAULT_FREQS[NUM_BANDS];

	struct BandChannelState {
		double in1 = 0.0, in2 = 0.0;
		double out1 = 0.0, out2 = 0.0;

		_FORCE_INLINE_ void clear() {
			in1 = in2 = out1 = out2 = 0.0;
		}
		_FORCE_INLINE_ void flush_denormals() {
			if (::fabs(out1) < DENORMAL_THRESHOLD) {
				clear();
			}
		}
	};

	struct Band {
		int type = FILTER_PEAK;
		bool enabled = true;
		double freq_hz = 1000.0;
		double gain_db = 0.0;
		double q = 1.0;

		BiquadCoeffs current;
		BiquadCoeffs target;
		BiquadCoeffs step;
		bool dirty = false;

		BandChannelState left;
		BandChannelState right;

		void clear_state() {
			left.clear();
			right.clear();
			dirty = false;
			step = BiquadCoeffs{ 0, 0, 0, 0, 0 };
		}
	};

	Band _bands[NUM_BANDS];
	double _output_gain = 1.0;
	double _target_output_gain = 1.0;
	double _output_gain_step = 0.0;
	bool _output_gain_dirty = false;
	bool _initialized = false;

	void _recompute_band(int p_band);
	void _apply_band_params(int p_band, int p_type, bool p_enabled, double p_freq_hz, double p_gain_db, double p_q);
	void _snap_all();
	_FORCE_INLINE_ double _process_biquad(BandChannelState *p_state, const BiquadCoeffs &p_coeffs, double p_input) const;

protected:
	static void _bind_methods();

public:
	void set_band_params(int p_band, int p_type, bool p_enabled, double p_freq_hz, double p_gain_db, double p_q);
	void set_output_gain_db(double p_gain_db);

	virtual int prepare_process() override;
	virtual int process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual bool set_arg(int p_arg_index, double p_value) override;
	virtual void reset() override;

	SiEffectGraphicEqualizer8();
	~SiEffectGraphicEqualizer8() {}
};

VARIANT_ENUM_CAST(SiEffectGraphicEqualizer8::FilterType);

#endif // SI_EFFECT_GRAPHIC_EQUALIZER_8_H
