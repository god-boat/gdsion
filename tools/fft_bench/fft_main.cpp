// Checks sion::dsp::RealFft against a direct DFT and times it against a
// textbook complex radix-2 FFT of the full length. Built by build.py.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "dsp/fft.h"

using sion::dsp::RealFft;

static int failures = 0;

static void check(bool p_ok, const char *p_label, double p_value) {
	std::printf("%s %-44s %.3e\n", p_ok ? "PASS" : "FAIL", p_label, p_value);
	if (!p_ok) {
		failures++;
	}
}

// Direct DFT in long double: the reference.
static void dft(const std::vector<double> &p_signal, std::vector<std::complex<long double>> &r_bins) {
	const int n = (int)p_signal.size();
	r_bins.assign(n / 2 + 1, 0.0L);
	const long double tau = 6.283185307179586476925286766559L;
	for (int k = 0; k <= n / 2; k++) {
		std::complex<long double> sum = 0.0L;
		for (int t = 0; t < n; t++) {
			const long double angle = -tau * (long double)((long long)k * t % n) / n;
			sum += (long double)p_signal[t] * std::complex<long double>(std::cos(angle), std::sin(angle));
		}
		r_bins[k] = sum;
	}
}

// The usual radix-2 complex FFT over the whole real signal, for the timing baseline.
struct TextbookFft {
	int size = 0;
	std::vector<int> reverse;
	std::vector<std::complex<double>> twiddles;
	std::vector<std::complex<double>> work;

	void prepare(int p_size) {
		size = p_size;
		int bits = 0;
		while ((1 << bits) < p_size) {
			bits++;
		}
		reverse.resize(p_size);
		for (int i = 0; i < p_size; i++) {
			int r = 0;
			for (int b = 0; b < bits; b++) {
				r |= ((i >> b) & 1) << (bits - 1 - b);
			}
			reverse[i] = r;
		}
		twiddles.resize(p_size / 2);
		for (int k = 0; k < p_size / 2; k++) {
			twiddles[k] = std::polar(1.0, -6.283185307179586 * k / p_size);
		}
		work.resize(p_size);
	}

	void forward(const double *p_signal) {
		for (int i = 0; i < size; i++) {
			work[reverse[i]] = p_signal[i];
		}
		for (int span = 2; span <= size; span <<= 1) {
			const int half = span / 2;
			const int stride = size / span;
			for (int s = 0; s < size; s += span) {
				for (int j = 0; j < half; j++) {
					const std::complex<double> t = twiddles[j * stride] * work[s + j + half];
					work[s + j + half] = work[s + j] - t;
					work[s + j] += t;
				}
			}
		}
	}
};

static double now_ns() {
	return (double)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
	std::mt19937_64 rng(7);
	std::normal_distribution<double> noise(0.0, 1.0);

	// Accuracy: the worst bin error over the transform's RMS bin magnitude.
	for (int size = 8; size <= 4096; size <<= 1) {
		std::vector<double> signal(size);
		for (double &sample : signal) {
			sample = noise(rng);
		}
		RealFft fft;
		fft.prepare(size);
		std::vector<double> real(size / 2 + 1), imag(size / 2 + 1);
		fft.forward(signal.data(), real.data(), imag.data());
		std::vector<std::complex<long double>> reference;
		dft(signal, reference);
		long double worst = 0.0L;
		for (int k = 0; k <= size / 2; k++) {
			worst = std::max(worst, std::abs(std::complex<long double>(real[k], imag[k]) - reference[k]));
		}
		const double relative = (double)(worst / std::sqrt((long double)size));
		char label[64];
		std::snprintf(label, sizeof(label), "N=%d matches the direct DFT", size);
		check(relative < 1e-13, label, relative);
	}

	// A unit sine centered on bin 100 has magnitude N/2 there and nothing elsewhere.
	{
		const int size = 4096;
		std::vector<double> signal(size);
		for (int t = 0; t < size; t++) {
			signal[t] = std::sin(6.283185307179586 * 100.0 * t / size);
		}
		RealFft fft;
		fft.prepare(size);
		std::vector<double> real(size / 2 + 1), imag(size / 2 + 1);
		fft.forward(signal.data(), real.data(), imag.data());
		const double peak = std::hypot(real[100], imag[100]);
		check(std::abs(peak - size / 2) < 1e-9, "unit sine on bin 100 has magnitude N/2", std::abs(peak - size / 2));
		double leak = 0.0;
		for (int k = 0; k <= size / 2; k++) {
			if (k != 100) {
				leak = std::max(leak, std::hypot(real[k], imag[k]));
			}
		}
		check(leak < 1e-9, "no other bin of a bin-centered sine", leak);
	}

	// Timing: median of repeated batches, real FFT against the full complex FFT.
	for (int size : { 1024, 4096, 8192 }) {
		std::vector<double> signal(size);
		for (double &sample : signal) {
			sample = noise(rng);
		}
		RealFft fft;
		fft.prepare(size);
		TextbookFft textbook;
		textbook.prepare(size);
		std::vector<double> real(size / 2 + 1), imag(size / 2 + 1);
		const int batch = 2000;
		std::vector<double> real_times, textbook_times;
		volatile double sink = 0.0;
		for (int round = 0; round < 15; round++) {
			double start = now_ns();
			for (int i = 0; i < batch; i++) {
				fft.forward(signal.data(), real.data(), imag.data());
				sink = sink + real[1];
			}
			real_times.push_back((now_ns() - start) / batch);
			start = now_ns();
			for (int i = 0; i < batch; i++) {
				textbook.forward(signal.data());
				sink = sink + textbook.work[1].real();
			}
			textbook_times.push_back((now_ns() - start) / batch);
		}
		std::sort(real_times.begin(), real_times.end());
		std::sort(textbook_times.begin(), textbook_times.end());
		std::printf("TIME N=%-5d RealFft %8.2f us   textbook complex %8.2f us   (%.1fx)\n",
				size, real_times[7] / 1000.0, textbook_times[7] / 1000.0, textbook_times[7] / real_times[7]);
	}

	std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
	return failures == 0 ? 0 : 1;
}
