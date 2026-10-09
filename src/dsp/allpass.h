/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_ALLPASS_H
#define SION_DSP_ALLPASS_H

#include <cmath>
#include <complex>

namespace sion::dsp {

// One channel of the first-order allpass (a + z^-1) / (1 + a z^-1). A negative
// coefficient delays low frequencies more than high ones, so a chain of them in
// a string or drum loop runs the upper partials sharp, like stiffness.
struct Allpass {
	double x1 = 0.0;
	double y1 = 0.0;

	inline double tick(double p_coeff, double p_input) {
		const double output = p_coeff * p_input + x1 - p_coeff * y1;
		x1 = p_input;
		y1 = output;
		return output;
	}
};

// Phase delay in samples at p_w radians per sample.
inline double allpass_phase_delay(double p_coeff, double p_w) {
	const double phase = std::atan2(-std::sin(p_w), p_coeff + std::cos(p_w)) - std::atan2(-p_coeff * std::sin(p_w), 1.0 + p_coeff * std::cos(p_w));
	return -phase / p_w;
}

// The response where z^-1 is p_z_inverse.
inline std::complex<double> allpass_response(double p_coeff, std::complex<double> p_z_inverse) {
	return (p_coeff + p_z_inverse) / (1.0 + p_coeff * p_z_inverse);
}

} // namespace sion::dsp

#endif // SION_DSP_ALLPASS_H
