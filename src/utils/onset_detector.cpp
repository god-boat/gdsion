/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "onset_detector.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <cmath>
#include <algorithm>

#include "chip/wave/siopm_wave_base.h"

using namespace godot;

void OnsetDetector::_bind_methods() {
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("detect_onsets", "wave_data", "channel_count", "sensitivity", "sample_rate"),
		&OnsetDetector::detect_onsets, DEFVAL(2), DEFVAL(50), DEFVAL(48000));
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("detect_onsets_from_stream", "stream", "sensitivity"),
		&OnsetDetector::detect_onsets_from_stream, DEFVAL(50));
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("estimate_bpm", "wave_data", "channel_count", "sample_rate"),
		&OnsetDetector::estimate_bpm, DEFVAL(2), DEFVAL(48000));
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("estimate_bpm_from_stream", "stream"),
		&OnsetDetector::estimate_bpm_from_stream);
}

PackedInt32Array OnsetDetector::detect_onsets(
	const PackedFloat32Array &p_wave_data,
	int p_channel_count,
	int p_sensitivity,
	int p_sample_rate
) {
	// Convert PackedFloat32Array to Vector<double>.
	Vector<double> wave_data;
	wave_data.resize(p_wave_data.size());
	for (int i = 0; i < p_wave_data.size(); i++) {
		wave_data.write[i] = static_cast<double>(p_wave_data[i]);
	}

	Vector<int> onsets = detect_onsets_internal(wave_data, p_channel_count, p_sensitivity, p_sample_rate);

	// Convert Vector<int> to PackedInt32Array for GDScript.
	PackedInt32Array result;
	result.resize(onsets.size());
	for (int i = 0; i < onsets.size(); i++) {
		result.set(i, onsets[i]);
	}

	return result;
}

PackedInt32Array OnsetDetector::detect_onsets_from_stream(
	const Ref<AudioStream> &p_stream,
	int p_sensitivity
) {
	PackedInt32Array result;
	if (p_stream.is_null()) {
		return result;
	}

	int channel_count = 2;
	Vector<double> wave_data = SiOPMWaveBase::extract_wave_data(p_stream, &channel_count);
	if (wave_data.is_empty() || channel_count < 1) {
		return result;
	}

	int sample_rate = 48000;
	if (p_stream->has_method("get_mix_rate")) {
		Variant v_rate = p_stream->call("get_mix_rate");
		if (v_rate.get_type() == Variant::INT) {
			sample_rate = (int)v_rate;
		} else if (v_rate.get_type() == Variant::FLOAT) {
			sample_rate = (int)((double)v_rate);
		}
	}

	Vector<int> onsets = detect_onsets_internal(wave_data, channel_count, p_sensitivity, sample_rate);

	result.resize(onsets.size());
	for (int i = 0; i < onsets.size(); i++) {
		result.set(i, onsets[i]);
	}

	return result;
}

Vector<int> OnsetDetector::detect_onsets_internal(
	const Vector<double> &p_wave_data,
	int p_channel_count,
	int p_sensitivity,
	int p_sample_rate
) {
	Vector<int> onsets;

	if (p_wave_data.is_empty() || p_channel_count < 1) {
		return onsets;
	}

	// Clamp sensitivity to valid range and convert from 1-100 to 0.0-1.0.
	int clamped_sensitivity = CLAMP(p_sensitivity, 1, 100);
	double sensitivity = (clamped_sensitivity - 1) / 99.0;

	// Calculate frame count (samples per channel).
	int frame_count = p_wave_data.size() / p_channel_count;
	if (frame_count < DEFAULT_WINDOW_SIZE * 2) {
		// Audio too short for meaningful analysis.
		return onsets;
	}

	// Scale window and hop size based on sample rate.
	double rate_scale = static_cast<double>(p_sample_rate) / 48000.0;
	int window_size = static_cast<int>(DEFAULT_WINDOW_SIZE * rate_scale);
	int hop_size = static_cast<int>(DEFAULT_HOP_SIZE * rate_scale);
	int min_onset_interval = static_cast<int>(DEFAULT_MIN_ONSET_INTERVAL * rate_scale);

	// Ensure minimum sizes.
	window_size = MAX(window_size, 64);
	hop_size = MAX(hop_size, 32);
	min_onset_interval = MAX(min_onset_interval, hop_size * 2);

	// Step 1: Compute RMS envelope.
	int num_windows = (frame_count - window_size) / hop_size + 1;
	if (num_windows < 2) {
		return onsets;
	}

	Vector<double> envelope;
	envelope.resize_zeroed(num_windows);

	for (int w = 0; w < num_windows; w++) {
		int start_frame = w * hop_size;
		double sum_sq = 0.0;

		for (int f = 0; f < window_size; f++) {
			int frame_idx = start_frame + f;
			if (frame_idx >= frame_count) {
				break;
			}

			// Mix channels to mono for analysis.
			double sample = 0.0;
			for (int ch = 0; ch < p_channel_count; ch++) {
				sample += p_wave_data[frame_idx * p_channel_count + ch];
			}
			sample /= p_channel_count;

			sum_sq += sample * sample;
		}

		envelope.write[w] = std::sqrt(sum_sq / window_size);
	}

	// Step 2: Calculate onset strength (positive first derivative).
	Vector<double> onset_strength;
	onset_strength.resize_zeroed(num_windows);

	for (int w = 1; w < num_windows; w++) {
		double diff = envelope[w] - envelope[w - 1];
		onset_strength.write[w] = MAX(0.0, diff);
	}

	// Step 3: Compute adaptive threshold.
	// Find median and max of onset strength signal.
	Vector<double> sorted_strength = onset_strength;
	sorted_strength.sort();

	double median = 0.0;
	double max_strength = 0.0;

	if (num_windows > 0) {
		median = sorted_strength[num_windows / 2];
		max_strength = sorted_strength[num_windows - 1];
	}

	// Threshold: lower sensitivity = higher threshold = fewer detections.
	// Map sensitivity [0,1] -> threshold multiplier [1.0, 0.1].
	double threshold_mult = 1.0 - sensitivity * 0.9;
	double threshold = median + threshold_mult * (max_strength - median);

	// Ensure threshold is reasonable.
	threshold = MAX(threshold, max_strength * 0.05);

	// Step 4: Peak-pick above threshold.
	Vector<int> candidate_onsets;

	for (int w = 1; w < num_windows - 1; w++) {
		if (onset_strength[w] >= threshold) {
			// Check if local maximum.
			if (onset_strength[w] >= onset_strength[w - 1] &&
				onset_strength[w] >= onset_strength[w + 1]) {
				// Convert window index to sample index.
				int sample_idx = w * hop_size;
				candidate_onsets.push_back(sample_idx);
			}
		}
	}

	// Step 5: Filter by minimum inter-onset interval.
	int last_onset = -min_onset_interval;
	for (int i = 0; i < candidate_onsets.size(); i++) {
		int onset = candidate_onsets[i];
		if (onset - last_onset >= min_onset_interval) {
			onsets.push_back(onset);
			last_onset = onset;
		}
	}

	return onsets;
}

// ---------------------------------------------------------------------------
// BPM estimation — onset-IOI histogram approach
// ---------------------------------------------------------------------------

// Internal: estimate BPM from onset sample indices and sample rate.
static double _estimate_bpm_from_onsets(const Vector<int> &p_onsets, int p_sample_rate) {
	// Need at least 3 onsets (2 IOIs) for meaningful estimation.
	if (p_onsets.size() < 3 || p_sample_rate <= 0) {
		return 0.0;
	}

	// BPM range for histogram buckets.
	static constexpr int BPM_MIN = 60;
	static constexpr int BPM_MAX = 200;
	static constexpr int BPM_BUCKETS = BPM_MAX - BPM_MIN + 1;

	// Build a histogram of IOIs quantized to BPM buckets.
	Vector<int> histogram;
	histogram.resize_zeroed(BPM_BUCKETS);

	int valid_iois = 0;

	for (int i = 1; i < p_onsets.size(); i++) {
		int ioi_samples = p_onsets[i] - p_onsets[i - 1];
		if (ioi_samples <= 0) {
			continue;
		}

		// Convert IOI to BPM: BPM = 60 * sample_rate / ioi_samples.
		double bpm = 60.0 * (double)p_sample_rate / (double)ioi_samples;

		// Try the raw BPM and octave-related multiples (half, double) to catch
		// rhythmic subdivisions (e.g. 8th notes at 120 BPM yield 240 BPM IOIs,
		// but halving gives the correct 120).
		double candidates[3] = { bpm, bpm * 0.5, bpm * 2.0 };

		for (int c = 0; c < 3; c++) {
			int rounded = (int)std::round(candidates[c]);
			if (rounded >= BPM_MIN && rounded <= BPM_MAX) {
				histogram.write[rounded - BPM_MIN]++;
				valid_iois++;
			}
		}
	}

	if (valid_iois < 2) {
		return 0.0;
	}

	// Find peak bucket.
	int peak_index = 0;
	int peak_count = 0;
	for (int i = 0; i < BPM_BUCKETS; i++) {
		if (histogram[i] > peak_count) {
			peak_count = histogram[i];
			peak_index = i;
		}
	}

	double estimated_bpm = (double)(peak_index + BPM_MIN);

	// Octave correction: bring into 60-180 range.
	while (estimated_bpm > 180.0) {
		estimated_bpm *= 0.5;
	}
	while (estimated_bpm < 60.0) {
		estimated_bpm *= 2.0;
	}

	// Round to one decimal place for cleanliness.
	estimated_bpm = std::round(estimated_bpm * 10.0) / 10.0;

	return estimated_bpm;
}

double OnsetDetector::estimate_bpm(
	const PackedFloat32Array &p_wave_data,
	int p_channel_count,
	int p_sample_rate
) {
	// Convert PackedFloat32Array to Vector<double>.
	Vector<double> wave_data;
	wave_data.resize(p_wave_data.size());
	for (int i = 0; i < p_wave_data.size(); i++) {
		wave_data.write[i] = static_cast<double>(p_wave_data[i]);
	}

	// Use moderate sensitivity (50) for BPM estimation — we want clear hits.
	Vector<int> onsets = detect_onsets_internal(wave_data, p_channel_count, 50, p_sample_rate);

	return _estimate_bpm_from_onsets(onsets, p_sample_rate);
}

double OnsetDetector::estimate_bpm_from_stream(const Ref<AudioStream> &p_stream) {
	if (p_stream.is_null()) {
		return 0.0;
	}

	int channel_count = 2;
	Vector<double> wave_data = SiOPMWaveBase::extract_wave_data(p_stream, &channel_count);
	if (wave_data.is_empty() || channel_count < 1) {
		return 0.0;
	}

	int sample_rate = 48000;
	if (p_stream->has_method("get_mix_rate")) {
		Variant v_rate = p_stream->call("get_mix_rate");
		if (v_rate.get_type() == Variant::INT) {
			sample_rate = (int)v_rate;
		} else if (v_rate.get_type() == Variant::FLOAT) {
			sample_rate = (int)((double)v_rate);
		}
	}

	// Use moderate sensitivity (50) for BPM estimation.
	Vector<int> onsets = detect_onsets_internal(wave_data, channel_count, 50, sample_rate);

	return _estimate_bpm_from_onsets(onsets, sample_rate);
}
