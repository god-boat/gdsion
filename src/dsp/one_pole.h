/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_ONE_POLE_H
#define SION_DSP_ONE_POLE_H

#include <cmath>
#include <complex>
#include <godot_cpp/core/math_defs.hpp>

namespace sion::dsp {

inline double one_pole_coeff(double p_cutoff_hz, double p_sample_rate) {
	return 1.0 - ::exp(-Math_TAU * p_cutoff_hz / p_sample_rate);
}

// The responses below let a feedback loop tune itself: it subtracts each
// filter's phase delay from its period and counts the magnitude as loss.

// Phase delay in samples of the one-pole lowpass y += c (x - y) at p_w radians per sample.
inline double one_pole_phase_delay(double p_coeff, double p_w) {
	const double pole = 1.0 - p_coeff;
	return std::atan2(pole * std::sin(p_w), 1.0 - pole * std::cos(p_w)) / p_w;
}

inline double one_pole_magnitude(double p_coeff, double p_w) {
	const double pole = 1.0 - p_coeff;
	const double re = 1.0 - pole * std::cos(p_w);
	const double im = pole * std::sin(p_w);
	return p_coeff / std::sqrt(re * re + im * im);
}

// The one-pole coefficient whose magnitude at p_w is p_magnitude; brighter coefficients pass more.
inline double one_pole_coeff_for_magnitude(double p_magnitude, double p_w) {
	const double b = 1.0 - p_magnitude * p_magnitude;
	const double a = 1.0 - p_magnitude * p_magnitude * std::cos(p_w);
	return 1.0 - b / (a + std::sqrt(a * a - b * b));
}

// The lowpass's response where z^-1 is p_z_inverse: std::polar(1, -w) on the unit
// circle, or std::polar(1 / r, -w) at a pole of radius r. The highpass's is one minus it.
inline std::complex<double> one_pole_response(double p_coeff, std::complex<double> p_z_inverse) {
	return p_coeff / (1.0 - (1.0 - p_coeff) * p_z_inverse);
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
