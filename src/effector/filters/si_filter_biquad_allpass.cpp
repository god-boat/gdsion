/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_filter_biquad_allpass.h"

SiFilterBiquadAllpass::SiFilterBiquadAllpass(double p_delay_length, double p_gain, int p_sample_rate) :
		_delay(p_delay_length, p_sample_rate) {
	_delay_length = p_delay_length;
	_delay_offset = 0.0;
	_gain = p_gain;
}

double SiFilterBiquadAllpass::process(double p_input) {
	double sum2 = _delay.read(_delay_length - _delay_offset) + (-_gain * p_input);
	double sum1 = _gain * sum2 + p_input;
	_delay.write(sum1);
	return sum2;
}
