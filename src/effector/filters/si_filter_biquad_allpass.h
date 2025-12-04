/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_FILTER_BIQUAD_ALLPASS_H
#define SI_FILTER_BIQUAD_ALLPASS_H

#include "effector/components/si_effect_line_delay.h"

// An allpass filter component for use in effects (e.g., reverb).
// This is a building block class, not a standalone effect.
class SiFilterBiquadAllpass {

	SiEffectLineDelay _delay;
	double _delay_length = 0.0;
	double _delay_offset = 0.0;
	double _gain = 0.0;

public:
	double process(double p_input);

	double get_delay_length() const { return _delay_length; }
	void set_delay_length(double p_delay_length) { _delay_length = p_delay_length; }

	double get_delay_offset() const { return _delay_offset; }
	void set_delay_offset(double p_delay_offset) { _delay_offset = p_delay_offset; }

	double get_gain() const { return _gain; }
	void set_gain(double p_gain) { _gain = p_gain; }

	void reset() { _delay.reset(); }

	SiFilterBiquadAllpass(double p_delay_length, double p_gain, int p_sample_rate);
	~SiFilterBiquadAllpass() {}
};

#endif // SI_FILTER_BIQUAD_ALLPASS_H
