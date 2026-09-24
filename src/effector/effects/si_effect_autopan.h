/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_AUTOPAN_H
#define SI_EFFECT_AUTOPAN_H

#include "dsp/lfo.h"
#include "dsp/linear_transition.h"
#include "effector/si_effect_base.h"

class SiEffectAutopan : public SiEffectBase {
	GDCLASS(SiEffectAutopan, SiEffectBase)

	sion::dsp::Lfo _lfo;
	sion::dsp::LfoRate _rate;
	sion::dsp::LfoShape _shape = sion::dsp::LfoShape::SINE;
	double _stereo_width = 1.0;
	double _width_offset = 0.0; // The right channel's phase offset, in cycles.
	double _resolved_hz = -1.0;

	bool _transition_pending = false;
	sion::dsp::LinearTransition _left_gain;
	sion::dsp::LinearTransition _right_gain;

	template <sion::dsp::LfoShape SHAPE>
	void _process(sion::dsp::LfoShapeTag<SHAPE>, double *r_buffer, int p_begin, int p_end);

protected:
	static void _bind_methods();

public:
	void set_params(double p_frequency = 1, double p_stereo_width = 1, int p_rate_mode = 0, int p_sync_division = 2, int p_shape = 0, double p_time = 1000);

	//

	virtual int prepare_process() override;
	virtual int process(const ProcessContext &p_context, int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual bool set_arg(int p_arg_index, double p_value) override;
	virtual void reset() override;

	SiEffectAutopan(double p_frequency = 1, double p_stereo_width = 1);
	~SiEffectAutopan() {}
};

#endif // SI_EFFECT_AUTOPAN_H
