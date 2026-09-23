/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_FRACTIONAL_DELAY_H
#define SION_DSP_FRACTIONAL_DELAY_H

#include <algorithm>
#include <vector>
#include <godot_cpp/core/defs.hpp>

namespace sion::dsp {

// Delay line with linearly interpolated reads. The length is a power of two, so
// wrapping is a mask. prepare() allocates, so call it off the audio thread.
struct FractionalDelay {
	std::vector<double> buffer;
	int mask = 0;
	int write_index = 0;

	void prepare(double p_max_delay_samples) {
		buffer.assign(godot::next_power_of_2((unsigned int)p_max_delay_samples + 1), 0.0);
		mask = (int)buffer.size() - 1;
		write_index = 0;
	}

	void clear() {
		std::fill(buffer.begin(), buffer.end(), 0.0);
		write_index = 0;
	}

	// Delays run from 1 (the last written sample) to the prepared maximum.
	inline double read(double p_delay_samples) const {
		double position = (double)(write_index + mask + 1) - p_delay_samples;
		int index = (int)position;
		double a = buffer[index & mask];
		return a + (buffer[(index + 1) & mask] - a) * (position - (double)index);
	}

	inline void write(double p_sample) {
		buffer[write_index] = p_sample;
		write_index = (write_index + 1) & mask;
	}
};

} // namespace sion::dsp

#endif // SION_DSP_FRACTIONAL_DELAY_H
