/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_METER_STATE_H
#define SION_METER_STATE_H

#include <godot_cpp/variant/dictionary.hpp>
#include <atomic>

using namespace godot;

// The highest peak since the reader last took it. The audio thread raises it on
// every metered block and the main thread takes it, so a peak that lands between
// two UI reads still reaches the meter.
struct HeldPeak {
	std::atomic<float> value{ 0.0f };

	void raise(float p_peak) {
		float held = value.load(std::memory_order_relaxed);
		while (p_peak > held && !value.compare_exchange_weak(held, p_peak, std::memory_order_relaxed)) {
		}
	}
	float take() { return value.exchange(0.0f, std::memory_order_relaxed); }
};

// One metered output. RMS describes the most recently metered block; peaks
// hold until taken. Every field is shared by the audio and main threads.
struct MeterState {
	std::atomic<float> rms_left{ 0.0f };
	std::atomic<float> rms_right{ 0.0f };
	HeldPeak peak_left;
	HeldPeak peak_right;

	void reset() {
		rms_left.store(0.0f, std::memory_order_relaxed);
		rms_right.store(0.0f, std::memory_order_relaxed);
		peak_left.value.store(0.0f, std::memory_order_relaxed);
		peak_right.value.store(0.0f, std::memory_order_relaxed);
	}

	Dictionary take() {
		Dictionary result;
		result["rms_left"] = rms_left.load(std::memory_order_relaxed);
		result["rms_right"] = rms_right.load(std::memory_order_relaxed);
		result["peak_left"] = peak_left.take();
		result["peak_right"] = peak_right.take();
		return result;
	}
};

#endif // SION_METER_STATE_H
