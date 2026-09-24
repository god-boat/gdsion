/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_STEREO_CHORUS_H
#define SI_EFFECT_STEREO_CHORUS_H

#include "dsp/fractional_delay.h"
#include "dsp/lfo.h"
#include "effector/si_effect_base.h"

class SiEffectStereoChorus : public SiEffectBase {
	GDCLASS(SiEffectStereoChorus, SiEffectBase)

	// The center delay is at most half of this, so the swing around it fits.
	static const int MAX_DELAY_SAMPLES = 4095;

	sion::dsp::FractionalDelay _delay_left;
	sion::dsp::FractionalDelay _delay_right;
	sion::dsp::Lfo _lfo;

	double _center_delay = 0;
	double _depth = 0;
	double _feedback = 0;
	double _dry_gain = 1.0;
	double _wet_gain = 0.0;
	double _phase_invert = -1.0;

	double _process_channel(sion::dsp::FractionalDelay &r_delay, double p_input, double p_delay_samples);

protected:
	static void _bind_methods();

public:
	void set_params(double p_delay_time = 20, double p_feedback = 0.2, double p_frequency = 4, double p_depth = 20, double p_wet = 0.5, bool p_invert_phase = true);

	//

	virtual int prepare_process() override;
	virtual int process(const ProcessContext &p_context, int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual void reset() override;

	SiEffectStereoChorus(double p_delay_time = 20, double p_feedback = 0.2, double p_frequency = 4, double p_depth = 20, double p_wet = 0.5, bool p_invert_phase = true);
	~SiEffectStereoChorus() {}
};

#endif // SI_EFFECT_STEREO_CHORUS_H
