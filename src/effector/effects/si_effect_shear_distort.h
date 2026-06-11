/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_SHEAR_DISTORT_H
#define SI_EFFECT_SHEAR_DISTORT_H

#include "effector/si_effect_base.h"

// Stereo distortion with body-preserving saturation, shape morphing,
// tone control, and animated stereo circuit drift ("shear").
//
// Parameters (all normalized 0-1 except bias -1..+1):
//   0  Drive  – input gain into the nonlinear stage
//   1  Shape  – morphs between soft tanh, asymmetric, hard clip, wavefold
//   2  Bias   – DC offset before shaper (removed afterward)
//   3  Tone   – pre-emphasis + post-distortion lowpass macro
//   4  Body   – low-frequency protection / clean bass blend
//   5  Width  – stereo side-channel drive (0=mono, 0.5=normal, 1=wide)
//   6  Shear  – animated L/R drive+bias mismatch for stereo instability
//   7  Mix    – dry/wet blend

class SiEffectShearDistort : public SiEffectBase {
	GDCLASS(SiEffectShearDistort, SiEffectBase)

public:
	static const int NUM_PARAMS = 8;

private:
	static const double BODY_CROSSOVER_HZ;
	static const double PRE_EMPH_CENTER_HZ;
	static const double SHEAR_LFO_RATE_1;
	static const double SHEAR_LFO_RATE_2;

	struct OnePoleLPF {
		double z = 0.0;
		double coeff = 0.0;

		void set_cutoff(double p_hz, double p_sample_rate);
		double process(double p_input);
		void clear();
		void flush_denormals();
	};

	struct DcBlocker {
		double x1 = 0.0;
		double y1 = 0.0;

		double process(double p_input);
		void clear();
		void flush_denormals();
	};

	struct ChannelState {
		OnePoleLPF body_lpf;
		OnePoleLPF pre_emph_lpf;
		OnePoleLPF tone_lpf;
		DcBlocker dc;

		void clear();
		void flush_denormals();
	};

	double _p_drive = 0.35;
	double _p_shape = 0.35;
	double _p_bias = 0.0;
	double _p_tone = 0.55;
	double _p_body = 0.45;
	double _p_width = 0.5;
	double _p_shear = 0.1;
	double _p_mix = 1.0;

	double _drive_linear = 1.0;
	double _side_drive = 1.0;
	double _pre_emph_amount = 0.0;

	ChannelState _left;
	ChannelState _right;

	double _shear_phase = 0.0;
	double _shear_phase2 = 0.0;

	bool _initialized = false;

	void _update_derived();
	void _update_filters();

	static double _soft_tanh(double p_x);
	static double _asymmetric(double p_x);
	static double _hard_clip(double p_x);
	static double _fold(double p_x);
	static double _shape_sample(double p_input, double p_shape);

protected:
	static void _bind_methods();

public:
	void set_params(double p_drive = 0.35, double p_shape = 0.35, double p_bias = 0.0,
			double p_tone = 0.55, double p_body = 0.45, double p_width = 0.5,
			double p_shear = 0.1, double p_mix = 1.0);

	virtual int prepare_process() override;
	virtual int process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;
	virtual void set_by_mml(Vector<double> p_args) override;
	virtual bool set_arg(int p_arg_index, double p_value) override;
	virtual void reset() override;

	SiEffectShearDistort();
	~SiEffectShearDistort() {}
};

#endif // SI_EFFECT_SHEAR_DISTORT_H
