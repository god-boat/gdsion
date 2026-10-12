/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_DISTORTION_H
#define SI_EFFECT_DISTORTION_H

#include "dsp/biquad.h"
#include "effector/si_effect_base.h"

class SiEffectDistortion : public SiEffectBase {
	GDCLASS(SiEffectDistortion, SiEffectBase)

	static const double THRESHOLD;

	bool _filter_enabled = false;
	double _pre_scale = 0;
	double _limit = 0;

	sion::dsp::BiquadCoeffs _lpf_coeffs;
	sion::dsp::BiquadState _lpf_state[2];

protected:
	static void _bind_methods();

public:
	void set_params(double p_pre_gain = -60, double p_post_gain = 18, double p_lpf_frequency = 2400, double p_lpf_slope = 1);

	//

	virtual void process(const ProcessContext &p_context, Vector<double> *r_buffer, int p_start_index, int p_length) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual void reset() override;

	SiEffectDistortion(double p_pre_gain = -60, double p_post_gain = 18, double p_lpf_frequency = 2400, double p_lpf_slope = 1);
	~SiEffectDistortion() {}
};

#endif // SI_EFFECT_DISTORTION_H
