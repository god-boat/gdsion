/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_WAVEFORM_NATIVE_BUILDER_H
#define SION_WAVEFORM_NATIVE_BUILDER_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

class WaveformNativeBuilder : public RefCounted {
	GDCLASS(WaveformNativeBuilder, RefCounted)

protected:
	static void _bind_methods();

public:
	Dictionary build_from_wav_path(
		const String &p_file_path,
		int p_stage0_frames_per_bin = 64,
		int p_max_polyline_cache_samples = 262144,
		int p_peak_stage_shift_padding = 3
	);

	WaveformNativeBuilder() {}
	~WaveformNativeBuilder() {}
};

#endif // SION_WAVEFORM_NATIVE_BUILDER_H