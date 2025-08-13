/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SIOPM_WAVE_SAMPLER_DATA_H
#define SIOPM_WAVE_SAMPLER_DATA_H

#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/variant.hpp>
#include "chip/wave/siopm_wave_base.h"

using namespace godot;

class SiOPMWaveSamplerData : public SiOPMWaveBase {
	GDCLASS(SiOPMWaveSamplerData, SiOPMWaveBase)

	Vector<double> _wave_data;
	// Preserve original sample data to allow non-destructive re-application of fades
	Vector<double> _original_wave_data;
	int _channel_count = 0;
	int _pan = 0;
    // Source sample rate of the decoded audio (Hz). Default sampler target is 48000 Hz.
    int _sample_rate = 48000;
    // This flag is only available for non-loop samples.
	bool _ignore_note_off = false;
	// When true, playback is forced at original pitch regardless of note input.
	bool _fixed_pitch = false;
    // Additional per-sample pitch offset (in semitones). Used when _fixed_pitch
    // is enabled to allow global transposition independent from the incoming
    // MIDI note number. Positive values raise the pitch, negative values lower
    // it. A value of 0 preserves the original pitch.
    double _pitch_offset = 0.0;

	// Track the last applied fade so we can efficiently undo it.
	int _prev_fade_start = -1;
	int _prev_fade_end = -1;
	int _prev_fade_len = 0;

	void _prepare_wave_data(const Variant &p_data, int p_src_channel_count, int p_channel_count);

	//

	// Wave positions in the sample count.
	int _start_point = 0;
	int _end_point = 0;
	int _loop_point = -1; // -1 means no looping.

	// Seek head and end gaps in the sample.
	int _seek_head_silence();
	int _seek_end_gap();
	void _slice();

	// Apply short linear fades at the slice boundaries to avoid clicks when the
	// sampler starts or ends playback away from a zero-crossing. This is a very
	// small operation (<<1 ms) and is done once per slice change.
	void _apply_fade();

protected:
	static void _bind_methods();

public:
	Vector<double> get_wave_data() const { return _wave_data; }
	int get_channel_count() const { return _channel_count; }
	int get_pan() const { return _pan; }
	int get_sample_rate() const { return _sample_rate; }
	void set_pan(int p_pan);
	int get_length() const;

	bool is_ignoring_note_off() const { return _ignore_note_off; }
	void set_ignore_note_off(bool p_ignore);
	bool is_fixed_pitch() const { return _fixed_pitch; }
	void set_fixed_pitch(bool p_fixed) { _fixed_pitch = p_fixed; }
	double get_pitch_offset() const { return _pitch_offset; }
	void set_pitch_offset(double p_offset) { _pitch_offset = p_offset; }

	//

	int get_start_point() const { return _start_point; }
	int get_end_point() const { return _end_point; }
	int get_loop_point() const { return _loop_point; }
	int get_initial_sample_index(double p_phase = 0) const;

	void set_start_point(int p_start);
	void set_end_point(int p_end);
	void set_loop_point(int p_loop);
	void slice(int p_start_point, int p_end_point, int p_loop_point);

	// Create a shallow copy that shares the underlying sample buffer.
	Ref<SiOPMWaveSamplerData> duplicate() const;

	//

	SiOPMWaveSamplerData(const Variant &p_data = Variant(), bool p_ignore_note_off = false, int p_pan = 0, int p_src_channel_count = 2, int p_channel_count = 0, bool p_fixed_pitch = false);
	~SiOPMWaveSamplerData() {}
};

#endif // SIOPM_WAVE_SAMPLER_DATA_H
