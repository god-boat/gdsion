/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_FAST_TANH_H
#define SION_DSP_FAST_TANH_H

namespace sion::dsp {

// Pade approximation of tanh; exact ±1 with zero slope at |x| = 3, so the
// curve is C1-continuous and its harmonics roll off fast.
inline double fast_tanh(double p_x) {
	if (p_x >= 3.0) {
		return 1.0;
	}
	if (p_x <= -3.0) {
		return -1.0;
	}
	double x2 = p_x * p_x;
	return p_x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

} // namespace sion::dsp

#endif // SION_DSP_FAST_TANH_H
