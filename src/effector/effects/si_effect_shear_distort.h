/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_SHEAR_DISTORT_H
#define SI_EFFECT_SHEAR_DISTORT_H

#include "effector/si_effect_base.h"

// Stereo "shear" distortion.
//
// Wet path, per channel:
//   width (M/S) -> body crossover -> pre-emphasis tilt -> drive + bias ->
//   4x oversampled morphing waveshaper -> auto makeup -> DC block ->
//   de-emphasis tilt -> tone lowpass -> + protected low band
//
// Design notes:
//   - The shaper runs 4x oversampled through polyphase IIR halfbands and all
//     curves are C1-continuous, which keeps aliasing (the main source of
//     digital harshness) low.
//   - Wet level is auto-compensated against drive/shape/bias so it tracks the
//     dry level instead of slamming full scale as drive goes up; this also
//     keeps the protected low band balanced against the distorted band.
//   - Lows below the body crossover can bypass the shaper (Body), keeping the
//     bottom end full instead of letting saturation flatten it.
//   - All audible parameters are smoothed (~5 ms) to avoid zipper noise.
//
// Parameters (normalized 0..1 except bias -1..+1):
//   0  Drive  – gain into the shaper, 0..+48 dB on a perceptual curve
//   1  Shape  – morph: warm -> tube (even harmonics) -> clip -> fold
//   2  Bias   – shifts the shaper operating point (gated/sputtery fuzz)
//   3  Tone   – tilt into the shaper + post lowpass macro (dark..open)
//   4  Body   – 0: lows fully distorted, 1: lows bypass the shaper cleanly
//   5  Width  – stereo side drive (0=mono, 0.5=unity, 1=wide)
//   6  Shear  – slow animated L/R drive+bias mismatch
//   7  Mix    – dry/wet blend

class SiEffectShearDistort : public SiEffectBase {
	GDCLASS(SiEffectShearDistort, SiEffectBase)

public:
	static const int NUM_PARAMS = 8;

private:
	static const double BODY_CROSSOVER_HZ;
	static const double TILT_PIVOT_HZ;
	static const double DC_BLOCK_HZ;
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
		double r = 0.9993;
		double x1 = 0.0;
		double y1 = 0.0;

		void set_cutoff(double p_hz, double p_sample_rate);
		double process(double p_input);
		void clear();
		void flush_denormals();
	};

	// First-order allpass section of a two-path polyphase halfband filter.
	struct AllpassSection {
		double c = 0.0;
		double x1 = 0.0;
		double y1 = 0.0;

		double process(double p_input);
		void clear();
		void flush_denormals();
	};

	// One 2x resampling stage. Upsample: 1 in -> 2 out; downsample: 2 in -> 1 out.
	struct HalfbandStage {
		static const int MAX_SECTIONS = 4;

		AllpassSection path_a[MAX_SECTIONS];
		AllpassSection path_b[MAX_SECTIONS];
		int section_count = 0;

		void setup(const double *p_coeffs_a, const double *p_coeffs_b, int p_count);
		double _run_path(AllpassSection *p_path, double p_input);
		void upsample(double p_input, double &r_out0, double &r_out1);
		double downsample(double p_input0, double p_input1);
		void clear();
		void flush_denormals();
	};

	// Zavalishin TPT state-variable lowpass; stable under cutoff modulation,
	// so the smoothed tone cutoff can move every sample.
	struct ToneSVF {
		double ic1 = 0.0;
		double ic2 = 0.0;

		double process_lowpass(double p_input, double p_g);
		void clear();
		void flush_denormals();
	};

	struct SmoothedParam {
		double current = 0.0;
		double target = 0.0;

		void set_target(double p_value) { target = p_value; }
		void snap() { current = target; }
		double tick(double p_coeff) {
			current += (target - current) * p_coeff;
			return current;
		}
		void flush_denormals();
	};

	struct ChannelState {
		OnePoleLPF body_lpf;
		OnePoleLPF tilt_pre_lpf;
		OnePoleLPF tilt_post_lpf;
		DcBlocker dc;
		ToneSVF tone_svf;
		HalfbandStage up_steep;
		HalfbandStage up_light;
		HalfbandStage down_light;
		HalfbandStage down_steep;

		void setup();
		void clear();
		void flush_denormals();
	};

	double _p_drive = 0.4;
	double _p_shape = 0.25;
	double _p_bias = 0.0;
	double _p_tone = 0.6;
	double _p_body = 0.5;
	double _p_width = 0.5;
	double _p_shear = 0.15;
	double _p_mix = 1.0;

	SmoothedParam _sm_drive;
	SmoothedParam _sm_bias;
	SmoothedParam _sm_makeup;
	SmoothedParam _sm_side;
	SmoothedParam _sm_body;
	SmoothedParam _sm_shape;
	SmoothedParam _sm_tilt_pre;
	SmoothedParam _sm_tilt_post;
	SmoothedParam _sm_tone_g;
	SmoothedParam _sm_mix;
	double _smooth_coeff = 0.005;

	ChannelState _left;
	ChannelState _right;

	double _shear_phase = 0.0;
	double _shear_phase2 = 0.0;

	void _update_derived();
	void _update_filters();
	void _snap_smoothers();

	static double _fast_tanh(double p_x);
	static double _curve_warm(double p_x);
	static double _curve_tube(double p_x);
	static double _curve_clip(double p_x);
	static double _curve_fold(double p_x);
	static double _shape_sample(double p_input, double p_shape);

	double _process_channel(ChannelState &p_ch, double p_input, double p_drive, double p_bias,
			double p_makeup, double p_body, double p_shape,
			double p_tilt_pre, double p_tilt_post, double p_tone_g);

protected:
	static void _bind_methods();

public:
	void set_params(double p_drive = 0.4, double p_shape = 0.25, double p_bias = 0.0,
			double p_tone = 0.6, double p_body = 0.5, double p_width = 0.5,
			double p_shear = 0.15, double p_mix = 1.0);

	virtual int prepare_process() override;
	virtual int process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;
	virtual void set_by_mml(Vector<double> p_args) override;
	virtual bool set_arg(int p_arg_index, double p_value) override;
	virtual void reset() override;

	SiEffectShearDistort();
	~SiEffectShearDistort() {}
};

#endif // SI_EFFECT_SHEAR_DISTORT_H
