/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_LINKWITZ_RILEY_FILTER_H
#define SI_EFFECT_LINKWITZ_RILEY_FILTER_H

#include "dsp/linkwitz_riley.h"
#include "effector/si_effect_base.h"

using namespace godot;

// Passes one band of a fourth-order Linkwitz-Riley crossover.
class SiEffectLinkwitzRileyFilter : public SiEffectBase {
	GDCLASS(SiEffectLinkwitzRileyFilter, SiEffectBase)

	double _cutoff = 1000.0;
	// Output mode: 0 = low band, 1 = high band
	int _output_mode = 0;

	sion::dsp::LinkwitzRiley4Coeffs _coeffs;
	sion::dsp::LinkwitzRiley4 _left;
	sion::dsp::LinkwitzRiley4 _right;

protected:
	static void _bind_methods();

public:
	void set_params(double p_cutoff_frequency = 1000.0, int p_output_mode = 0);

	virtual int prepare_process() override;
	virtual int process(const ProcessContext &p_context, int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual void reset() override;

	SiEffectLinkwitzRileyFilter();
	~SiEffectLinkwitzRileyFilter() {}
};

#endif // SI_EFFECT_LINKWITZ_RILEY_FILTER_H
