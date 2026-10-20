/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "spectrum_analyzer.h"

#include <godot_cpp/core/class_db.hpp>
#include <algorithm>
#include <cmath>

void SpectrumAnalyzer::configure(double p_sample_rate, double p_freq_min, double p_freq_max, int p_column_count) {
	ERR_FAIL_COND_MSG(p_column_count < 2, "SpectrumAnalyzer: A spectrum needs at least two columns.");

	const int nyquist_bin = WINDOW_SIZE / 2;
	const double bins_per_hz = WINDOW_SIZE / p_sample_rate;
	// Each column reaches half a step to either side of its center, in log frequency.
	const double step = std::log(p_freq_max / p_freq_min) / (p_column_count - 1);
	const double half_step = std::exp(0.5 * step);

	_columns.resize(p_column_count);
	for (int c = 0; c < p_column_count; c++) {
		const double center = std::min(p_freq_min * std::exp(step * c) * bins_per_hz, (double)nyquist_bin);
		Column &column = _columns[c];
		column.first_bin = (int)std::ceil(center / half_step);
		column.last_bin = std::min((int)std::floor(center * half_step), nyquist_bin);
		column.interpolate = column.last_bin < column.first_bin;
		if (column.interpolate) {
			column.first_bin = std::min((int)center, nyquist_bin - 1);
			column.fraction = center - column.first_bin;
		}
	}

	_levels.resize(p_column_count);
	_levels.fill((float)FLOOR_DB);
}

PackedFloat32Array SpectrumAnalyzer::analyze(const PackedFloat32Array &p_frames, double p_delta_seconds) {
	ERR_FAIL_COND_V_MSG(p_frames.size() != WINDOW_SIZE, _levels,
			vformat("SpectrumAnalyzer: Expected %d frames, got %d.", WINDOW_SIZE, p_frames.size()));

	const float *frames = p_frames.ptr();
	for (int i = 0; i < WINDOW_SIZE; i++) {
		_windowed[i] = frames[i] * _window[i];
	}
	_fft.forward(_windowed.data(), _real.data(), _imag.data());
	for (int k = 0; k <= WINDOW_SIZE / 2; k++) {
		_power[k] = (_real[k] * _real[k] + _imag[k] * _imag[k]) * _power_scale;
	}

	const double attack = 1.0 - std::exp(-p_delta_seconds / ATTACK_SECONDS);
	const double release = 1.0 - std::exp(-p_delta_seconds / RELEASE_SECONDS);
	float *levels = _levels.ptrw();
	for (int c = 0; c < (int)_columns.size(); c++) {
		const Column &column = _columns[c];
		double power;
		if (column.interpolate) {
			const double below = _power[column.first_bin];
			power = below + (_power[column.first_bin + 1] - below) * column.fraction;
		} else {
			power = *std::max_element(_power.begin() + column.first_bin, _power.begin() + column.last_bin + 1);
		}
		const double level = 10.0 * std::log10(std::max(power, FLOOR_POWER));
		const double held = levels[c];
		levels[c] = (float)(held + (level - held) * (level > held ? attack : release));
	}
	return _levels;
}

void SpectrumAnalyzer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure", "sample_rate", "freq_min", "freq_max", "column_count"), &SpectrumAnalyzer::configure);
	ClassDB::bind_method(D_METHOD("analyze", "frames", "delta_seconds"), &SpectrumAnalyzer::analyze);

	BIND_CONSTANT(WINDOW_SIZE);
}

SpectrumAnalyzer::SpectrumAnalyzer() {
	_fft.prepare(WINDOW_SIZE);
	// Periodic Hann. A sine of amplitude A peaks at A times half the window's sum.
	_window.resize(WINDOW_SIZE);
	double window_sum = 0.0;
	for (int i = 0; i < WINDOW_SIZE; i++) {
		_window[i] = 0.5 - 0.5 * std::cos(Math_TAU * i / WINDOW_SIZE);
		window_sum += _window[i];
	}
	_power_scale = 4.0 / (window_sum * window_sum);
	_windowed.resize(WINDOW_SIZE);
	_real.resize(WINDOW_SIZE / 2 + 1);
	_imag.resize(WINDOW_SIZE / 2 + 1);
	_power.resize(WINDOW_SIZE / 2 + 1);
}
