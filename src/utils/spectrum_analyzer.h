/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_SPECTRUM_ANALYZER_H
#define SION_SPECTRUM_ANALYZER_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <vector>
#include "dsp/fft.h"

using namespace godot;

// Turns the newest frames of a signal into a level per column of a spectrum
// display with a logarithmic frequency axis. Column centers run from freq_min
// to freq_max inclusive, so they line up with a curve sampled at both ends.
// Levels are dBFS, where a full-scale sine reads 0, and rise and fall with
// display ballistics. Main thread: configure() allocates.
class SpectrumAnalyzer : public RefCounted {
	GDCLASS(SpectrumAnalyzer, RefCounted)

public:
	static constexpr int WINDOW_SIZE = 4096;

private:
	static constexpr double ATTACK_SECONDS = 0.02;
	static constexpr double RELEASE_SECONDS = 0.25;
	static constexpr double FLOOR_DB = -150.0;
	static constexpr double FLOOR_POWER = 1e-15;

	// A column narrower than one bin interpolates the two bins around its
	// center. A wider one takes the loudest bin it covers.
	struct Column {
		int first_bin = 0;
		int last_bin = 0;
		double fraction = 0.0;
		bool interpolate = false;
	};

	sion::dsp::RealFft _fft;
	std::vector<double> _window;
	std::vector<double> _windowed;
	std::vector<double> _real;
	std::vector<double> _imag;
	std::vector<double> _power;
	// Scales a bin's squared magnitude to the power of the sine it holds.
	double _power_scale = 0.0;
	std::vector<Column> _columns;
	PackedFloat32Array _levels;

protected:
	static void _bind_methods();

public:
	void configure(double p_sample_rate, double p_freq_min, double p_freq_max, int p_column_count);
	// Takes the newest WINDOW_SIZE frames, oldest first, and the time since the
	// previous call. Returns the level of every column.
	PackedFloat32Array analyze(const PackedFloat32Array &p_frames, double p_delta_seconds);

	SpectrumAnalyzer();
	~SpectrumAnalyzer() {}
};

#endif // SION_SPECTRUM_ANALYZER_H
