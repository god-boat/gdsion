/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_ONE_POLE_H
#define SION_DSP_ONE_POLE_H

#include <cmath>
#include <godot_cpp/core/math_defs.hpp>

namespace sion::dsp {

inline double one_pole_coeff(double p_cutoff_hz, double p_sample_rate) {
	return 1.0 - ::exp(-Math_TAU * p_cutoff_hz / p_sample_rate);
}

// One channel of a one-pole filter. highpass() is the exact complement of
// lowpass(), so the two bands of one input sum back to it.
struct OnePole {
	double z = 0.0;

	inline double lowpass(double p_coeff, double p_input) {
		z += p_coeff * (p_input - z);
		return z;
	}

	inline double highpass(double p_coeff, double p_input) {
		return p_input - lowpass(p_coeff, p_input);
	}
};

} // namespace sion::dsp

#endif // SION_DSP_ONE_POLE_H
