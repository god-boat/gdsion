/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_MODAL_BANK_H
#define SION_DSP_MODAL_BANK_H

#include <cmath>

namespace sion::dsp {

// A bank of decaying sinusoids, each a complex one-pole: the "phasor filter" of
// Mathews and Smith (2003). A mode keeps its amplitude when its frequency
// changes, so pitch glides are clean, and it is stable for any radius below 1.
//
// Each sample a mode rotates by its frequency, shrinks by its radius and takes
// gain * input into its real part. The bank outputs the sum of the imaginary
// parts, so a struck mode starts at zero and rings as a sine.
template <int MODE_MAX>
struct ModalCoeffs {
	int count = 0;
	double w[MODE_MAX] = {}; // Radians per sample, before the pitch scale.
	double radius[MODE_MAX] = {}; // Amplitude kept per sample.
	double gain[MODE_MAX] = {}; // Input weight.
	double a[MODE_MAX] = {}; // radius * cos(w * scale)
	double b[MODE_MAX] = {}; // radius * sin(w * scale)

	// Two trig calls per mode, so callers that glide refresh it every few samples.
	void set_pitch_scale(double p_scale) {
		for (int k = 0; k < count; k++) {
			const double angle = w[k] * p_scale;
			a[k] = radius[k] * std::cos(angle);
			b[k] = radius[k] * std::sin(angle);
		}
	}
};

template <int MODE_MAX>
struct ModalState {
	double re[MODE_MAX] = {};
	double im[MODE_MAX] = {};

	inline double tick(const ModalCoeffs<MODE_MAX> &p_coeffs, double p_input) {
		double sum = 0.0;
		for (int k = 0; k < p_coeffs.count; k++) {
			const double re_k = re[k];
			const double im_k = im[k];
			re[k] = p_coeffs.a[k] * re_k - p_coeffs.b[k] * im_k + p_coeffs.gain[k] * p_input;
			im[k] = p_coeffs.b[k] * re_k + p_coeffs.a[k] * im_k;
			sum += im[k];
		}
		return sum;
	}

	// tick() with no input, for the long stretch after a strike.
	inline double tick_free(const ModalCoeffs<MODE_MAX> &p_coeffs) {
		double sum = 0.0;
		for (int k = 0; k < p_coeffs.count; k++) {
			const double re_k = re[k];
			const double im_k = im[k];
			re[k] = p_coeffs.a[k] * re_k - p_coeffs.b[k] * im_k;
			im[k] = p_coeffs.b[k] * re_k + p_coeffs.a[k] * im_k;
			sum += im[k];
		}
		return sum;
	}

	// The sum of every mode's squared amplitude.
	double energy(int p_count) const {
		double sum = 0.0;
		for (int k = 0; k < p_count; k++) {
			sum += re[k] * re[k] + im[k] * im[k];
		}
		return sum;
	}

	// Silences every mode from p_first up, such as modes a retune left out.
	void clear(int p_first = 0) {
		for (int k = p_first; k < MODE_MAX; k++) {
			re[k] = 0.0;
			im[k] = 0.0;
		}
	}
};

} // namespace sion::dsp

#endif // SION_DSP_MODAL_BANK_H
