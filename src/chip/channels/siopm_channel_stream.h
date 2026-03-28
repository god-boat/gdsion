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
// Reads source frames from a SiOPMWaveStreamData ring buffer,
// applies dB-based gain (-36..+36 dB, matching sampler), scheduler-driven
// clip fades, pitch shifting via variable playback rate, and writes to
// SiON output streams.
//
// Unlike SiOPMChannelSampler, this channel has no ADSR envelope, no LFO,
// and no voice-stealing declick. Playback continues until out_sample / EOF
// or note_off. All params are real-time updateable via setters (called from
// the driver's mailbox drain on the audio thread).
class SiOPMChannelStream : public SiOPMChannelBase {
	GDCLASS(SiOPMChannelStream, SiOPMChannelBase)

	Ref<SiOPMWaveStreamData> _stream_data;

	// ---- Playback state (audio thread only) ----

	double _playback_pos = 0.0;     // Fractional source-frame position in ring buffer.
	double _source_frames_elapsed = 0.0; // Source frames consumed since note_on (pitch-aware).
	bool _playing = false;
	bool _reached_end = false;

	// ---- Clip params (set via mailbox, read in buffer()) ----

	int _gain_db = 0;               // Gain in dB (-36..+36), matches sampler.
	double _clip_gain = 1.0;        // Linear gain derived from _gain_db: pow(2, dB/6).
	double _pitch_step = 1.0;       // Source-frame advance per output sample.
	int _pitch_cents = 0;           // Pitch shift in cents.
	int _fade_in_frames = 0;        // Raw user fade-in param (cached for UI/live param parity).
	int _fade_out_frames = 0;       // Raw user fade-out param (cached for UI/live param parity).
	int _declick_in_remaining = 0;  // Remaining output samples for automatic start/seek declick ramp (0→1).
	int _declick_in_total = 0;      // Total output samples for the current declick-in ramp.

	// ---- Clip envelope (scheduler-driven, clip-time fades) ----
	// The scheduler sends the current clip-time position plus clip-time
	// fade boundaries. The channel advances clip-time per output sample
	// using the actual driver sample rate and evaluates the envelope
	// sample-accurately, independent of source-domain playback progress.
	double _clip_envelope = 1.0;             // Current clip envelope multiplier (0.0-1.0).
	double _clip_time_steps = 0.0;           // Current clip-time position relative to placement start.
	double _clip_fade_in_steps = 0.0;        // Fade-in duration in clip-time steps.
	double _clip_fade_out_start_steps = 0.0; // Fade-out start boundary in clip-time steps.
	double _clip_end_steps = 0.0;            // Placement end boundary in clip-time steps.
	int64_t _in_sample = 0;         // Start trim in source frames.
	int64_t _out_sample = 0;        // End trim (0 = EOF).
	int _warp_mode = 0;             // 0 = OFF, 1 = REPITCH, 2 = BEATS, 3 = TONES, 4 = TEXTURE, 5 = COMPLEX.
	double _clip_bpm = 0.0;         // Original BPM of the clip (for REPITCH warp mode).

	// ---- Loop state (set via mailbox, read in buffer()) ----

	bool _looping = false;          // Whether the clip should loop.
	int64_t _loop_start_sample = 0; // Loop start in absolute source frames.
	int64_t _loop_end_sample = 0;   // Loop end (0 = use out_sample/EOF).
	int64_t _loops_completed = 0;   // Number of full loops completed since note_on.

	// ---- Granular warp engine (TONES / TEXTURE warp modes) ----

	SiOPMWarpProcessor _warp;       // Reusable granular overlap-add processor.

	// ---- Stereo output ----

	SinglyLinkedList<int> *_out_pipe2 = nullptr;
	double _filter_variables2[3] = { 0, 0, 0 };

	// Unified pitch step recalculation from _pitch_cents.
	void _recalc_pitch_step();
	double _get_source_to_driver_rate_ratio() const;

	// Compute the effective clip length in source frames (accounts for trim).
	int64_t _effective_clip_length() const;

	// Compute technical envelope multiplier (loop crossfade, one-shot end declick).
	// User clip fades (fade-in/fade-out) are handled by _clip_envelope from the scheduler.
	double _compute_technical_envelope(double p_source_frame) const;
	double _get_clip_steps_per_output_sample() const;
	double _evaluate_clip_envelope(double p_clip_time_steps) const;

	// Shared note_on implementation: resets playback state and starts from p_start_sample.
	void _start_playback_at(int64_t p_start_sample);

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
	// Start playback from an explicit source-frame position instead of _in_sample.
	// Used when the track's deferred key-on carries a start offset (arrangement phase).
	void note_on_at(int64_t p_start_sample);
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

	// Scheduler-driven clip envelope state. The channel advances clip-time
	// per output sample from p_clip_time_steps and evaluates fades against
	// the supplied clip-time boundaries.
	void set_stream_clip_envelope(double p_clip_time_steps, double p_fade_in_steps, double p_fade_out_start_steps, double p_clip_end_steps);
	double get_stream_clip_envelope() const { return _clip_envelope; }

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

	void set_stream_loop_region(int64_t p_start_sample, int64_t p_end_sample);
	int64_t get_stream_loop_start() const { return _loop_start_sample; }
	int64_t get_stream_loop_end() const { return _loop_end_sample; }

	int64_t get_loops_completed() const { return _loops_completed; }

	// Seek to an absolute source-frame position. Resets playback cursor and
	// triggers a ring buffer refill from the new position.
	void seek_to(int64_t p_position_sample);

	SiOPMChannelStream(SiOPMSoundChip *p_chip = nullptr);
};

#endif // SIOPM_CHANNEL_STREAM_H
