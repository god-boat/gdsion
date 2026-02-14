/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SIOPM_CHANNEL_STREAM_H
#define SIOPM_CHANNEL_STREAM_H

#include "chip/channels/siopm_channel_base.h"
#include "chip/siopm_warp_processor.h"
#include "chip/wave/siopm_wave_stream_data.h"
#include "templates/singly_linked_list.h"

enum SiONPitchTableType : unsigned int;
class SiOPMChannelParams;
class SiOPMSoundChip;
class SiOPMWaveBase;

// Streaming audio channel for clip playback.
//
// Reads pre-resampled 48 kHz frames from a SiOPMWaveStreamData ring buffer,
// applies dB-based gain (-36..+36 dB, matching sampler), fade in/out ramps,
// pitch shifting via variable playback rate, and writes to SiON output streams.
//
// Unlike SiOPMChannelSampler, this channel has no ADSR envelope, no LFO,
// and no voice-stealing declick. Playback continues until out_sample / EOF
// or note_off. All params are real-time updateable via setters (called from
// the driver's mailbox drain on the audio thread).
class SiOPMChannelStream : public SiOPMChannelBase {
	GDCLASS(SiOPMChannelStream, SiOPMChannelBase)

	Ref<SiOPMWaveStreamData> _stream_data;

	// ---- Playback state (audio thread only) ----

	double _playback_pos = 0.0;     // Fractional frame position in ring buffer.
	double _source_frames_elapsed = 0.0; // Source 48 kHz frames consumed since note_on (pitch-aware).
	bool _playing = false;
	bool _reached_end = false;

	// ---- Clip params (set via mailbox, read in buffer()) ----

	int _gain_db = 0;               // Gain in dB (-36..+36), matches sampler.
	double _clip_gain = 1.0;        // Linear gain derived from _gain_db: pow(2, dB/6).
	double _pitch_step = 1.0;       // Playback rate (1.0 = normal).
	int _pitch_cents = 0;           // Pitch shift in cents.
	int _fade_in_frames = 0;        // Fade-in length in 48 kHz frames.
	int _fade_out_frames = 0;       // Fade-out length in 48 kHz frames.
	int64_t _in_sample = 0;         // Start trim in 48 kHz frames.
	int64_t _out_sample = 0;        // End trim (0 = EOF).
	int _warp_mode = 0;             // 0 = OFF, 1 = REPITCH, 2 = BEATS, 3 = TONES, 4 = TEXTURE, 5 = COMPLEX.
	double _clip_bpm = 0.0;         // Original BPM of the clip (for REPITCH warp mode).

	// ---- Loop state (set via mailbox, read in buffer()) ----

	bool _looping = false;          // Whether the clip should loop.
	int64_t _loop_start_48k = 0;    // Loop start in absolute 48 kHz frames.
	int64_t _loop_end_48k = 0;      // Loop end (0 = use out_sample/EOF).
	int64_t _loops_completed = 0;   // Number of full loops completed since note_on.

	// ---- Granular warp engine (TONES / TEXTURE warp modes) ----

	SiOPMWarpProcessor _warp;       // Reusable granular overlap-add processor.

	// ---- Stereo output ----

	SinglyLinkedList<int> *_out_pipe2 = nullptr;
	double _filter_variables2[3] = { 0, 0, 0 };

	// Unified pitch step recalculation from _pitch_cents.
	void _recalc_pitch_step();

	// Compute the effective clip length in source 48 kHz frames (accounts for trim).
	int64_t _effective_clip_length() const;

	// Compute fade envelope multiplier for the given source-domain frame position.
	double _compute_fade_envelope(double p_source_frame) const;

	// Stream writers (mirror sampler channel pattern).
	void _write_stream_mono(SinglyLinkedList<int>::Element *p_output, int p_length);
	void _write_stream_stereo(SinglyLinkedList<int>::Element *p_output_left, SinglyLinkedList<int>::Element *p_output_right, int p_length);

protected:
	static void _bind_methods() {}

	String _to_string() const;

public:
	virtual void get_channel_params(const Ref<SiOPMChannelParams> &p_params) const override;
	virtual void set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation = true) override;

	virtual void set_wave_data(const Ref<SiOPMWaveBase> &p_wave_data) override;

	virtual void set_types(int p_pg_type, SiONPitchTableType p_pt_type) override {}
	virtual int get_pitch() const override { return 0; }
	virtual void set_pitch(int p_value) override {}
	virtual void set_phase(int p_value) override {}

	virtual void offset_volume(int p_expression, int p_velocity) override {}

	// Processing.

	virtual void note_on() override;
	virtual void note_off() override;

	virtual void buffer(int p_length) override;
	virtual void buffer_no_process(int p_length) override;

	//

	virtual void initialize(SiOPMChannelBase *p_prev, int p_buffer_index) override;
	virtual void reset() override;

	// ---- Stream-specific real-time param setters (called from driver mailbox) ----

	void set_stream_gain(double p_gain_db);
	int get_stream_gain_db() const { return _gain_db; }
	double get_stream_gain_linear() const { return _clip_gain; }

	void set_stream_pan(int p_pan);

	void set_stream_pitch_cents(int p_cents);
	int get_stream_pitch_cents() const { return _pitch_cents; }

	void set_stream_fade_in(int p_frames);
	int get_stream_fade_in() const { return _fade_in_frames; }

	void set_stream_fade_out(int p_frames);
	int get_stream_fade_out() const { return _fade_out_frames; }

	void set_stream_in_sample(int64_t p_sample);
	int64_t get_stream_in_sample() const { return _in_sample; }

	void set_stream_out_sample(int64_t p_sample);
	int64_t get_stream_out_sample() const { return _out_sample; }

	void set_stream_warp_mode(int p_mode);
	int get_stream_warp_mode() const { return _warp_mode; }

	void set_stream_clip_bpm(double p_bpm);
	double get_stream_clip_bpm() const { return _clip_bpm; }

	void set_stream_grain_size(double p_grain_size);
	double get_stream_grain_size() const { return _warp.get_grain_size(); }

	void set_stream_flux(double p_flux);
	double get_stream_flux() const { return _warp.get_flux(); }

	virtual void update_lfo_for_bpm() override;

	// Loop control — propagates to stream data for loader-side wrapping.
	void set_stream_looping(bool p_looping);
	bool get_stream_looping() const { return _looping; }

	void set_stream_loop_region(int64_t p_start_48k, int64_t p_end_48k);
	int64_t get_stream_loop_start() const { return _loop_start_48k; }
	int64_t get_stream_loop_end() const { return _loop_end_48k; }

	int64_t get_loops_completed() const { return _loops_completed; }

	// Seek to an absolute 48 kHz source position. Resets playback cursor and
	// triggers a ring buffer refill from the new position.
	void seek_to(int64_t p_position_48k);

	SiOPMChannelStream(SiOPMSoundChip *p_chip = nullptr);
};

#endif // SIOPM_CHANNEL_STREAM_H
