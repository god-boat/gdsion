/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DRIVER_H
#define SION_DRIVER_H

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_generator.hpp>
#include <godot_cpp/classes/audio_stream_generator_playback.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/node.hpp>
#include <atomic>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <cstdint>
#include <godot_cpp/classes/audio_frame.hpp>

#include "sion_voice.h"
#include "chip/wave/siopm_wave_sampler_data.h"
#include "events/sion_event.h"
#include "events/sion_track_event.h"
#include "sequencer/base/mml_data.h"
#include "sequencer/base/mml_system_command.h"
#include "templates/singly_linked_list.h"
#include "sion_data.h"
#include "sion_stream.h"
#include "sion_stream_playback.h"
// Forward declarations in correct namespace to avoid ambiguity.
namespace godot { class SiONStream; class SiONStreamPlayback; }

using namespace godot;

class FaderUtil;
class MIDIModule;
class MMLEvent;
class MMLSequence;
class SiEffector;
class SiMMLSequencer;
class SiMMLTrack;
class SiONData;
class SiONDataConverterSMF;
class SiOPMSoundChip;
class SiOPMWaveTable;
class SiOPMWavePCMData;
class SiOPMWaveSamplerData;
class SiOPMWaveSamplerTable;

// SiONDriver class provides the driver of SiON's digital signal processor emulator. All SiON's basic operations are
// provided as driver's properties, methods, and signals. Only one instance must exist at a time.
// TODO: Mostly implemented, aside from MIDI support, audio stream sampling, and background sound. Refer to FIXMEs and TODOs.
class SiONDriver : public Node {
	GDCLASS(SiONDriver, Node)

public:
	static const char *VERSION;
	static const char *VERSION_FLAVOR;

	// Note-on exception modes.
	enum ExceptionMode {
		NEM_IGNORE = 0,    // Ignore track ID conflicts in note_on() method (default).
		NEM_REJECT = 1,    // Reject new note when track IDs are conflicting.
		NEM_OVERWRITE = 2, // Overwrite current note when track IDs are conflicting.
		NEM_SHIFT = 3,     // Shift the sound timing to next quantize when track IDs are conflicting.
		NEM_MAX = 4
	};

private:
	enum FrameProcessingType {
		NONE = 0,
		PROCESSING_QUEUE = 1,
		PROCESSING_IMMEDIATE = 2,
	};

	static const int TIME_AVERAGING_COUNT = 8;

	// Single unique instance.
	static SiONDriver *_mutex;
	static bool _allow_multiple_drivers;

	SiOPMSoundChip *sound_chip = nullptr;
	SiEffector *effector = nullptr;
	SiMMLSequencer *sequencer = nullptr;

	// Data.

	Ref<SiONData> _data;
	// MML string from previous compilation.
	String _mml_string;

	// Main playback.

	AudioStreamPlayer *_audio_player = nullptr;
	Ref<SiONStream> _audio_stream;
	Ref<SiONStreamPlayback> _audio_playback;

	FaderUtil *_fader = nullptr;

	// Background sound.

	Ref<AudioStream> _background_sample;
	Ref<SiOPMWaveSamplerData> _background_sample_data;
	double _background_loop_point = -1; // In seconds.

	Ref<SiONVoice> _background_voice;
	SiMMLTrack *_background_track = nullptr;
	SiMMLTrack *_background_fade_out_track = nullptr;

	int _background_fade_out_frames = 0;
	int _background_fade_in_frames = 0;
	int _background_fade_gap_frames = 0;
	int _background_total_fade_frames = 0;

	FaderUtil *_background_fader = nullptr;

	void _set_background_sample(const Ref<AudioStream> &p_sound);
	void _start_background_sample();
	void _fade_background_callback(double p_value);

	// Sound and output properties.

	// Module and streaming buffer size (must be a power of 2 between 32 and 8192, e.g., 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192).
	int _buffer_length = 512;
	// Output channels (1 or 2).
	int _channel_num = 2;
	// Output sample rate (48000 or 44100).
	double _sample_rate = 48000;
	// Output bitrate. Value of 0 means that the wave is represented by a float in [-1,+1].
	int _bitrate = 0;

	// Streaming and rendering.

	bool _beat_event_enabled = false;
	bool _stream_event_enabled = false;
	bool _fading_event_enabled = false;

	bool _is_streaming = false;
	bool _in_streaming_process = false;
	// Preserve stop after streaming.
	bool _preserve_stop = false;
	bool _suspend_streaming = false;
	// Suspend starting steam while loading.
	bool _suspend_while_loading = true;
	Vector<Variant> _loading_sound_list;
	// If true, FINISH_SEQUENCE event has already been dispatched.
	bool _is_finish_sequence_dispatched = false;

	Vector<double> _render_buffer;
	int _render_buffer_channel_num = 0;
	int _render_buffer_index = 0;
	int _render_buffer_size_max = 0;

	bool _parse_system_command(const List<Ref<MMLSystemCommand>> &p_system_commands);

	void _prepare_compile(String p_mml, const Ref<SiONData> &p_data);
	void _prepare_render(const Variant &p_data, int p_buffer_size, int p_buffer_channel_num, bool p_reset_effector);
	void _prepare_stream(const Variant &p_data, bool p_reset_effector);
	bool _rendering();
	void _streaming(); // no-op in pull model

	// Playback.

	// Auto stop when the sequence finishes.
	bool _auto_stop = false;
	bool _is_paused = false;
	double _start_position = 0; // ms
	double _master_volume = 1;
	double _fader_volume = 1;

	ExceptionMode _note_on_exception_mode = NEM_IGNORE;
	// Send the CHANGE_BPM event when position changes.
	bool _notify_change_bpm_on_position_changed = true;

	SiMMLTrack *_find_or_create_track(int p_track_id, double p_delay, double p_quant, bool p_disposable, int *r_delay_samples);

	void _update_volume();
	void _fade_callback(double p_value);

	// Processing.

	FrameProcessingType _current_frame_processing = FrameProcessingType::NONE;

	void _set_processing_queue();
	void _set_processing_immediate();
	void _clear_processing();

	void _prepare_process(const Variant &p_data, bool p_reset_effector);

	void _process_frame();
	void _process_frame_queue();
	void _process_frame_immediate();

	enum JobType {
		NO_JOB = 0,
		COMPILE = 1,
		RENDER = 2,
	};

	struct SiONDriverJob {
		JobType type = JobType::NO_JOB;
		Ref<SiONData> data;

		String mml_string;
		int buffer_size = 0;
		int channel_num = 0;
		bool reset_effector = false;
	};

	int _queue_interval = 500;
	int _queue_length = 0;
	double _job_progress = 0;
	JobType _current_job_type = JobType::NO_JOB;
	List<SiONDriverJob> _job_queue;
	List<Ref<SiONTrackEvent>> _track_event_queue;

	bool _prepare_next_job();
	void _cancel_all_jobs();

	// Events.

	double _convert_event_length(double p_length) const;

	void _dispatch_event(const Ref<SiONEvent> &p_event);

	void _note_on_callback(SiMMLTrack *p_track);
	void _note_off_callback(SiMMLTrack *p_track);
	void _publish_note_event(SiMMLTrack *p_track, int p_type, String p_frame_event, String p_stream_event);

	void _tempo_changed_callback(int p_buffer_index, bool p_dummy);
	void _beat_callback(int p_buffer_index, int p_beat_counter);

	MMLSequence *_timer_sequence = nullptr;
	MMLEvent *_timer_interval_event = nullptr; // MMLEvent::GLOBAL_WAIT

	void _timer_callback();

	// MIDI.
	// FIXME: Implement SMF/MIDI support.

	MIDIModule *_midi_module = nullptr;
	SiONDataConverterSMF *_midi_converter = nullptr;

	int _check_midi_event_listeners();
	void _dispatch_midi_event(String p_type, SiMMLTrack *p_track, int p_channel_number, int p_note, int p_data);

	// Benchmarking and stats.
	struct {
		// Previous compiling time.
		int compiling_time = 0;
		// Previous rendering time.
		int rendering_time = 0;
		// Average processing time in 1sec.
		int average_processing_time = 0;
		// Total processing time in last 8 bufferings.
		int total_processing_time = 0;
		// Processing time data of last 8 bufferings.
		SinglyLinkedList<int> *processing_time_data = nullptr;
		// Number for averaging _total_processing_time.
		double total_processing_time_ratio = 0;
		// Previous streaming time.
		int streaming_time = 0;
		// Streaming latency, ms.
		double streaming_latency = 0;
		// Previous frame timestamp, ms.
		int frame_timestamp = 0;
		// Frame rate, ms.
		int frame_rate = 1;

		void update_average_processing_time() {
			average_processing_time = total_processing_time * total_processing_time_ratio;
		}
	} _performance_stats;

	//

	void _update_node_processing();

	void _emit_signal_thread_safe(const StringName &p_signal, const Variant &p_arg = Variant());

	// --- Realtime mailbox (track-scoped SPSC ring, drained on audio thread) ------
	struct _TrackUpdate {
		int track_id = -1;
		// Optional: when >= 0, restricts update to channels stamped with this voice scope id
		int64_t voice_scope_id = -1;
		bool has_vol = false;
		double vol_linear = 1.0;
		bool has_pan = false;
		int pan = 0;
		bool has_filter = false;
		bool has_filter_cutoff = false;
		bool has_filter_resonance = false;
		int filter_cutoff = 128;
		int filter_resonance = 0;
		// Filter extended params
		bool has_filter_type = false;
		int filter_type = 0;
		bool has_filter_ar = false;
		int filter_ar = 0;
		bool has_filter_dr1 = false;
		int filter_dr1 = 0;
		bool has_filter_dr2 = false;
		int filter_dr2 = 0;
		bool has_filter_rr = false;
		int filter_rr = 0;
		bool has_filter_dc1 = false;
		int filter_dc1 = 128;
		bool has_filter_dc2 = false;
		int filter_dc2 = 64;
		bool has_filter_sc = false;
		int filter_sc = 32;
		bool has_filter_rc = false;
		int filter_rc = 128;
		// FM operator fields (one flag per message; separate messages for separate params)
		bool has_fm_op_tl = false;
		bool has_fm_op_mul = false;
		bool has_fm_op_fmul = false;
		bool has_fm_op_dt1 = false;
		bool has_fm_op_dt2 = false;
		int op_index = 0;
		int fm_value = 0;
		// Channel modulation (FM-only currently)
		bool has_ch_am = false;
		int ch_am_depth = 0;
		bool has_ch_pm = false;
		int ch_pm_depth = 0;
		// LFO frequency step
		bool has_lfo_step = false;
		int lfo_frequency_step = 0;
		// LFO wave shape (new)
		bool has_lfo_wave = false;
		int lfo_wave_shape = 0;
		// Envelope frequency ratio (channel)
		bool has_env_freq_ratio = false;
		int env_freq_ratio = 100;
		// Analog-Like (AL) live params
		bool has_al_ws1 = false;
		int al_ws1 = 0;
		bool has_al_ws2 = false;
		int al_ws2 = 0;
		bool has_al_balance = false;
		int al_balance = 0;
		bool has_al_detune2 = false;
		int al_detune2 = 0;
	};

	// Per-track cached filter state for merging partial updates
	struct _FilterState {
		bool initialized = false;
		int type = 0;
		int cutoff = 128;
		int resonance = 0;
		int ar = 0;
		int dr1 = 0;
		int dr2 = 0;
		int rr = 0;
		int dc1 = 128;
		int dc2 = 64;
		int sc = 32;
		int rc = 128;
	};

	static const int _MB_CAPACITY = 1024; // power of two for cheap wrap
	_TrackUpdate _mb_ring[_MB_CAPACITY];
	std::atomic<int> _mb_head { 0 }; // producer (main thread)
	std::atomic<int> _mb_tail { 0 }; // consumer (audio thread)

	// Cache for filter params per track
	HashMap<int, _FilterState> _filter_state_cache;
	_FilterState &_ensure_filter_state(int p_track_id);

	bool _mb_try_push(const _TrackUpdate &p_update);
	void _drain_track_mailbox();

protected:
	static void _bind_methods();

	void _notification(int p_what);

public:
	static String get_version() { return VERSION; }
	static String get_version_flavor() { return VERSION_FLAVOR; }

	// The singleton instance.
	static SiONDriver *get_mutex() { return _mutex; }
	// These are the factory methods that provide creation of SiONDriver instances. You can either create the driver with the constructor
	// (new SiONDriver()) or use static factory method (SiONDriver.create()) in your client code. Subsequent call of these methods in the
	// same client code causes errors by the default.
	// @param   buffer_length : (default=2048) Size of streaming buffer. Must be a power of 2 between 32 and 8192.
	// @param   channel_num   : (default=2) Output channel. 1 is monaural and 2 is stereo.
	// @param   sample_rate   : (default=48000) Output sample rate. Can be 48000Hz or 44100Hz.
	// @param   bitrate       : (default=0) Output bitrate.
	static SiONDriver *create(int p_buffer_length = 2048, int p_channel_num = 2, int p_sample_rate = 48000, int p_bitrate = 0);

	// Original code marks this as experimental and notes that each driver has a large memory footprint.
	// Some static classes can also be stateful, leading to conflicts if multiple drivers exist at the same time.
	// E.g. MMLParser.
	static bool are_multiple_drivers_allowed() { return _allow_multiple_drivers; }
	static void set_allow_multiple_drivers(bool p_allow) { _allow_multiple_drivers = p_allow; }

	SiOPMSoundChip *get_sound_chip() const { return sound_chip; }
	SiEffector *get_effector() const { return effector; }
	SiMMLSequencer *get_sequencer() const { return sequencer; }

	// Data.

	// Compiling only.
	String get_mml_string() const { return _mml_string; }
	Ref<SiONData> get_data() const { return _data; }
	void clear_data() { _data = Ref<SiONData>(); }

	Ref<SiOPMWaveTable> set_wave_table(int p_index, Vector<double> p_table);
	Ref<SiOPMWavePCMData> set_pcm_wave(int p_index, const Variant &p_data, double p_sampling_note = 69, int p_key_range_from = 0, int p_key_range_to = 127, int p_src_channel_num = 2, int p_channel_num = 0);
	Ref<SiOPMWaveSamplerData> set_sampler_wave(int p_index, const Variant &p_data, bool p_ignore_note_off = false, int p_pan = 0, int p_src_channel_num = 2, int p_channel_num = 0);
	void set_pcm_voice(int p_index, const Ref<SiONVoice> &p_voice);
	void set_sampler_table(int p_bank, const Ref<SiOPMWaveSamplerTable> &p_table);
	void set_envelope_table(int p_index, Vector<int> p_table, int p_loop_point = -1);
	void set_voice(int p_index, const Ref<SiONVoice> &p_voice);
	void clear_all_user_tables();

	SiMMLTrack *create_user_controllable_track(int p_track_id = 0);
	void notify_user_defined_track(int p_event_trigger_id, int p_note);

	// Main sound.

	AudioStreamPlayer *get_audio_player() const { return _audio_player; }
	Ref<SiONStream> get_audio_stream() const { return _audio_stream; }
	Ref<SiONStreamPlayback> get_audio_playback() const { return _audio_playback; }
	FaderUtil *get_fader() const { return _fader; }

	// Background sound.

	Ref<AudioStream> get_background_sample() const { return _background_sample; }
	void set_background_sample(const Ref<AudioStream> &p_sound, double p_mix_level = 0.5, double p_loop_point = -1);
	void clear_background_sample();
	Ref<SiOPMWaveSamplerData> get_background_sample_data() const { return _background_sample_data; }
	SiMMLTrack *get_background_sample_track() const { return _background_track; }

	double get_background_sample_fade_out_time() const;
	void set_background_sample_fade_out_time(double p_time);
	double get_background_sample_fade_in_time() const;
	void set_background_sample_fade_in_times(double p_time);
	double get_background_sample_fade_gap_time() const;
	void set_background_sample_fade_gap_time(double p_time);

	double get_background_sample_volume() const;
	void set_background_sample_volume(double p_value);

	// Sound and output properties.

	int get_track_count() const;
	int get_max_track_count() const;
	void set_max_track_count(int p_value);

	int get_buffer_length() const { return _buffer_length; }
	int get_channel_num() const { return _channel_num; }
	double get_sample_rate() const { return _sample_rate; }
	double get_bitrate() const { return _bitrate; }

	double get_volume() const;
	void set_volume(double p_value);
	double get_bpm() const;
	void set_bpm(double p_value);

	// Streaming and rendering.

	ExceptionMode get_note_on_exception_mode() const { return _note_on_exception_mode; }
	void set_note_on_exception_mode(ExceptionMode p_mode);

	bool get_auto_stop() const { return _auto_stop; }
	void set_auto_stop(bool p_enabled) { _auto_stop = p_enabled; }

	bool is_notify_change_bpm_on_position_changed() const { return _notify_change_bpm_on_position_changed; }
	void set_notify_change_bpm_on_position_changed(bool p_enabled) { _notify_change_bpm_on_position_changed = p_enabled; }

	double get_streaming_position() const;
	void set_start_position(double p_value);

	bool get_suspend_while_loading() { return _suspend_while_loading; }
	void set_suspend_while_loading(bool p_enabled) { _suspend_while_loading = p_enabled; }

	void set_beat_event_enabled(bool p_enabled) { _beat_event_enabled = p_enabled; }
	void set_stream_event_enabled(bool p_enabled) { _stream_event_enabled = p_enabled; }
	void set_fading_event_enabled(bool p_enabled) { _fading_event_enabled = p_enabled; }

	Ref<SiONData> compile(String p_mml);
	int queue_compile(String p_mml);

	PackedFloat64Array render(const Variant &p_data, int p_buffer_size, int p_buffer_channel_num = 2, bool p_reset_effector = true);
	int queue_render(const Variant &p_data, int p_buffer_size, int p_buffer_channel_num = 2, bool p_reset_effector = false);

	// Playback.

	void stream(bool p_reset_effector = true);
	void play(const Variant &p_data, bool p_reset_effector = true);
	void stop();
	void reset();
	void pause();
	void resume();

	bool is_streaming() const { return _is_streaming; }
	bool is_paused() const { return _is_paused; }

	SiMMLTrack *sample_on(int p_sample_number, double p_length = 0, double p_delay = 0, double p_quant = 0, int p_track_id = 0, bool p_disposable = true);
	SiMMLTrack *note_on(int p_note, const Ref<SiONVoice> &p_voice = Ref<SiONVoice>(), double p_length = 0, double p_delay = 0, double p_quant = 0, int p_track_id = 0, bool p_disposable = true);
	SiMMLTrack *note_on_with_bend(int p_note, int p_note_to, double p_bend_length, const Ref<SiONVoice> &p_voice = Ref<SiONVoice>(), double p_length = 0, double p_delay = 0, double p_quant = 0, int p_track_id = 0, bool p_disposable = true);
	TypedArray<SiMMLTrack> note_off(int p_note, int p_track_id = 0, double p_delay = 0, double p_quant = 0, bool p_stop_immediately = false);

	TypedArray<SiMMLTrack> sequence_on(const Ref<SiONData> &p_data, const Ref<SiONVoice> &p_voice = Ref<SiONVoice>(), double p_length = 0, double p_delay = 0, double p_quant = 1, int p_track_id = 0, bool p_disposable = true);
	TypedArray<SiMMLTrack> sequence_off(int p_track_id, double p_delay = 0, double p_quant = 1, bool p_stop_with_reset = false);

	void fade_in(double p_time);
	void fade_out(double p_time);

	// Processing.

	double get_queue_job_progress() const { return _job_progress; }
	double get_queue_total_progress() const;
	int get_queue_length() const;
	bool is_queue_executing() const;

	int start_queue(int p_interval = 500);

	// Events.

	void set_beat_callback_interval(double p_length_16th = 1);
	// Note: Original code takes a callback. Here you need to connect to the `timer_interval` signal.
	void set_timer_interval(double p_length = 1);

	// MIDI.
	// FIXME: Implement SMF/MIDI support.

	MIDIModule *get_midi_module() const { return _midi_module; }

	// Benchmarking and stats.

	int get_compiling_time() const { return _performance_stats.compiling_time; }
	int get_rendering_time() const { return _performance_stats.rendering_time; }
	int get_processing_time() const { return _performance_stats.average_processing_time; }

	double get_streaming_latency() const { return _performance_stats.streaming_latency; }

	//

	SiONDriver(int p_buffer_length = 2048, int p_channel_num = 2, int p_sample_rate = 48000, int p_bitrate = 0);
	~SiONDriver();

	/* Pull-model helper – fills a buffer of AudioFrame with freshly generated audio.
	   Returns the number of frames written (always p_frames on success). */
	int32_t generate_audio(godot::AudioFrame *p_buffer, int32_t p_frames);

	// Per-track metering helper: returns Vector2(rms, peak) for a given SiMMLTrack
	// If p_window_length <= 0, the driver's buffer length is used.
	godot::Vector2 track_get_level(Object *p_track_obj, int p_window_length = 0);

	// --- Mailbox API (call from main thread) -------------------------------------
	void mailbox_set_track_volume(int p_track_id, double p_linear_volume, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_pan(int p_track_id, int p_pan, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter(int p_track_id, int p_cutoff, int p_resonance, int p_type = -1, int p_attack_rate = -1, int p_decay_rate1 = -1, int p_decay_rate2 = -1, int p_release_rate = -1, int p_decay_cutoff1 = -1, int p_decay_cutoff2 = -1, int p_sustain_cutoff = -1, int p_release_cutoff = -1, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_type(int p_track_id, int p_type, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_cutoff(int p_track_id, int p_cutoff, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_resonance(int p_track_id, int p_resonance, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_attack_rate(int p_track_id, int p_value, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_decay_rate1(int p_track_id, int p_value, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_decay_rate2(int p_track_id, int p_value, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_release_rate(int p_track_id, int p_value, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_decay_cutoff1(int p_track_id, int p_value, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_decay_cutoff2(int p_track_id, int p_value, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_sustain_cutoff(int p_track_id, int p_value, int64_t p_voice_scope_id = -1);
	void mailbox_set_track_filter_release_cutoff(int p_track_id, int p_value, int64_t p_voice_scope_id = -1);
	// FM operator params (by operator index)
	void mailbox_set_fm_op_total_level(int p_track_id, int p_op_index, int p_value);
	void mailbox_set_fm_op_multiple(int p_track_id, int p_op_index, int p_value);
	void mailbox_set_fm_op_fine_multiple(int p_track_id, int p_op_index, int p_value);
	void mailbox_set_fm_op_detune1(int p_track_id, int p_op_index, int p_value);
	void mailbox_set_fm_op_detune2(int p_track_id, int p_op_index, int p_value);
	void mailbox_set_ch_am_depth(int p_track_id, int p_depth, int64_t p_voice_scope_id = -1);
	void mailbox_set_ch_pm_depth(int p_track_id, int p_depth, int64_t p_voice_scope_id = -1);
	void mailbox_set_lfo_frequency_step(int p_track_id, int p_step, int64_t p_voice_scope_id = -1);
	void mailbox_set_lfo_wave_shape(int p_track_id, int p_wave_shape, int64_t p_voice_scope_id = -1);
	void mailbox_set_envelope_freq_ratio(int p_track_id, int p_ratio, int64_t p_voice_scope_id = -1);
	// Analog-Like (AL) mailboxes
	void mailbox_set_ch_al_ws1(int p_track_id, int p_wave_shape, int64_t p_voice_scope_id = -1);
	void mailbox_set_ch_al_ws2(int p_track_id, int p_wave_shape, int64_t p_voice_scope_id = -1);
	void mailbox_set_ch_al_balance(int p_track_id, int p_balance, int64_t p_voice_scope_id = -1);
	void mailbox_set_ch_al_detune2(int p_track_id, int p_detune2, int64_t p_voice_scope_id = -1);
};

#endif // SION_DRIVER_H
