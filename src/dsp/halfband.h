/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_HALFBAND_H
#define SION_DSP_HALFBAND_H

namespace sion::dsp {

// A two-path polyphase allpass halfband: each path is a chain of first-order
// allpass sections running at the low rate.
struct HalfbandCoeffs {
	static constexpr int MAX_SECTIONS = 4;
	double a[MAX_SECTIONS] = {};
	double b[MAX_SECTIONS] = {};
	int sections = 0;
};

// Classic public-domain sets. STEEP rejects ~69 dB with a 0.01*fs transition
// band and protects the audible range at the 1x<->2x boundary. LIGHT rejects
// ~70 dB with a relaxed 0.1*fs transition, plenty for the 2x<->4x octave where
// the signal only occupies the bottom quarter of the spectrum.
inline constexpr HalfbandCoeffs HALFBAND_STEEP = {
	{ 0.07711507983241622, 0.4820706250610472, 0.7968204713315797, 0.9412514277740471 },
	{ 0.2659685265210946, 0.6651041532634957, 0.8841015085506159, 0.9820054141886075 },
	4,
};
inline constexpr HalfbandCoeffs HALFBAND_LIGHT = {
	{ 0.07986642623635751, 0.5453536510711322 },
	{ 0.28382934487410993, 0.8344118914807379 },
	2,
};

// One 2x resampling stage. Upsample: 1 in -> 2 out; downsample: 2 in -> 1 out.
struct Halfband {
	double xa[HalfbandCoeffs::MAX_SECTIONS] = {};
	double ya[HalfbandCoeffs::MAX_SECTIONS] = {};
	double xb[HalfbandCoeffs::MAX_SECTIONS] = {};
	double yb[HalfbandCoeffs::MAX_SECTIONS] = {};

	inline void upsample(const HalfbandCoeffs &p_coeffs, double p_input, double &r_out0, double &r_out1) {
		r_out0 = _run_path(p_coeffs.a, xa, ya, p_coeffs.sections, p_input);
		r_out1 = _run_path(p_coeffs.b, xb, yb, p_coeffs.sections, p_input);
	}

	inline double downsample(const HalfbandCoeffs &p_coeffs, double p_input0, double p_input1) {
		return 0.5 * (_run_path(p_coeffs.a, xa, ya, p_coeffs.sections, p_input1) + _run_path(p_coeffs.b, xb, yb, p_coeffs.sections, p_input0));
	}

	static inline double _run_path(const double *p_coeffs, double *r_x, double *r_y, int p_sections, double p_input) {
		double value = p_input;
		for (int i = 0; i < p_sections; i++) {
			double y = r_x[i] + p_coeffs[i] * (value - r_y[i]);
			r_x[i] = value;
			r_y[i] = y;
			value = y;
		}
		return value;
	}
};

} // namespace sion::dsp

#endif // SION_DSP_HALFBAND_H
