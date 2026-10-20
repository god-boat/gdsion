/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_FFT_H
#define SION_DSP_FFT_H

#include <cmath>
#include <vector>
#include <godot_cpp/core/math_defs.hpp>

namespace sion::dsp {

// Forward FFT of a real signal whose length is a power of two, at least 8.
// The signal is packed into a complex sequence of half its length, so a size-N
// transform is one N/2-point complex FFT plus an O(N) split into N/2 + 1 bins.
// The complex FFT runs radix-4 passes, with one radix-2 pass when the stage
// count is odd. prepare() allocates and builds every table, so call it off the
// audio thread. forward() allocates nothing and calls no trigonometry.
struct RealFft {
	void prepare(int p_size) {
		_size = p_size;
		const int half = p_size / 2;

		// Packing writes each complex sample to its bit-reversed index, so the
		// passes run in place with no separate permutation.
		_bit_reverse.resize(half);
		int bits = 0;
		while ((1 << bits) < half) {
			bits++;
		}
		for (int n = 0; n < half; n++) {
			int reversed = 0;
			for (int bit = 0; bit < bits; bit++) {
				reversed |= ((n >> bit) & 1) << (bits - 1 - bit);
			}
			_bit_reverse[n] = reversed;
		}

		// The first radix-4 pass has trivial twiddles. Each later pass over spans
		// of 4h reads, for j < h, the powers 1 to 3 of e^(-iπj/2h) as six
		// contiguous rows of h: real then imaginary, power by power. A final
		// radix-2 pass over spans of 2h reads e^(-iπj/h) as two rows of h.
		_twiddles.clear();
		int h = 4;
		for (; 2 * h < half; h *= 4) {
			for (int power = 1; power <= 3; power++) {
				for (int j = 0; j < h; j++) {
					_twiddles.push_back(std::cos(Math_PI * power * j / (2 * h)));
				}
				for (int j = 0; j < h; j++) {
					_twiddles.push_back(-std::sin(Math_PI * power * j / (2 * h)));
				}
			}
		}
		if (h < half) {
			for (int j = 0; j < h; j++) {
				_twiddles.push_back(std::cos(Math_PI * j / h));
			}
			for (int j = 0; j < h; j++) {
				_twiddles.push_back(-std::sin(Math_PI * j / h));
			}
		}

		_split_cos.resize(half);
		_split_sin.resize(half);
		for (int k = 0; k < half; k++) {
			_split_cos[k] = std::cos(Math_TAU * k / p_size);
			_split_sin[k] = std::sin(Math_TAU * k / p_size);
		}

		_real.assign(half, 0.0);
		_imag.assign(half, 0.0);
	}

	int get_size() const { return _size; }

	// Writes bins 0 to size/2, DC and Nyquist included: size/2 + 1 real and
	// imaginary parts. Unnormalized, so a unit sine centered on a bin has
	// magnitude size/2 there.
	void forward(const double *p_signal, double *r_real, double *r_imag) {
		const int half = _size / 2;
		double *re = _real.data();
		double *im = _imag.data();

		const int *reverse = _bit_reverse.data();
		for (int n = 0; n < half; n++) {
			re[reverse[n]] = p_signal[2 * n];
			im[reverse[n]] = p_signal[2 * n + 1];
		}

		// Spans of 4 with twiddles 1 and -i.
		for (int s = 0; s < half; s += 4) {
			const double b0r = re[s] + re[s + 1];
			const double b0i = im[s] + im[s + 1];
			const double b1r = re[s] - re[s + 1];
			const double b1i = im[s] - im[s + 1];
			const double b2r = re[s + 2] + re[s + 3];
			const double b2i = im[s + 2] + im[s + 3];
			const double b3r = re[s + 2] - re[s + 3];
			const double b3i = im[s + 2] - im[s + 3];
			re[s] = b0r + b2r;
			im[s] = b0i + b2i;
			re[s + 2] = b0r - b2r;
			im[s + 2] = b0i - b2i;
			re[s + 1] = b1r + b3i;
			im[s + 1] = b1i - b3r;
			re[s + 3] = b1r - b3i;
			im[s + 3] = b1i + b3r;
		}

		const double *twiddle = _twiddles.data();
		int h = 4;
		for (; 2 * h < half; h *= 4) {
			const double *v1r = twiddle;
			const double *v1i = twiddle + h;
			const double *v2r = twiddle + 2 * h;
			const double *v2i = twiddle + 3 * h;
			const double *v3r = twiddle + 4 * h;
			const double *v3i = twiddle + 5 * h;
			for (int s = 0; s < half; s += 4 * h) {
				// The four quarters never overlap, which lets the compiler
				// vectorize without an aliasing check per span.
				double *__restrict r0 = re + s;
				double *__restrict i0 = im + s;
				double *__restrict r1 = r0 + h;
				double *__restrict i1 = i0 + h;
				double *__restrict r2 = r0 + 2 * h;
				double *__restrict i2 = i0 + 2 * h;
				double *__restrict r3 = r0 + 3 * h;
				double *__restrict i3 = i0 + 3 * h;
				for (int j = 0; j < h; j++) {
					const double t1r = r1[j] * v2r[j] - i1[j] * v2i[j];
					const double t1i = r1[j] * v2i[j] + i1[j] * v2r[j];
					const double t2r = r2[j] * v1r[j] - i2[j] * v1i[j];
					const double t2i = r2[j] * v1i[j] + i2[j] * v1r[j];
					const double t3r = r3[j] * v3r[j] - i3[j] * v3i[j];
					const double t3i = r3[j] * v3i[j] + i3[j] * v3r[j];
					const double c0r = r0[j] + t1r;
					const double c0i = i0[j] + t1i;
					const double c1r = r0[j] - t1r;
					const double c1i = i0[j] - t1i;
					const double c2r = t2r + t3r;
					const double c2i = t2i + t3i;
					const double c3r = t2r - t3r;
					const double c3i = t2i - t3i;
					r0[j] = c0r + c2r;
					i0[j] = c0i + c2i;
					r2[j] = c0r - c2r;
					i2[j] = c0i - c2i;
					r1[j] = c1r + c3i;
					i1[j] = c1i - c3r;
					r3[j] = c1r - c3i;
					i3[j] = c1i + c3r;
				}
			}
			twiddle += 6 * h;
		}
		if (h < half) {
			const double *wr = twiddle;
			const double *wi = twiddle + h;
			double *__restrict a_re = re;
			double *__restrict a_im = im;
			double *__restrict b_re = re + h;
			double *__restrict b_im = im + h;
			for (int j = 0; j < h; j++) {
				const double t_re = b_re[j] * wr[j] - b_im[j] * wi[j];
				const double t_im = b_re[j] * wi[j] + b_im[j] * wr[j];
				b_re[j] = a_re[j] - t_re;
				b_im[j] = a_im[j] - t_im;
				a_re[j] += t_re;
				a_im[j] += t_im;
			}
		}

		// Bin k mixes the even samples' spectrum E and the odd samples' O:
		// X[k] = E[k] + e^(-2πik/N) O[k], both recovered from Z[k] and Z[N/2 - k].
		r_real[0] = re[0] + im[0];
		r_imag[0] = 0.0;
		r_real[half] = re[0] - im[0];
		r_imag[half] = 0.0;
		for (int k = 1; k < half; k++) {
			const double even_re = (re[k] + re[half - k]) * 0.5;
			const double even_im = (im[k] - im[half - k]) * 0.5;
			const double odd_re = (im[k] + im[half - k]) * 0.5;
			const double odd_im = (re[half - k] - re[k]) * 0.5;
			r_real[k] = even_re + _split_cos[k] * odd_re + _split_sin[k] * odd_im;
			r_imag[k] = even_im + _split_cos[k] * odd_im - _split_sin[k] * odd_re;
		}
	}

private:
	int _size = 0;
	std::vector<int> _bit_reverse;
	std::vector<double> _twiddles;
	std::vector<double> _split_cos;
	std::vector<double> _split_sin;
	std::vector<double> _real;
	std::vector<double> _imag;
};

} // namespace sion::dsp

#endif // SION_DSP_FFT_H
