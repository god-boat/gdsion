/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SIOPM_WARP_PROCESSOR_H
#define SIOPM_WARP_PROCESSOR_H

#include <cmath>
#include <cstdint>

#ifndef Math_PI
#define Math_PI 3.14159265358979323846
#endif

// Reusable granular overlap-add engine for audio warp modes (Tones / Texture).
//
// This processor is a plain C++ class (not a Godot Object) designed to be
// embedded by any audio channel type (SiOPMChannelStream, SiOPMChannelSampler,
// etc.) via composition. It encapsulates all granular synthesis state and
// algorithm logic, decoupled from the audio data source.
//
// Data source abstraction: the template method read_granular() accepts any
// callable with the signature `double(int offset, int channel)`, allowing
// zero-overhead adaptation to ring buffers, direct arrays, or any other
// sample storage.
//
// Thread safety: this class has no thread-safe members. It is designed to be
// used exclusively on the audio thread, matching the channel's buffer() call
// pattern.
class SiOPMWarpProcessor {
public:
	struct Grain {
		double read_pos = 0.0;     // Fractional read offset relative to ring/array read head.
		double window_pos = 0.0;   // Current position within the grain window [0, grain_len).
		int grain_len = 0;         // Length of this grain in output samples.
		bool active = false;
	};

private:
	Grain _grains[2];                 // Two alternating grains for overlap-add.
	int _grain_phase = 0;             // Output sample counter for scheduling new grains.
	double _grain_source_pos = 0.0;   // Accumulated source position (advances at BPM ratio or 1:1).
	uint32_t _rng_state = 1;          // Simple xorshift32 PRNG state for TEXTURE fluctuation.

	double _grain_size = 0.5;         // Grain size (0..1), maps to GRAIN_MIN..GRAIN_MAX samples.
	double _flux = 0.0;               // Fluctuation/randomness for TEXTURE mode (0..1).

	static constexpr int GRAIN_MIN_SAMPLES = 240;   // ~5 ms at 48 kHz
	static constexpr int GRAIN_MAX_SAMPLES = 4800;   // ~100 ms at 48 kHz

	// Inline xorshift32 PRNG (deterministic, no stdlib dependency).
	inline uint32_t _xorshift32() {
		uint32_t x = _rng_state;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		_rng_state = x;
		return x;
	}

public:
	// ---- State management ----

	void reset() {
		for (int i = 0; i < 2; i++) {
			_grains[i] = Grain();
		}
		_grain_phase = 0;
		_grain_source_pos = 0.0;
		_rng_state = 1;
		_grain_size = 0.5;
		_flux = 0.0;
	}

	// ---- Param setters / getters ----

	void set_grain_size(double p_size) {
		_grain_size = p_size < 0.0 ? 0.0 : (p_size > 1.0 ? 1.0 : p_size);
	}
	double get_grain_size() const { return _grain_size; }

	void set_flux(double p_flux) {
		_flux = p_flux < 0.0 ? 0.0 : (p_flux > 1.0 ? 1.0 : p_flux);
	}
	double get_flux() const { return _flux; }

	// ---- Core engine ----

	int compute_grain_length() const {
		return GRAIN_MIN_SAMPLES + (int)(_grain_size * (double)(GRAIN_MAX_SAMPLES - GRAIN_MIN_SAMPLES));
	}

	double get_source_pos() const { return _grain_source_pos; }
	void set_source_pos(double p_pos) { _grain_source_pos = p_pos; }

	int get_phase() const { return _grain_phase; }

	// Access grains for ring-buffer consumption adjustments.
	Grain &get_grain(int p_index) { return _grains[p_index]; }
	const Grain &get_grain(int p_index) const { return _grains[p_index]; }

	// Start a new grain at the given slot, reading from the current source position.
	// p_warp_mode: 3 = TONES, 4 = TEXTURE.
	void start_grain(int p_slot, double p_source_pos, double p_pitch_step, int p_warp_mode) {
		Grain &g = _grains[p_slot];
		g.grain_len = compute_grain_length();
		g.window_pos = 0.0;
		g.active = true;

		// In TEXTURE mode, apply fluctuation: randomize the read position.
		if (p_warp_mode == 4 /* TEXTURE */ && _flux > 0.0) {
			// Random offset in source frames: up to +/-(flux * grain_len * 2) frames.
			double rand_norm = ((double)(_xorshift32() & 0xFFFF) / 65535.0) * 2.0 - 1.0;
			double max_offset = _flux * (double)g.grain_len * 2.0;
			g.read_pos = p_source_pos + rand_norm * max_offset;
			if (g.read_pos < 0.0) {
				g.read_pos = 0.0;
			}
		} else {
			g.read_pos = p_source_pos;
		}
	}

	// Schedule a new grain if the phase counter has reached a hop boundary.
	// Call this once per output sample, before read_granular().
	void schedule_grain_if_needed(double p_source_pos, double p_pitch_step, int p_warp_mode) {
		int grain_len = compute_grain_length();
		int hop_size = grain_len / 2;
		if (hop_size < 1) {
			hop_size = 1;
		}
		if (_grain_phase % hop_size == 0) {
			int slot = (_grain_phase / hop_size) % 2;
			start_grain(slot, p_source_pos, p_pitch_step, p_warp_mode);
		}
	}

	// Advance the internal phase counter and source position.
	// Call this once per output sample, after read_granular().
	void advance(double p_source_advance) {
		_grain_source_pos += p_source_advance;
		_grain_phase++;
	}

	// ---- Granular read (template for zero-cost data-source abstraction) ----
	//
	// ReadFunc signature: double(int offset, int channel)
	//   - offset: sample offset relative to the current read head
	//   - channel: 0 = left/mono, 1 = right
	//
	// p_available: number of frames available ahead of the read head
	// p_channel:   which channel to read (0 or 1)
	// p_channels:  total channel count (1 or 2)
	// p_pitch_step: grain playback rate (controls pitch-shifting within grains)
	//
	// Returns the mixed, windowed sample value for the requested channel.
	// Grain state (read_pos, window_pos) is advanced only when p_channel
	// equals (p_channels - 1), so stereo calls (ch 0, then ch 1) don't
	// double-step the grain.
	template<typename ReadFunc>
	double read_granular(ReadFunc p_read_fn, int p_available,
			int p_channel, int p_channels, double p_pitch_step) {
		double mixed = 0.0;

		for (int s = 0; s < 2; s++) {
			Grain &g = _grains[s];
			if (!g.active || g.grain_len <= 0) {
				continue;
			}

			// Raised-cosine (Hann) window for smooth overlap-add.
			double phase = g.window_pos / (double)g.grain_len;
			double window = 0.5 - 0.5 * std::cos(2.0 * Math_PI * phase);

			// Read from source with linear interpolation.
			int base_idx = (int)g.read_pos;
			double frac = g.read_pos - base_idx;

			if (base_idx >= 0 && base_idx + 1 < p_available) {
				double s0 = p_read_fn(base_idx, p_channel);
				double s1 = p_read_fn(base_idx + 1, p_channel);
				mixed += (s0 + (s1 - s0) * frac) * window;
			}

			// Advance grain state only on the last channel to avoid
			// double-stepping for stereo.
			if (p_channel == p_channels - 1) {
				g.read_pos += p_pitch_step;
				g.window_pos += 1.0;

				if (g.window_pos >= (double)g.grain_len) {
					g.active = false;
				}
			}
		}

		return mixed;
	}

	// Adjust grain read positions when the data source's read head advances.
	// Call after consuming frames from a ring buffer or advancing the base
	// index in an array, to keep grain positions relative to the new head.
	void adjust_positions(int p_frames_consumed) {
		_grain_source_pos -= p_frames_consumed;
		for (int s = 0; s < 2; s++) {
			if (_grains[s].active) {
				_grains[s].read_pos -= p_frames_consumed;
			}
		}
	}
};

#endif // SIOPM_WARP_PROCESSOR_H
