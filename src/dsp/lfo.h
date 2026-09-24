/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_LFO_H
#define SION_DSP_LFO_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <godot_cpp/core/math_defs.hpp>

namespace sion::dsp {

// Each jump in a shape is a linear ramp this long that ends at the jump, so an
// LFO driving gain never clicks. Every shape keeps its value at phase 0.
constexpr double LFO_EDGE_SECONDS = 0.002;

// The enum values are the LFO param values, so a setter clamps and casts.
enum class LfoShape {
	SINE, // Starts at the center and rises.
	TRIANGLE, // Starts at the center and rises.
	SAW_UP, // Starts at the bottom.
	SAW_DOWN, // Starts at the top.
	SQUARE, // Starts high.
	NOISE, // A new random value 256 times per cycle.
	SAMPLE_HOLD, // A new random value 16 times per cycle.
};

enum class LfoRateMode {
	HZ,
	MS,
	SYNCED,
	DOTTED, // 1.5 times the synced period.
	TRIPLET, // 2/3 of the synced period.
};

// Beat divisions run from a whole note (0) to a 1/32 note (5).
constexpr int BEAT_DIVISION_COUNT = 6;

inline double beat_division_beats(int p_division) {
	static constexpr double BEATS[BEAT_DIVISION_COUNT] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
	return BEATS[p_division];
}

// An LFO rate as its params store it. The mode picks the field that applies.
struct LfoRate {
	LfoRateMode mode = LfoRateMode::HZ;
	double hz = 1.0;
	double ms = 1000.0;
	int division = 2;

	double hz_at(double p_bpm) const {
		switch (mode) {
			case LfoRateMode::HZ:
				return hz;
			case LfoRateMode::MS:
				return 1000.0 / ms;
			case LfoRateMode::SYNCED:
				return p_bpm / (60.0 * beat_division_beats(division));
			case LfoRateMode::DOTTED:
				return p_bpm / (90.0 * beat_division_beats(division));
			default: // TRIPLET
				return p_bpm / (40.0 * beat_division_beats(division));
		}
	}
};

struct LfoSineTable {
	static constexpr int SIZE = 512;
	double values[SIZE + 1]; // The last point repeats the first, for interpolation.

	LfoSineTable() {
		for (int i = 0; i <= SIZE; i++) {
			values[i] = std::sin(Math_TAU * i / SIZE);
		}
	}
};

inline const LfoSineTable LFO_SINE_TABLE;

// A repeatable value from -1 to 1 for each step: a fixed seed hashed with the
// step index (lowbias32).
inline double lfo_random(uint32_t p_step) {
	uint32_t x = p_step ^ 0x9E3779B9u;
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x * (2.0 / 4294967296.0) - 1.0;
}

// A bipolar LFO. read() evaluates the shape and advance() moves one sample, so
// one phase can drive several reads, such as the two sides of a stereo LFO.
struct Lfo {
	double phase = 0.0; // From 0 to 1.
	double increment = 0.0; // Cycles per sample.
	double edge = 0.0; // The length of each jump's ramp, in cycles, up to half of one. At 0 the jumps are hard.
	uint32_t cycle = 0; // Whole cycles since reset(). The random shapes count steps from it.

	void reset(double p_phase = 0.0) {
		phase = p_phase;
		cycle = 0;
	}

	void set_hz(double p_hz, double p_sample_rate) {
		increment = p_hz / p_sample_rate;
		edge = std::min(LFO_EDGE_SECONDS * p_hz, 0.5);
	}

	inline void advance() {
		phase += increment;
		if (phase >= 1.0) {
			phase -= 1.0;
			cycle++;
		}
	}

	// The value p_offset cycles (0 to 1) ahead of the phase.
	template <LfoShape SHAPE>
	inline double read(double p_offset = 0.0) const {
		double p = phase + p_offset;
		const bool wrapped = p >= 1.0;
		if (wrapped) {
			p -= 1.0;
		}
		if constexpr (SHAPE == LfoShape::SINE) {
			const double x = p * LfoSineTable::SIZE;
			const int index = (int)x;
			const double a = LFO_SINE_TABLE.values[index];
			return a + (LFO_SINE_TABLE.values[index + 1] - a) * (x - index);
		} else if constexpr (SHAPE == LfoShape::TRIANGLE) {
			return p < 0.5 ? 1.0 - std::abs(4.0 * p - 1.0) : std::abs(4.0 * p - 3.0) - 1.0;
		} else if constexpr (SHAPE == LfoShape::SAW_UP) {
			if (edge == 0.0) {
				return 2.0 * p - 1.0;
			}
			// Rises until the last edge of the cycle, then ramps back down.
			return std::min(2.0 * p / (1.0 - edge) - 1.0, 2.0 * (1.0 - p) / edge - 1.0);
		} else if constexpr (SHAPE == LfoShape::SAW_DOWN) {
			return -read<LfoShape::SAW_UP>(p_offset);
		} else if constexpr (SHAPE == LfoShape::SQUARE) {
			if (edge == 0.0) {
				return p < 0.5 ? 1.0 : -1.0;
			}
			// Each half cycle holds its level, then ramps to the other over its last edge.
			const double level = p < 0.5 ? 1.0 : -1.0;
			const double half_phase = p < 0.5 ? p : p - 0.5;
			return level * std::min(1.0, (1.0 - 2.0 * half_phase) / edge - 1.0);
		} else if constexpr (SHAPE == LfoShape::NOISE) {
			return _read_random<256>(p, wrapped);
		} else {
			return _read_random<16>(p, wrapped);
		}
	}

	// A random value per step. Each step ramps to the next step's value over
	// its last edge, which is at most the whole step.
	template <uint32_t STEPS>
	inline double _read_random(double p_phase, bool p_wrapped) const {
		const double x = p_phase * STEPS;
		const uint32_t step_in_cycle = (uint32_t)x;
		const uint32_t step = (cycle + p_wrapped) * STEPS + step_in_cycle;
		const double value = lfo_random(step);
		if (edge == 0.0) {
			return value;
		}
		const double ramp = std::max(0.0, 1.0 - (1.0 - (x - step_in_cycle)) / std::min(edge * STEPS, 1.0));
		return value + (lfo_random(step + 1) - value) * ramp;
	}

	template <LfoShape SHAPE>
	inline double tick() {
		const double value = read<SHAPE>();
		advance();
		return value;
	}
};

template <LfoShape SHAPE>
using LfoShapeTag = std::integral_constant<LfoShape, SHAPE>;

// Calls p_fn with the shape as an LfoShapeTag, so the caller's sample loop is
// instantiated per shape and never branches on it.
template <typename F>
inline void with_lfo_shape(LfoShape p_shape, F &&p_fn) {
	switch (p_shape) {
		case LfoShape::SINE:
			return p_fn(LfoShapeTag<LfoShape::SINE>());
		case LfoShape::TRIANGLE:
			return p_fn(LfoShapeTag<LfoShape::TRIANGLE>());
		case LfoShape::SAW_UP:
			return p_fn(LfoShapeTag<LfoShape::SAW_UP>());
		case LfoShape::SAW_DOWN:
			return p_fn(LfoShapeTag<LfoShape::SAW_DOWN>());
		case LfoShape::SQUARE:
			return p_fn(LfoShapeTag<LfoShape::SQUARE>());
		case LfoShape::NOISE:
			return p_fn(LfoShapeTag<LfoShape::NOISE>());
		case LfoShape::SAMPLE_HOLD:
			return p_fn(LfoShapeTag<LfoShape::SAMPLE_HOLD>());
	}
}

} // namespace sion::dsp

#endif // SION_DSP_LFO_H
