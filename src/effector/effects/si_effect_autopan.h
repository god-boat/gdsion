/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_AUTOPAN_H
#define SI_EFFECT_AUTOPAN_H

#include "effector/si_effect_base.h"

class SiEffectAutopan : public SiEffectBase {
	GDCLASS(SiEffectAutopan, SiEffectBase)

	double _lfo_cos = 1;
	double _lfo_sin = 0;
	double _step_cos = 1;
	double _step_sin = 0;
	double _offset_cos = -1;
	double _offset_sin = 0;

protected:
	static void _bind_methods();

public:
	void set_params(double p_frequency = 1, double p_stereo_width = 1);

	//

	virtual int prepare_process() override;
	virtual int process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual void reset() override;

	SiEffectAutopan(double p_frequency = 1, double p_stereo_width = 1);
	~SiEffectAutopan() {}
};

#endif // SI_EFFECT_AUTOPAN_H
