/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef BIQUAD_COEFFICIENTS_H
#define BIQUAD_COEFFICIENTS_H

#include <cmath>
#include <godot_cpp/core/defs.hpp>

enum BiquadFilterType {
	BIQUAD_PEAK = 0,
	BIQUAD_LOW_PASS = 1,
	BIQUAD_HIGH_PASS = 2,
	BIQUAD_BAND_PASS = 3,
	BIQUAD_NOTCH = 4,
	BIQUAD_LOW_SHELF = 5,
	BIQUAD_HIGH_SHELF = 6,
	BIQUAD_ALL_PASS = 7,
	BIQUAD_TYPE_MAX = 8,
};

struct BiquadCoeffs {
	double b0 = 1.0, b1 = 0.0, b2 = 0.0;
	double a1 = 0.0, a2 = 0.0;

	inline bool approx_equal(const BiquadCoeffs &p_other, double p_epsilon = 1e-12) const {
		return ::fabs(b0 - p_other.b0) <= p_epsilon &&
				::fabs(b1 - p_other.b1) <= p_epsilon &&
				::fabs(b2 - p_other.b2) <= p_epsilon &&
				::fabs(a1 - p_other.a1) <= p_epsilon &&
				::fabs(a2 - p_other.a2) <= p_epsilon;
	}
};

// Compute normalized biquad coefficients for all supported filter types.
// Uses Audio EQ Cookbook formulas (Robert Bristow-Johnson) with Q-based alpha.
// Outputs are normalized (divided by a0): y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
static inline BiquadCoeffs compute_biquad_coefficients(int p_type, double p_freq_hz, double p_q, double p_gain_db, double p_sample_rate) {
	BiquadCoeffs c;

	double nyquist = p_sample_rate * 0.5;
	double freq = CLAMP(p_freq_hz, 10.0, nyquist * 0.99);
	double q = MAX(p_q, 0.01);

	double omega = 2.0 * M_PI * freq / p_sample_rate;
	double sin_w = ::sin(omega);
	double cos_w = ::cos(omega);
	double alpha = sin_w / (2.0 * q);

	double a0 = 1.0;
	double raw_b0 = 1.0, raw_b1 = 0.0, raw_b2 = 0.0;
	double raw_a1 = 0.0, raw_a2 = 0.0;

	switch (p_type) {
		case BIQUAD_PEAK: {
			double A = ::pow(10.0, p_gain_db / 40.0);
			raw_b0 = 1.0 + alpha * A;
			raw_b1 = -2.0 * cos_w;
			raw_b2 = 1.0 - alpha * A;
			a0     = 1.0 + alpha / A;
			raw_a1 = -2.0 * cos_w;
			raw_a2 = 1.0 - alpha / A;
		} break;

		case BIQUAD_LOW_PASS: {
			raw_b0 = (1.0 - cos_w) * 0.5;
			raw_b1 = 1.0 - cos_w;
			raw_b2 = (1.0 - cos_w) * 0.5;
			a0     = 1.0 + alpha;
			raw_a1 = -2.0 * cos_w;
			raw_a2 = 1.0 - alpha;
		} break;

		case BIQUAD_HIGH_PASS: {
			raw_b0 = (1.0 + cos_w) * 0.5;
			raw_b1 = -(1.0 + cos_w);
			raw_b2 = (1.0 + cos_w) * 0.5;
			a0     = 1.0 + alpha;
			raw_a1 = -2.0 * cos_w;
			raw_a2 = 1.0 - alpha;
		} break;

		case BIQUAD_BAND_PASS: {
			raw_b0 = alpha;
			raw_b1 = 0.0;
			raw_b2 = -alpha;
			a0     = 1.0 + alpha;
			raw_a1 = -2.0 * cos_w;
			raw_a2 = 1.0 - alpha;
		} break;

		case BIQUAD_NOTCH: {
			raw_b0 = 1.0;
			raw_b1 = -2.0 * cos_w;
			raw_b2 = 1.0;
			a0     = 1.0 + alpha;
			raw_a1 = -2.0 * cos_w;
			raw_a2 = 1.0 - alpha;
		} break;

		case BIQUAD_LOW_SHELF: {
			double A = ::pow(10.0, p_gain_db / 40.0);
			double two_sqrt_A_alpha = 2.0 * ::sqrt(A) * alpha;
			raw_b0 = A * ((A + 1.0) - (A - 1.0) * cos_w + two_sqrt_A_alpha);
			raw_b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w);
			raw_b2 = A * ((A + 1.0) - (A - 1.0) * cos_w - two_sqrt_A_alpha);
			a0     = (A + 1.0) + (A - 1.0) * cos_w + two_sqrt_A_alpha;
			raw_a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w);
			raw_a2 = (A + 1.0) + (A - 1.0) * cos_w - two_sqrt_A_alpha;
		} break;

		case BIQUAD_HIGH_SHELF: {
			double A = ::pow(10.0, p_gain_db / 40.0);
			double two_sqrt_A_alpha = 2.0 * ::sqrt(A) * alpha;
			raw_b0 = A * ((A + 1.0) + (A - 1.0) * cos_w + two_sqrt_A_alpha);
			raw_b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w);
			raw_b2 = A * ((A + 1.0) + (A - 1.0) * cos_w - two_sqrt_A_alpha);
			a0     = (A + 1.0) - (A - 1.0) * cos_w + two_sqrt_A_alpha;
			raw_a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w);
			raw_a2 = (A + 1.0) - (A - 1.0) * cos_w - two_sqrt_A_alpha;
		} break;

		case BIQUAD_ALL_PASS: {
			raw_b0 = 1.0 - alpha;
			raw_b1 = -2.0 * cos_w;
			raw_b2 = 1.0 + alpha;
			a0     = 1.0 + alpha;
			raw_a1 = -2.0 * cos_w;
			raw_a2 = 1.0 - alpha;
		} break;

		default: {
			return c;
		}
	}

	if (::fabs(a0) < 1e-30) {
		a0 = 1e-30;
	}
	double inv_a0 = 1.0 / a0;
	c.b0 = raw_b0 * inv_a0;
	c.b1 = raw_b1 * inv_a0;
	c.b2 = raw_b2 * inv_a0;
	c.a1 = raw_a1 * inv_a0;
	c.a2 = raw_a2 * inv_a0;

	return c;
}

#endif // BIQUAD_COEFFICIENTS_H
