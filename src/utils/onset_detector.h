/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_ONSET_DETECTOR_H
#define SION_ONSET_DETECTOR_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>

using namespace godot;

// Energy-based onset (transient) detection utility for audio sample slicing.
// Detects sudden increases in audio energy that indicate drum hits, note attacks, etc.
class OnsetDetector : public RefCounted {
	GDCLASS(OnsetDetector, RefCounted)

	// Default analysis window size in samples (~10ms at 48kHz).
	static const int DEFAULT_WINDOW_SIZE = 512;
	// Default hop size between analysis windows.
	static const int DEFAULT_HOP_SIZE = 256;
	// Minimum inter-onset interval in samples (~50ms at 48kHz).
	static const int DEFAULT_MIN_ONSET_INTERVAL = 2400;

protected:
	static void _bind_methods();

public:
	// Detect onset (transient) positions in audio sample data.
	// p_wave_data: Interleaved audio samples (mono or stereo) in [-1.0, 1.0] range.
	// p_channel_count: Number of audio channels (1 = mono, 2 = stereo).
	// p_sensitivity: Detection sensitivity from 1 (least sensitive) to 100 (most sensitive).
	// p_sample_rate: Sample rate of the audio data (default 48000 Hz).
	// Returns: Array of sample indices where onsets (transients) were detected.
	static PackedInt32Array detect_onsets(
		const PackedFloat32Array &p_wave_data,
		int p_channel_count = 2,
		int p_sensitivity = 50,
		int p_sample_rate = 48000
	);

	// Detect onset (transient) positions directly from an AudioStream (WAV supported).
	static PackedInt32Array detect_onsets_from_stream(
		const Ref<AudioStream> &p_stream,
		int p_sensitivity = 50
	);

	// Overload accepting Vector<double> for internal C++ use.
	static Vector<int> detect_onsets_internal(
		const Vector<double> &p_wave_data,
		int p_channel_count = 2,
		int p_sensitivity = 50,
		int p_sample_rate = 48000
	);

	// Estimate the dominant BPM of audio sample data using onset-based analysis.
	// Returns 0.0 if there are too few onsets for reliable estimation.
	static double estimate_bpm(
		const PackedFloat32Array &p_wave_data,
		int p_channel_count = 2,
		int p_sample_rate = 48000
	);

	// Estimate the dominant BPM directly from an AudioStream (WAV supported).
	// Returns 0.0 if there are too few onsets for reliable estimation.
	static double estimate_bpm_from_stream(const Ref<AudioStream> &p_stream);

	OnsetDetector() {}
	~OnsetDetector() {}
};

#endif // SION_ONSET_DETECTOR_H
