/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_LINKWITZ_RILEY_H
#define SION_DSP_LINKWITZ_RILEY_H

#include "dsp/biquad.h"

namespace sion::dsp {

// Fourth-order Linkwitz-Riley crossover. Each band is two cascaded Butterworth
// biquads (the cookbook response at Q = 1/sqrt(2)), so the low and high bands
// sum to an allpass of the input.
struct LinkwitzRiley4Coeffs {
	BiquadCoeffs low_pass;
	BiquadCoeffs high_pass;

	void set_cutoff(double p_cutoff_hz, double p_sample_rate) {
		low_pass = compute_biquad_coefficients(BIQUAD_LOW_PASS, p_cutoff_hz, Math_SQRT12, 0.0, p_sample_rate);
		high_pass = compute_biquad_coefficients(BIQUAD_HIGH_PASS, p_cutoff_hz, Math_SQRT12, 0.0, p_sample_rate);
	}
};

// One channel of a crossover.
struct LinkwitzRiley4 {
	BiquadState low[2];
	BiquadState high[2];

	inline void split(const LinkwitzRiley4Coeffs &p_coeffs, double p_input, double &r_low, double &r_high) {
		r_low = low[1].tick(p_coeffs.low_pass, low[0].tick(p_coeffs.low_pass, p_input));
		r_high = high[1].tick(p_coeffs.high_pass, high[0].tick(p_coeffs.high_pass, p_input));
	}
};

} // namespace sion::dsp

#endif // SION_DSP_LINKWITZ_RILEY_H
