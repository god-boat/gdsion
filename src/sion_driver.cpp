/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "sion_driver.h"
#include "sion_stream.h"
#include "sion_stream_playback.h"
#include <algorithm>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/thread.hpp>

#include "sion_data.h"
#include "sion_enums.h"
#include "sion_voice.h"
#include "chip/channels/siopm_channel_base.h"
#include "chip/siopm_channel_params.h"
#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "chip/wave/siopm_wave_pcm_data.h"
#include "chip/wave/siopm_wave_pcm_table.h"
#include "chip/wave/siopm_wave_sampler_data.h"
#include "chip/wave/siopm_wave_table.h"
#include "effector/si_effector.h"
#include "events/sion_event.h"
#include "events/sion_track_event.h"
#include "sequencer/base/mml_event.h"
#include "sequencer/base/mml_executor.h"
#include "sequencer/base/mml_parser.h"
#include "sequencer/base/mml_parser_settings.h"
#include "sequencer/base/mml_sequence.h"
#include "sequencer/base/mml_sequence_group.h"
#include "sequencer/base/mml_sequencer.h"
#include "sequencer/simml_envelope_table.h"
#include "sequencer/simml_ref_table.h"
#include "sequencer/simml_sequencer.h"
#include "sequencer/simml_track.h"
#include "chip/channels/siopm_channel_base.h"
#include "chip/channels/siopm_channel_fm.h"
#include "utils/fader_util.h"
#include "utils/transformer_util.h"
#include <atomic>

// TODO: Extract somewhere more manageable?
const char *SiONDriver::VERSION = "0.7.0.0"; // Original code was last versioned as 0.6.6.0.
const char *SiONDriver::VERSION_FLAVOR = "beta8";

SiONDriver *SiONDriver::_mutex = nullptr;
bool SiONDriver::_allow_multiple_drivers = false;

// Data.

Ref<SiOPMWaveTable> SiONDriver::set_wave_table(int p_index, Vector<double> p_table) {
	int bits = -1;
	for (int i = p_table.size(); i > 0; i >>= 1) {
		bits += 1;
	}

	if (bits < 2) {
		return Ref<SiOPMWaveTable>();
	}

	Vector<int> wave_data = TransformerUtil::transform_pcm_data(p_table, 1);
	wave_data.resize_zeroed(1 << bits);

	Ref<SiOPMWaveTable> wave_table = memnew(SiOPMWaveTable(wave_data));
	SiOPMRefTable::get_instance()->register_wave_table(p_index, wave_table);
	return wave_table;
}

Ref<SiOPMWavePCMData> SiONDriver::set_pcm_wave(int p_index, const Variant &p_data, double p_sampling_note, int p_key_range_from, int p_key_range_to, int p_src_channel_num, int p_channel_num) {
	Ref<SiMMLVoice> pcm_voice = SiOPMRefTable::get_instance()->get_global_pcm_voice(p_index & (SiOPMRefTable::PCM_DATA_MAX - 1));
	Ref<SiOPMWavePCMTable> pcm_table = pcm_voice->get_wave_data();
	Ref<SiOPMWavePCMData> pcm_data = memnew(SiOPMWavePCMData(p_data, (int)(p_sampling_note * 64), p_src_channel_num, p_channel_num));

	pcm_table->set_key_range_data(pcm_data, p_key_range_from, p_key_range_to);
	return pcm_data;
}

Ref<SiOPMWaveSamplerData> SiONDriver::set_sampler_wave(int p_index, const Variant &p_data, bool p_ignore_note_off, int p_pan, int p_src_channel_num, int p_channel_num) {
	return SiOPMRefTable::get_instance()->register_sampler_data(p_index, p_data, p_ignore_note_off, p_pan, p_src_channel_num, p_channel_num);
}

void SiONDriver::set_pcm_voice(int p_index, const Ref<SiONVoice> &p_voice) {
	SiOPMRefTable::get_instance()->set_global_pcm_voice(p_index & (SiOPMRefTable::PCM_DATA_MAX - 1), p_voice);
}

void SiONDriver::set_sampler_table(int p_bank, const Ref<SiOPMWaveSamplerTable> &p_table) {
	SiOPMRefTable::get_instance()->sampler_tables.write[p_bank & (SiOPMRefTable::SAMPLER_TABLE_MAX - 1)] = p_table;
}

void SiONDriver::set_envelope_table(int p_index, Vector<int> p_table, int p_loop_point) {
	SiMMLRefTable::get_instance()->register_master_envelope_table(p_index, memnew(SiMMLEnvelopeTable(p_table, p_loop_point)));
}

void SiONDriver::set_voice(int p_index, const Ref<SiONVoice> &p_voice) {
	ERR_FAIL_COND_MSG(!p_voice->is_suitable_for_fm_voice(), "SiONDriver: Cannot register a voice that is not suitable to be an FM voice.");

	SiMMLRefTable::get_instance()->register_master_voice(p_index, p_voice);
}

void SiONDriver::clear_all_user_tables() {
	SiOPMRefTable::get_instance()->reset_all_user_tables();
	SiMMLRefTable::get_instance()->reset_all_user_tables();
}

SiMMLTrack *SiONDriver::create_user_controllable_track(int p_track_id) {
	int internal_track_id = (p_track_id & SiMMLTrack::TRACK_ID_FILTER) | SiMMLTrack::USER_CONTROLLED;

	return sequencer->create_controllable_track(internal_track_id, false);
}

void SiONDriver::notify_user_defined_track(int p_event_trigger_id, int p_note) {
	SiONTrackEvent *event = memnew(SiONTrackEvent(SiONTrackEvent::USER_DEFINED, this, nullptr, sequencer->get_stream_writing_residue(), p_note, p_event_trigger_id));
	_track_event_queue.push_back(event);
}

// Background sound.

void SiONDriver::_set_background_sample(const Ref<AudioStream> &p_sound) {
	_background_sample = p_sound;
	if (_background_sample.is_valid()) {
		_background_sample_data = Ref<SiOPMWaveSamplerData>(memnew(SiOPMWaveSamplerData(_background_sample, true)));
	} else {
		_background_sample_data = Ref<SiOPMWaveSamplerData>();
	}

	if (_is_streaming) {
		_start_background_sample();
	}
}

void SiONDriver::set_background_sample(const Ref<AudioStream> &p_sound, double p_mix_level, double p_loop_point) {
	set_background_sample_volume(p_mix_level);
	_background_loop_point = p_loop_point;
	_set_background_sample(p_sound);
}

void SiONDriver::clear_background_sample() {
	_background_loop_point = -1;
	_set_background_sample(nullptr);
}

void SiONDriver::_start_background_sample() {
	int start_frame = 0;
	int end_frame = 0;

	// Currently fading out -> stop fade out track.
	if (_background_fade_out_track) {
		_background_fade_out_track->set_disposable();
		_background_fade_out_track->key_off(0, true);
		_background_fade_out_track = nullptr;
	}

	// Background sound is playing now -> fade out.
	if (_background_track) {
		_background_fade_out_track = _background_track;
		_background_track = nullptr;
		start_frame = 0;
	} else {
		start_frame = _background_fade_out_frames + _background_fade_gap_frames;
	}

	// Play sound with fade in.
	if (_background_sample_data.is_valid()) {
		_background_voice->set_wave_data(_background_sample_data);
		if (_background_loop_point != -1) {
			_background_sample_data->slice(-1, -1, _background_loop_point * _sample_rate);
		}

		_background_track = sequencer->create_controllable_track(SiMMLTrack::DRIVER_BACKGROUND, false);
		_background_track->set_expression(128);
		_background_voice->update_track_voice(_background_track);
		_background_track->key_on(60, 0, (_background_fade_out_frames + _background_fade_gap_frames) * _buffer_length);

		end_frame = _background_total_fade_frames;
	} else {
		_background_voice->set_wave_data(Ref<SiOPMWaveBase>());
		_background_loop_point = -1;
		end_frame = _background_fade_out_frames + _background_fade_gap_frames;
	}

	// Set up the fader.
	if (end_frame - start_frame > 0) {
		_background_fader->set_fade(start_frame, end_frame, end_frame - start_frame);
	} else {
		// Stop fade out immediately.
		if (_background_fade_out_track) {
			_background_fade_out_track->set_disposable();
			_background_fade_out_track->key_off(0, true);
			_background_fade_out_track = nullptr;
		}
	}
}

void SiONDriver::_fade_background_callback(double p_value) {
	double fade_out = 0;
	double fade_in = 0;

	if (_background_fade_out_track) {
		if (_background_fade_out_frames > 0) {
			fade_out = 1.0 - p_value / _background_fade_out_frames;
			fade_out = CLAMP(fade_out, 0, 1);
		}

		_background_fade_out_track->set_expression(fade_out * 128);
	}

	if (_background_track) {
		if (_background_fade_in_frames > 0) {
			fade_in = 1.0 - (_background_total_fade_frames - p_value) / _background_fade_in_frames;
			fade_in = CLAMP(fade_in, 0, 1);
		} else {
			fade_in = 1.0;
		}

		_background_track->set_expression(fade_in * 128);
	}

	if (_background_fade_out_track && (fade_out == 0 || fade_in == 1)) {
		_background_fade_out_track->set_disposable();
		_background_fade_out_track->key_off(0, true);
		_background_fade_out_track = nullptr;
	}
}

double SiONDriver::get_background_sample_fade_out_time() const {
	return _background_fade_out_frames * _buffer_length / _sample_rate;
}

void SiONDriver::set_background_sample_fade_out_time(double p_time) {
	double ratio = _sample_rate / _buffer_length;

	_background_fade_out_frames = p_time * ratio;
	_background_total_fade_frames = _background_fade_out_frames + _background_fade_in_frames + _background_fade_gap_frames;
}

double SiONDriver::get_background_sample_fade_in_time() const {
	return _background_fade_in_frames * _buffer_length / _sample_rate;
}

void SiONDriver::set_background_sample_fade_in_times(double p_time) {
	double ratio = _sample_rate / _buffer_length;

	_background_fade_in_frames = p_time * ratio;
	_background_total_fade_frames = _background_fade_out_frames + _background_fade_in_frames + _background_fade_gap_frames;
}

double SiONDriver::get_background_sample_fade_gap_time() const {
	return _background_fade_gap_frames * _buffer_length / _sample_rate;
}

void SiONDriver::set_background_sample_fade_gap_time(double p_time) {
	double ratio = _sample_rate / _buffer_length;

	_background_fade_gap_frames = p_time * ratio;
	_background_total_fade_frames = _background_fade_out_frames + _background_fade_in_frames + _background_fade_gap_frames;
}

double SiONDriver::get_background_sample_volume() const {
	return _background_voice->get_channel_params()->get_master_volume(0);
}

void SiONDriver::set_background_sample_volume(double p_value) {
	_background_voice->get_channel_params()->set_master_volume(0, p_value);
	if (_background_track) {
		_background_track->set_master_volume(p_value * 128);
	}
	if (_background_fade_out_track) {
		_background_fade_out_track->set_master_volume(p_value * 128);
	}
}

// Sound parameters.

// Streaming only.
int SiONDriver::get_track_count() const {
	return sequencer->get_tracks().size();
}

int SiONDriver::get_max_track_count() const {
	return sequencer->get_max_track_count();
}

void SiONDriver::set_max_track_count(int p_value) {
	ERR_FAIL_COND_MSG(p_value < 1, "SiONDriver: Max track limit cannot be lower than 1.");

	sequencer->set_max_track_count(p_value);
}

void SiONDriver::_update_volume() {
	double db_volume = Math::linear2db(_master_volume * _fader_volume);
	_audio_player->set_volume_db(db_volume);
}

double SiONDriver::get_volume() const {
	return _master_volume;
}

void SiONDriver::set_volume(double p_value) {
	ERR_FAIL_COND_MSG(p_value < 0 || p_value > 1, "SiONDriver: Volume must be between 0.0 and 1.0 (inclusive).");

	_master_volume = p_value;
	_update_volume();
}

double SiONDriver::get_bpm() const {
	return sequencer->get_effective_bpm();
}

void SiONDriver::set_bpm(double p_value) {
	// Scholars debate whether BPM even has the upper limit. At some point it definitely turns into tone for most people,
	// with no discernible beat in earshot. But having no limit at all for the API feels strange. Besides, we don't have
	// infinitely scalable performance. So as a compromise you can set the BPM to up to 4000 beats per minute.
	// You're welcome!
	ERR_FAIL_COND_MSG(p_value < 1 || p_value > 4000, "SiONDriver: BPM must be between 1 and 4000 (inclusive).");

	sequencer->set_effective_bpm(p_value);
}

// Streaming and rendering.

void SiONDriver::set_note_on_exception_mode(ExceptionMode p_mode) {
	ERR_FAIL_INDEX(p_mode, NEM_MAX);

	_note_on_exception_mode = p_mode;
}

double SiONDriver::get_streaming_position() const {
	return sequencer->get_processed_sample_count() * 1000.0 / _sample_rate;
}

void SiONDriver::set_start_position(double p_value) {
	_start_position = p_value;
	if (sequencer->is_ready_to_process()) {
		sequencer->reset_all_tracks();
		sequencer->process_dummy(_start_position * _sample_rate * 0.001);
	}
}

bool SiONDriver::_parse_system_command(const List<Ref<MMLSystemCommand>> &p_system_commands) {
	bool effect_set = false;

	for (const Ref<MMLSystemCommand> &command : p_system_commands) {
		if (command->command == "#EFFECT") {
			effect_set = true;
			effector->parse_global_effect_mml(command->number, command->content, command->postfix);
		} else if (command->command == "#WAVCOLOR" || command->command == "#WAVC") {
			uint32_t wave_color = command->content.hex_to_int();
			set_wave_table(command->number, TransformerUtil::wave_color_to_vector(wave_color));
		}
	}

	return effect_set;
}

void SiONDriver::_prepare_compile(String p_mml, const Ref<SiONData> &p_data) {
	ERR_FAIL_COND(p_data.is_null());

	p_data->clear();
	_data = p_data;
	_mml_string = p_mml;
	sequencer->prepare_compile(_data, _mml_string);

	_job_progress = 0.01;
	_performance_stats.compiling_time = 0;
	_current_job_type = JobType::COMPILE;
}

void SiONDriver::_prepare_render(const Variant &p_data, int p_buffer_size, int p_buffer_channel_num, bool p_reset_effector) {
	_prepare_process(p_data, p_reset_effector);

	_render_buffer.clear();
	_render_buffer.resize_zeroed(p_buffer_size);

	_render_buffer_channel_num = (p_buffer_channel_num == 2 ? 2 : 1);
	_render_buffer_size_max = p_buffer_size;
	_render_buffer_index = 0;

	_job_progress = 0.01;
	_performance_stats.rendering_time = 0;
	_current_job_type = JobType::RENDER;
}

bool SiONDriver::_rendering() {
	// Processing.
	_drain_track_mailbox();
	sound_chip->begin_process();
	effector->begin_process();
	sequencer->process();
	effector->end_process();
	sound_chip->end_process();

	bool finished = false;

	// Limit the rendering length.
	int rendering_length = _buffer_length << 1;
	int buffer_extension = _buffer_length << (_render_buffer_channel_num - 1);

	if (_render_buffer_size_max != 0 && _render_buffer_size_max < (_render_buffer_index + buffer_extension)) {
		buffer_extension = _render_buffer_size_max - _render_buffer_index;
		finished = true;
	}

	// Extend the buffer.
	if (_render_buffer.size() < (_render_buffer_index + buffer_extension)) {
		_render_buffer.resize_zeroed(_render_buffer_index + buffer_extension);
	}

	// Read the output.
	Vector<double> *output_buffer = sound_chip->get_output_buffer_ptr();

	if (_render_buffer_channel_num == 2) {
		for (int i = 0, j = _render_buffer_index; i < rendering_length && j < _render_buffer.size(); i++, j++) {
			_render_buffer.write[j] = (*output_buffer)[i];
		}
	} else {
		for (int i = 0, j = _render_buffer_index; i < rendering_length && j < _render_buffer.size(); i += 2, j++) {
			_render_buffer.write[j] = (*output_buffer)[i];
		}
	}

	// Increment the index.
	_render_buffer_index += buffer_extension;

	return (finished || (_render_buffer_size_max == 0 && sequencer->is_finished()));
}

Ref<SiONData> SiONDriver::compile(String p_mml) {
	stop();

	int start_time = Time::get_singleton()->get_ticks_msec();
	Ref<SiONData> temp_data;
	temp_data.instantiate();
	_prepare_compile(p_mml, temp_data);

	_job_progress = sequencer->compile(0); // 0 ensures that the process is completed within the same frame.
	_performance_stats.compiling_time = Time::get_singleton()->get_ticks_msec() - start_time;
	_mml_string = "";

	static const StringName compilation_finished = StringName("compilation_finished");
	_emit_signal_thread_safe(compilation_finished, _data);
	return _data;
}

int SiONDriver::queue_compile(String p_mml) {
	ERR_FAIL_COND_V_MSG(p_mml.is_empty(), _job_queue.size(), "SiONDriver: Cannot queue a compile task, the MML string is empty.");

	Ref<SiONData> sion_data;
	sion_data.instantiate();

	SiONDriverJob compile_job;
	compile_job.type = JobType::COMPILE;
	compile_job.data = sion_data;
	compile_job.mml_string = p_mml;
	compile_job.channel_num = 2;

	_job_queue.push_back(compile_job);
	return _job_queue.size();
}

PackedFloat64Array SiONDriver::render(const Variant &p_data, int p_buffer_size, int p_buffer_channel_num, bool p_reset_effector) {
	stop();

	int start_time = Time::get_singleton()->get_ticks_msec();
	_prepare_render(p_data, p_buffer_size, p_buffer_channel_num, p_reset_effector);

	while (true) { // Render everything.
		if (_rendering()) {
			break;
		}
	}
	_performance_stats.rendering_time = Time::get_singleton()->get_ticks_msec() - start_time;

	PackedFloat64Array buffer;
	for (double value : _render_buffer) {
		buffer.push_back(value);
	}

	static const StringName render_finished = StringName("render_finished");
	_emit_signal_thread_safe(render_finished, buffer);
	return buffer;
}

int SiONDriver::queue_render(const Variant &p_data, int p_buffer_size, int p_buffer_channel_num, bool p_reset_effector) {
	ERR_FAIL_COND_V_MSG(p_data.get_type() == Variant::NIL, _job_queue.size(), "SiONDriver: Cannot queue a render task, the data object is empty.");
	ERR_FAIL_COND_V_MSG(p_buffer_size <= 0, _job_queue.size(), "SiONDriver: Cannot queue a render task, the buffer size must be a positive number.");

	Variant::Type data_type = p_data.get_type();
	switch (data_type) {
		case Variant::STRING: {
			String mml_string = p_data;

			// Data is shared between the two tasks.
			Ref<SiONData> sion_data = memnew(SiONData);
			sion_data.instantiate();

			// Queue compilation first.
			SiONDriverJob compile_job;
			compile_job.type = JobType::COMPILE;
			compile_job.data = sion_data;
			compile_job.mml_string = mml_string;
			compile_job.channel_num = 2;

			_job_queue.push_back(compile_job);

			// Then queue the render.
			SiONDriverJob render_job;
			render_job.type = JobType::RENDER;
			render_job.data = sion_data;
			render_job.buffer_size = p_buffer_size;
			render_job.channel_num = p_buffer_channel_num;
			render_job.reset_effector = p_reset_effector;

			_job_queue.push_back(render_job);
			return _job_queue.size();
		} break;

		case Variant::OBJECT: {
			Ref<SiONData> sion_data = p_data;
			if (sion_data.is_valid()) {
				SiONDriverJob render_job;
				render_job.type = JobType::RENDER;
				render_job.data = sion_data;
				render_job.buffer_size = p_buffer_size;
				render_job.channel_num = p_buffer_channel_num;
				render_job.reset_effector = p_reset_effector;

				_job_queue.push_back(render_job);
				return _job_queue.size();
			}
		} break;

		default: break; // Silences enum warnings.
	}

	ERR_FAIL_V_MSG(_job_queue.size(), "SiONDriver: Data type is unsupported by the render.");
}

// Playback.

void SiONDriver::_prepare_stream(const Variant &p_data, bool p_reset_effector) {
	_prepare_process(p_data, p_reset_effector);

	_performance_stats.total_processing_time = 0;
	_performance_stats.processing_time_data->reset();

	_is_paused = false;
	_is_finish_sequence_dispatched = (p_data.get_type() == Variant::NIL);

	// Start streaming.
	_is_streaming = true;
	_audio_player->play();
	_audio_playback = Ref<SiONStreamPlayback>();
	_audio_playback = _audio_player->get_stream_playback();

	_set_processing_immediate();
}

void SiONDriver::stream(bool p_reset_effector) {
	stop();
	_prepare_stream(nullptr, p_reset_effector);
}

void SiONDriver::play(const Variant &p_data, bool p_reset_effector) {
	stop();
	_prepare_stream(p_data, p_reset_effector);
}

void SiONDriver::stop() {
	if (!_is_streaming) {
		return;
	}
	if (_in_streaming_process) {
		_preserve_stop = true;
		return;
	}

	_preserve_stop = false;
	_is_paused = false;
	_is_streaming = false;

	clear_data(); // Original SiON doesn't do that, but that seems like an oversight.
	clear_background_sample();
	_clear_processing();

	_fader->stop();
	_fader_volume = 1;
	_audio_playback = Ref<SiONStreamPlayback>();
	_audio_player->stop();
	_update_volume();
	sequencer->stop_sequence();

	_dispatch_event(memnew(SiONEvent(SiONEvent::STREAM_STOPPED, this)));

	_performance_stats.streaming_latency = 0;
}

void SiONDriver::reset() {
	sequencer->reset_all_tracks();
}

void SiONDriver::pause() {
	if (_is_streaming) {
		_is_paused = true;
	}
}

void SiONDriver::resume() {
	_is_paused = false;
}

SiMMLTrack *SiONDriver::_find_or_create_track(int p_track_id, double p_delay, double p_quant, bool p_disposable, int *r_delay_samples) {
	ERR_FAIL_COND_V_MSG(p_delay < 0, nullptr, "SiONDriver: Playback delay cannot be less than zero.");

	int internal_track_id = (p_track_id & SiMMLTrack::TRACK_ID_FILTER) | SiMMLTrack::DRIVER_NOTE;
	double delay_samples = sequencer->calculate_sample_delay(0, p_delay, p_quant);

	SiMMLTrack *track = nullptr;

	// Check track ID conflicts.
	if (_note_on_exception_mode != NEM_IGNORE) {
		// Find a track with the same sound timings.
		track = sequencer->find_active_track(internal_track_id, delay_samples);

		if (track && _note_on_exception_mode == NEM_REJECT) {
			return nullptr;
		}
		if (track && _note_on_exception_mode == NEM_SHIFT) {
			int step = sequencer->calculate_sample_length(p_quant);
			while (track) {
				delay_samples += step;
				track = sequencer->find_active_track(internal_track_id, delay_samples);
			}
		}
	}

	*r_delay_samples = delay_samples;

	if (track) {
		return track;
	}

	track = sequencer->create_controllable_track(internal_track_id, p_disposable);
	ERR_FAIL_NULL_V_MSG(track, nullptr, "SiONDriver: Failed to allocate a track for playback. Pushing the limits?");
	return track;
}

SiMMLTrack *SiONDriver::sample_on(int p_sample_number, double p_length, double p_delay, double p_quant, int p_track_id, bool p_disposable) {
	ERR_FAIL_COND_V_MSG(!_is_streaming, nullptr, "SiONDriver: Driver is not streaming, you must call SiONDriver.stream() first.");
	ERR_FAIL_COND_V_MSG(p_length < 0, nullptr, "SiONDriver: Sample length cannot be less than zero.");

	int delay_samples = 0;
	SiMMLTrack *track = _find_or_create_track(p_delay, p_quant, p_track_id, p_disposable, &delay_samples);
	if (!track) {
		return nullptr;
	}

	track->set_channel_module_type(SiONModuleType::MODULE_SAMPLE, 0);
	track->key_on(p_sample_number, _convert_event_length(p_length), delay_samples);

	return track;
}

SiMMLTrack *SiONDriver::note_on(int p_note, const Ref<SiONVoice> &p_voice, double p_length, double p_delay, double p_quant, int p_track_id, bool p_disposable) {
	ERR_FAIL_COND_V_MSG(!_is_streaming, nullptr, "SiONDriver: Driver is not streaming, you must call SiONDriver.stream() first.");
	ERR_FAIL_COND_V_MSG(p_length < 0, nullptr, "SiONDriver: Note length cannot be less than zero.");

	int delay_samples = 0;
	SiMMLTrack *track = _find_or_create_track(p_delay, p_quant, p_track_id, p_disposable, &delay_samples);
	if (!track) {
		return nullptr;
	}

	if (p_voice.is_valid()) {
		p_voice->update_track_voice(track);
	}
	track->key_on(p_note, _convert_event_length(p_length), delay_samples);

	return track;
}

SiMMLTrack *SiONDriver::note_on_with_bend(int p_note, int p_note_to, double p_bend_length, const Ref<SiONVoice> &p_voice, double p_length, double p_delay, double p_quant, int p_track_id, bool p_disposable) {
	ERR_FAIL_COND_V_MSG(!_is_streaming, nullptr, "SiONDriver: Driver is not streaming, you must call SiONDriver.stream() first.");
	ERR_FAIL_COND_V_MSG(p_length < 0, nullptr, "SiONDriver: Note length cannot be less than zero.");
	ERR_FAIL_COND_V_MSG(p_bend_length < 0, nullptr, "SiONDriver: Pitch bending length cannot be less than zero.");

	int delay_samples = 0;
	SiMMLTrack *track = _find_or_create_track(p_delay, p_quant, p_track_id, p_disposable, &delay_samples);
	if (!track) {
		return nullptr;
	}

	if (p_voice.is_valid()) {
		p_voice->update_track_voice(track);
	}
	track->key_on(p_note, _convert_event_length(p_length), delay_samples);
	track->bend_note(p_note_to, _convert_event_length(p_bend_length));

	return track;
}

TypedArray<SiMMLTrack> SiONDriver::note_off(int p_note, int p_track_id, double p_delay, double p_quant, bool p_stop_immediately) {
	ERR_FAIL_COND_V_MSG(!_is_streaming, TypedArray<SiMMLTrack>(), "SiONDriver: Driver is not streaming, you must call SiONDriver.stream() first.");
	ERR_FAIL_COND_V_MSG(p_delay < 0, TypedArray<SiMMLTrack>(), "SiONDriver: Note off delay cannot be less than zero.");

	int internal_track_id = (p_track_id & SiMMLTrack::TRACK_ID_FILTER) | SiMMLTrack::DRIVER_NOTE;
	int delay_samples = sequencer->calculate_sample_delay(0, p_delay, p_quant);

	TypedArray<SiMMLTrack> tracks;
	for (SiMMLTrack *track : sequencer->get_tracks()) {
		if (track->get_internal_track_id() != internal_track_id) {
			continue;
		}

		if (p_note == -1 || (p_note == track->get_note() && track->get_channel()->is_note_on())) {
			track->key_off(delay_samples, p_stop_immediately);
			tracks.push_back(track);
		} else if (track->get_executor()->get_waiting_note() == p_note) {
			// This track is waiting for this note to start.
			track->key_on(p_note, 1, delay_samples);
			tracks.push_back(track);
		}
	}

	return tracks;
}

TypedArray<SiMMLTrack> SiONDriver::sequence_on(const Ref<SiONData> &p_data, const Ref<SiONVoice> &p_voice, double p_length, double p_delay, double p_quant, int p_track_id, bool p_disposable) {
	ERR_FAIL_COND_V(p_data.is_null(), TypedArray<SiMMLTrack>());
	ERR_FAIL_COND_V_MSG(p_length < 0, TypedArray<SiMMLTrack>(), "SiONDriver: Sequence length cannot be less than zero.");
	ERR_FAIL_COND_V_MSG(p_delay < 0, TypedArray<SiMMLTrack>(), "SiONDriver: Sequence delay cannot be less than zero.");

	int internal_track_id = (p_track_id & SiMMLTrack::TRACK_ID_FILTER) | SiMMLTrack::DRIVER_SEQUENCE;
	int delay_samples = sequencer->calculate_sample_delay(0, p_delay, p_quant);
	int length_samples = sequencer->calculate_sample_length(p_length);

	TypedArray<SiMMLTrack> tracks;

	MMLSequence *sequence = p_data->get_sequence_group()->get_head_sequence();
	while (sequence) {
		if (sequence->is_active()) {
			SiMMLTrack *track =	sequencer->create_controllable_track(internal_track_id, p_disposable);
			ERR_FAIL_NULL_V_MSG(track, tracks, "SiONDriver: Failed to allocate a track for playback. Pushing the limits?");

			track->sequence_on(p_data, sequence, length_samples, delay_samples);
			if (p_voice.is_valid()) {
				p_voice->update_track_voice(track);
			}

			tracks.push_back(track);
		}

		sequence = sequence->get_next_sequence();
	}

	return tracks;
}

TypedArray<SiMMLTrack> SiONDriver::sequence_off(int p_track_id, double p_delay, double p_quant, bool p_stop_with_reset) {
	ERR_FAIL_COND_V_MSG(p_delay < 0, TypedArray<SiMMLTrack>(), "SiONDriver: Sequence off delay cannot be less than zero.");

	int internal_track_id = (p_track_id & SiMMLTrack::TRACK_ID_FILTER) | SiMMLTrack::DRIVER_SEQUENCE;
	int delay_samples = sequencer->calculate_sample_delay(0, p_delay, p_quant);

	TypedArray<SiMMLTrack> tracks;
	for (SiMMLTrack *track : sequencer->get_tracks()) {
		if (track->get_internal_track_id() != internal_track_id) {
			continue;
		}

		track->sequence_off(delay_samples, p_stop_with_reset);
		tracks.push_back(track);
	}

	return tracks;
}

void SiONDriver::_fade_callback(double p_value) {
	_fader_volume = p_value;
	_update_volume();

	if (!_fading_event_enabled) {
		return;
	}

	_dispatch_event(memnew(SiONEvent(SiONEvent::FADING, this)));
}

void SiONDriver::fade_in(double p_time) {
	_fader->set_fade(0, 1, p_time * _sample_rate / _buffer_length);
}

void SiONDriver::fade_out(double p_time) {
	_fader->set_fade(1, 0, p_time * _sample_rate / _buffer_length);
}

// Processing.

void SiONDriver::_set_processing_queue() {
	ERR_FAIL_COND_MSG(_current_frame_processing != FrameProcessingType::NONE, vformat("SiONDriver: Cannot begin processing the queue, driver is busy (%d).", _current_frame_processing));
	_current_frame_processing = FrameProcessingType::PROCESSING_QUEUE;
	_update_node_processing();
}

void SiONDriver::_set_processing_immediate() {
	ERR_FAIL_COND_MSG(_current_frame_processing != FrameProcessingType::NONE, vformat("SiONDriver: Cannot begin immediate processing, driver is busy (%d).", _current_frame_processing));
	_current_frame_processing = FrameProcessingType::PROCESSING_IMMEDIATE;
	_update_node_processing();

	_performance_stats.frame_timestamp = Time::get_singleton()->get_ticks_msec();
}

void SiONDriver::_clear_processing() {
	_current_frame_processing = FrameProcessingType::NONE;
	_update_node_processing();
}

void SiONDriver::_prepare_process(const Variant &p_data, bool p_reset_effector) {
	Variant::Type data_type = p_data.get_type();
	switch (data_type) {
		case Variant::NIL: {
			// Do nothing and continue.
		} break;

		case Variant::STRING: { // MML string.
			String mml_string = p_data;
			compile(mml_string); // Populates _data inside.
		} break;

		case Variant::OBJECT: {
			Ref<SiONData> sion_data = p_data;
			if (sion_data.is_valid()) {
				_data = sion_data;
				break;
			}

			// TODO: Add MIDI/SMF support.

			ERR_FAIL_MSG("SiONDriver: Unsupported data type.");
		} break;

		default: {
			ERR_FAIL_MSG("SiONDriver: Unsupported data type.");
		} break;
	}

	// Order of operations below is critical.

	sound_chip->initialize(_channel_num, _bitrate, _buffer_length);  // Initialize DSP.
	sound_chip->reset();                                             // Reset all channels.

	if (p_reset_effector) {                                          // Initialize or reset effectors.
		effector->initialize();
	} else {
		effector->reset();
	}

	sequencer->prepare_process(_data, _sample_rate, _buffer_length); // Set sequencer tracks (should be called after sound_chip::reset()).
	if (_data.is_valid()) {
		_parse_system_command(_data->get_system_commands());         // Parse #EFFECT command (should be called after effector::reset()).
	}

	effector->prepare_process();                                     // Set effector connections.
	_track_event_queue.clear();                                      // Clear event queue.

	//

	// Set position if we don't start from the top.
	if (_data.is_valid() && _start_position > 0) {
		sequencer->process_dummy(_start_position * _sample_rate * 0.001);
	}

	if (_background_sample_data.is_valid()) {
		_start_background_sample();
	}

	if (_timer_interval_event->get_length() > 0) {
		sequencer->set_global_sequence(_timer_sequence);
	}
}

void SiONDriver::_process_frame() {
	switch (_current_frame_processing) {
		case FrameProcessingType::PROCESSING_QUEUE: {
			_process_frame_queue();
		} break;

		case FrameProcessingType::PROCESSING_IMMEDIATE: {
			_process_frame_immediate();
		} break;

		default: break; // Silences enum warnings.
	}
}

void SiONDriver::_process_frame_queue() {
	int start_time = Time::get_singleton()->get_ticks_msec();

	switch (_current_job_type) {
		case JobType::COMPILE: {
			_job_progress = sequencer->compile(_queue_interval);
			_performance_stats.compiling_time += Time::get_singleton()->get_ticks_msec() - start_time;
		} break;

		case JobType::RENDER: {
			_job_progress += (1 - _job_progress) * 0.5; // I guess we can't correctly track this?

			int rendering_time = Time::get_singleton()->get_ticks_msec() - start_time;
			while (rendering_time <= _queue_interval) {
				if (_rendering()) {
					_job_progress = 1;
					break;
				}

				rendering_time = Time::get_singleton()->get_ticks_msec() - start_time;
			}

			_performance_stats.rendering_time += Time::get_singleton()->get_ticks_msec() - start_time;
		} break;

		default: break; // Silences enum warnings.
	}

	// Finish the job and prepare the next one.
	if (_job_progress == 1) {
		switch (_current_job_type) {
			case JobType::COMPILE: {
				static const StringName compilation_finished = StringName("compilation_finished");
				_emit_signal_thread_safe(compilation_finished, _data);
			} break;

			case JobType::RENDER: {
				static const StringName render_finished = StringName("render_finished");

				PackedFloat64Array buffer;
				for (double value : _render_buffer) {
					buffer.push_back(value);
				}
				_emit_signal_thread_safe(render_finished, buffer);
			} break;

			default: break; // Silences enum warnings.
		}

		if (_prepare_next_job()) {
			return; // Queue is finished.
		}
	}

	_dispatch_event(memnew(SiONEvent(SiONEvent::QUEUE_EXECUTING, this)));
}

void SiONDriver::_process_frame_immediate() {
	// Calculate the framerate.
	int t = Time::get_singleton()->get_ticks_msec();
	_performance_stats.frame_rate = t - _performance_stats.frame_timestamp;
	_performance_stats.frame_timestamp = t;

	// This is true at the start of streaming.
	if (_suspend_streaming) {
		_suspend_streaming = false;

		// In the original code this event is cancellable and this means users can
		// react to it to trigger an immediate stop to streaming. If this is needed
		// in this implementation, you can just call stop() while reacting to the signal.
		_dispatch_event(memnew(SiONEvent(SiONEvent::STREAM_STARTED, this)));
		return;
	}

	if (_preserve_stop) {
		stop();
	}

	// Process events and keep the ones which are still remaining.
	if (_track_event_queue.size() > 0) {
		List<Ref<SiONTrackEvent>> remaining_events;
		for (const Ref<SiONTrackEvent> &event : _track_event_queue) {
			if (event->decrement_timer(_performance_stats.frame_rate)) {
				_dispatch_event(event);
				continue;
			}

			remaining_events.push_back(event);
		}

		_track_event_queue = remaining_events;
	}
}

bool SiONDriver::_prepare_next_job() {
	_data = Ref<SiONData>();
	_mml_string = "";

	_current_job_type = JobType::NO_JOB;
	if (_job_queue.size() == 0) {
		_queue_length = 0;
		_clear_processing();

		_dispatch_event(memnew(SiONEvent(SiONEvent::QUEUE_COMPLETED, this)));
		return true; // Finished.
	}

	SiONDriverJob job = _job_queue.front()->get();
	_job_queue.pop_front();

	switch (job.type) {
		case JobType::COMPILE: {
			if (job.mml_string.is_empty()) {
				WARN_PRINT("SiONDriver: Invalid compile job queued up, missing MML string.");
				return _prepare_next_job(); // Skip this job.
			}

			_prepare_compile(job.mml_string, job.data);
		} break;

		case JobType::RENDER: {
			if (job.buffer_size <= 0) {
				WARN_PRINT("SiONDriver: Invalid render job queued up, buffer size must be a positive number.");
				return _prepare_next_job(); // Skip this job.
			}

			_prepare_render(job.data, job.buffer_size, job.channel_num, job.reset_effector);
		} break;

		default: {
			WARN_PRINT("SiONDriver: Unknown job queued up.");
			return _prepare_next_job(); // Skip this job.
		} break;
	}

	return false; // Not finished yet.
}

void SiONDriver::_cancel_all_jobs() {
	_data = Ref<SiONData>();
	_mml_string = "";

	_current_job_type = JobType::NO_JOB;
	_job_progress = 0;
	_job_queue.clear();
	_queue_length = 0;
	_clear_processing();

	_dispatch_event(memnew(SiONEvent(SiONEvent::QUEUE_CANCELLED, this)));
}

double SiONDriver::get_queue_total_progress() const {
	if (_queue_length == 0) {
		return 1.0;
	}
	if (_queue_length == _job_queue.size()) {
		return 0.0;
	}

	return (_queue_length - _job_queue.size() - 1.0 + _job_progress) / _queue_length;
}

int SiONDriver::get_queue_length() const {
	return _job_queue.size();
}

bool SiONDriver::is_queue_executing() const {
	return (_job_progress > 0 && _job_progress < 1);
}

int SiONDriver::start_queue(int p_interval) {
	stop();

	_queue_length = _job_queue.size();
	if (_queue_length > 0) {
		_queue_interval = p_interval;
		_prepare_next_job();
		_set_processing_queue();
	}

	return _queue_length;
}

// Events.

double SiONDriver::_convert_event_length(double p_length) const {
	// Driver methods expect length in 1/16ths of a beat. The event length is in resolution units.
	// Note: In the original implementation this was mistakenly interpreted as 1/16th of
	// the sequencer's note resolution, which only translated to 1/4th of the beat.

	double beat_resolution = (double)sequencer->get_parser_settings()->resolution / 4.0;
	return p_length * beat_resolution * 0.0625;
}

void SiONDriver::_dispatch_event(const Ref<SiONEvent> &p_event) {
	// Convert event type to signal name and emit it in a thread-safe manner.
	String signal_name = p_event->get_event_type();
	ERR_FAIL_COND(signal_name.is_empty());

	_emit_signal_thread_safe(signal_name, p_event);
	return;
}

void SiONDriver::_note_on_callback(SiMMLTrack *p_track) {
	_publish_note_event(p_track, p_track->get_event_trigger_type_on(), SiONTrackEvent::NOTE_ON_FRAME, SiONTrackEvent::NOTE_ON_STREAM);
}

void SiONDriver::_note_off_callback(SiMMLTrack *p_track) {
	_publish_note_event(p_track, p_track->get_event_trigger_type_off(), SiONTrackEvent::NOTE_OFF_FRAME, SiONTrackEvent::NOTE_OFF_STREAM);
}

void SiONDriver::_publish_note_event(SiMMLTrack *p_track, int p_type, String p_frame_event, String p_stream_event) {
	// Frame event; dispatch later.
	if (p_type & 1) {
		Ref<SiONTrackEvent> event = memnew(SiONTrackEvent(p_frame_event, this, p_track));
		_track_event_queue.push_back(event);
		return;
	}

	// Stream event; dispatch immediately.
	if (p_type & 2) {
		Ref<SiONTrackEvent> event = memnew(SiONTrackEvent(p_stream_event, this, p_track));
		_dispatch_event(event);
		return;
	}
}

void SiONDriver::_tempo_changed_callback(int p_buffer_index, bool p_dummy) {
	Ref<SiONTrackEvent> event = memnew(SiONTrackEvent(SiONTrackEvent::BPM_CHANGED, this, nullptr, p_buffer_index));

	if (p_dummy && _notify_change_bpm_on_position_changed) {
		_dispatch_event(event);
	} else {
		_track_event_queue.push_back(event);
	}
}

void SiONDriver::_beat_callback(int p_buffer_index, int p_beat_counter) {
	if (!_beat_event_enabled) {
		return;
	}

	Ref<SiONTrackEvent> event = memnew(SiONTrackEvent(SiONTrackEvent::STREAMING_BEAT, this, nullptr, p_buffer_index, 0, p_beat_counter));
	_track_event_queue.push_back(event);
}

void SiONDriver::set_beat_callback_interval(double p_length_16th) {
	ERR_FAIL_COND_MSG(p_length_16th < 0, "SiONDriver: Beat callback interval value cannot be less than zero.");

	int filter = 1;
	double length = p_length_16th;

	while (length > 1.5) {
		filter <<= 1;
		length *= 0.5;
	}

	sequencer->set_beat_callback_filter(filter - 1);
}

void SiONDriver::_timer_callback() {
	static const StringName timer_interval = StringName("timer_interval");
	_emit_signal_thread_safe(timer_interval);
}

void SiONDriver::set_timer_interval(double p_length) {
	ERR_FAIL_COND_MSG(p_length < 0, "SiONDriver: Timer interval value cannot be less than zero.");

	_timer_interval_event->set_length(_convert_event_length(p_length));

	if (p_length > 0) {
		sequencer->set_timer_callback(Callable(this, "_timer_callback"));
	} else {
		sequencer->set_timer_callback(Callable());
	}
}

//

void SiONDriver::_update_node_processing() {
	set_process(_is_streaming || _current_frame_processing != FrameProcessingType::NONE);
}

void SiONDriver::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS: {
			if (_is_streaming) {
				_streaming();
			}
			if (_current_frame_processing != FrameProcessingType::NONE) {
				_process_frame();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			stop();
		} break;
	}
}

//

SiONDriver *SiONDriver::create(int p_buffer_length, int p_channel_num, int p_sample_rate, int p_bitrate) {
	return memnew(SiONDriver(p_buffer_length, p_channel_num, p_sample_rate, p_bitrate));
}

void SiONDriver::_bind_methods() {
	// Used as callables.

	ClassDB::bind_method(D_METHOD("_note_on_callback", "track"), &SiONDriver::_note_on_callback);
	ClassDB::bind_method(D_METHOD("_note_off_callback", "track"), &SiONDriver::_note_off_callback);
	ClassDB::bind_method(D_METHOD("_tempo_changed_callback", "buffer_index", "dummy"), &SiONDriver::_tempo_changed_callback);

	ClassDB::bind_method(D_METHOD("_beat_callback", "buffer_index", "beat_counter"), &SiONDriver::_beat_callback);
	ClassDB::bind_method(D_METHOD("_timer_callback"), &SiONDriver::_timer_callback);

	ClassDB::bind_method(D_METHOD("_fade_callback", "value"), &SiONDriver::_fade_callback);
	ClassDB::bind_method(D_METHOD("_fade_background_callback", "value"), &SiONDriver::_fade_callback);

	// Meta information.

	ClassDB::bind_static_method("SiONDriver", D_METHOD("get_version"), &SiONDriver::get_version);
	ClassDB::bind_static_method("SiONDriver", D_METHOD("get_version_flavor"), &SiONDriver::get_version_flavor);

	// Factory.

	ClassDB::bind_static_method("SiONDriver", D_METHOD("create", "buffer_size", "channel_num", "sample_rate", "bitrate"), &SiONDriver::create, DEFVAL(512), DEFVAL(2), DEFVAL(48000), DEFVAL(0));

	// Internal components.

	ClassDB::bind_method(D_METHOD("get_sound_chip"), &SiONDriver::get_sound_chip);
	ClassDB::bind_method(D_METHOD("get_effector"), &SiONDriver::get_effector);
	ClassDB::bind_method(D_METHOD("get_sequencer"), &SiONDriver::get_sequencer);

	ClassDB::bind_method(D_METHOD("get_audio_player"), &SiONDriver::get_audio_player);
	ClassDB::bind_method(D_METHOD("get_audio_stream"), &SiONDriver::get_audio_stream);
	ClassDB::bind_method(D_METHOD("get_audio_playback"), &SiONDriver::get_audio_playback);

	// Metering API
	ClassDB::bind_method(D_METHOD("track_get_level", "track", "window_length"), &SiONDriver::track_get_level, DEFVAL(0));

	// Mailbox bindings
	ClassDB::bind_method(D_METHOD("mailbox_set_track_volume", "track_id", "linear_volume", "voice_scope_id"), &SiONDriver::mailbox_set_track_volume, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_pan", "track_id", "pan", "voice_scope_id"), &SiONDriver::mailbox_set_track_pan, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter", "track_id", "cutoff", "resonance", "type", "attack_rate", "decay_rate1", "decay_rate2", "release_rate", "decay_cutoff1", "decay_cutoff2", "sustain_cutoff", "release_cutoff", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter, DEFVAL(-1), DEFVAL(-1), DEFVAL(-1), DEFVAL(-1), DEFVAL(-1), DEFVAL(-1), DEFVAL(-1), DEFVAL(-1), DEFVAL(-1), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_type", "track_id", "type", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_type, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_cutoff", "track_id", "cutoff", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_cutoff, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_resonance", "track_id", "resonance", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_resonance, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_attack_rate", "track_id", "value", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_attack_rate, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_decay_rate1", "track_id", "value", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_decay_rate1, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_decay_rate2", "track_id", "value", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_decay_rate2, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_release_rate", "track_id", "value", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_release_rate, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_decay_cutoff1", "track_id", "value", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_decay_cutoff1, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_decay_cutoff2", "track_id", "value", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_decay_cutoff2, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_sustain_cutoff", "track_id", "value", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_sustain_cutoff, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_filter_release_cutoff", "track_id", "value", "voice_scope_id"), &SiONDriver::mailbox_set_track_filter_release_cutoff, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_fm_op_total_level", "track_id", "op_index", "value"), &SiONDriver::mailbox_set_fm_op_total_level);
	ClassDB::bind_method(D_METHOD("mailbox_set_fm_op_multiple", "track_id", "op_index", "value"), &SiONDriver::mailbox_set_fm_op_multiple);
	ClassDB::bind_method(D_METHOD("mailbox_set_fm_op_fine_multiple", "track_id", "op_index", "value"), &SiONDriver::mailbox_set_fm_op_fine_multiple);
	ClassDB::bind_method(D_METHOD("mailbox_set_fm_op_detune1", "track_id", "op_index", "value"), &SiONDriver::mailbox_set_fm_op_detune1);
	ClassDB::bind_method(D_METHOD("mailbox_set_fm_op_detune2", "track_id", "op_index", "value"), &SiONDriver::mailbox_set_fm_op_detune2);
	ClassDB::bind_method(D_METHOD("mailbox_set_ch_am_depth", "track_id", "depth", "voice_scope_id"), &SiONDriver::mailbox_set_ch_am_depth, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_ch_pm_depth", "track_id", "depth", "voice_scope_id"), &SiONDriver::mailbox_set_ch_pm_depth, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_pitch_bend", "track_id", "value", "voice_scope_id"), &SiONDriver::mailbox_set_pitch_bend, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_lfo_frequency_step", "track_id", "step", "voice_scope_id"), &SiONDriver::mailbox_set_lfo_frequency_step, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_lfo_wave_shape", "track_id", "wave_shape", "voice_scope_id"), &SiONDriver::mailbox_set_lfo_wave_shape, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_envelope_freq_ratio", "track_id", "ratio", "voice_scope_id"), &SiONDriver::mailbox_set_envelope_freq_ratio, DEFVAL(-1));
	// Analog-Like (AL)
	ClassDB::bind_method(D_METHOD("mailbox_set_ch_al_ws1", "track_id", "wave_shape", "voice_scope_id"), &SiONDriver::mailbox_set_ch_al_ws1, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_ch_al_ws2", "track_id", "wave_shape", "voice_scope_id"), &SiONDriver::mailbox_set_ch_al_ws2, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_ch_al_balance", "track_id", "balance", "voice_scope_id"), &SiONDriver::mailbox_set_ch_al_balance, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_ch_al_detune2", "track_id", "detune2", "voice_scope_id"), &SiONDriver::mailbox_set_ch_al_detune2, DEFVAL(-1));

	// User-controllable track API
	ClassDB::bind_method(D_METHOD("create_user_controllable_track", "track_id"), &SiONDriver::create_user_controllable_track, DEFVAL(0));

	// Configuration.

	ClassDB::bind_method(D_METHOD("get_track_count"), &SiONDriver::get_track_count);
	ClassDB::bind_method(D_METHOD("get_max_track_count"), &SiONDriver::get_max_track_count);
	ClassDB::bind_method(D_METHOD("set_max_track_count", "value"), &SiONDriver::set_max_track_count);

	ClassDB::add_property("SiONDriver", PropertyInfo(Variant::INT, "max_track_count"), "set_max_track_count", "get_max_track_count");

	ClassDB::bind_method(D_METHOD("get_buffer_length"), &SiONDriver::get_buffer_length);
	ClassDB::bind_method(D_METHOD("get_channel_num"), &SiONDriver::get_channel_num);
	ClassDB::bind_method(D_METHOD("get_sample_rate"), &SiONDriver::get_sample_rate);
	ClassDB::bind_method(D_METHOD("get_bitrate"), &SiONDriver::get_bitrate);

	ClassDB::bind_method(D_METHOD("get_volume"), &SiONDriver::get_volume);
	ClassDB::bind_method(D_METHOD("set_volume", "value"), &SiONDriver::set_volume);
	ClassDB::bind_method(D_METHOD("get_bpm"), &SiONDriver::get_bpm);
	ClassDB::bind_method(D_METHOD("set_bpm", "bpm"), &SiONDriver::set_bpm);

	ClassDB::add_property("SiONDriver", PropertyInfo(Variant::FLOAT, "volume"), "set_volume", "get_volume");
	ClassDB::add_property("SiONDriver", PropertyInfo(Variant::FLOAT, "bpm"), "set_bpm", "get_bpm");

	// Events.

	ClassDB::bind_method(D_METHOD("set_beat_callback_interval", "length_16th"), &SiONDriver::set_beat_callback_interval);
	ClassDB::bind_method(D_METHOD("set_timer_interval", "length_16th"), &SiONDriver::set_timer_interval);

	ClassDB::bind_method(D_METHOD("set_beat_event_enabled", "enabled"), &SiONDriver::set_beat_event_enabled);
	ClassDB::bind_method(D_METHOD("set_stream_event_enabled", "enabled"), &SiONDriver::set_stream_event_enabled);
	ClassDB::bind_method(D_METHOD("set_fading_event_enabled", "enabled"), &SiONDriver::set_fading_event_enabled);

	// Processing, compiling, rendering.

	ClassDB::bind_method(D_METHOD("compile", "mml"), &SiONDriver::compile);
	ClassDB::bind_method(D_METHOD("queue_compile", "mml"), &SiONDriver::queue_compile);

	ClassDB::bind_method(D_METHOD("render", "data", "buffer_size", "buffer_channel_num", "reset_effector"), &SiONDriver::render, DEFVAL(2), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("queue_render", "data", "buffer_size", "buffer_channel_num", "reset_effector"), &SiONDriver::queue_render, DEFVAL(2), DEFVAL(false));

	ClassDB::bind_method(D_METHOD("stream", "reset_effector"), &SiONDriver::stream, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("play", "data", "reset_effector"), &SiONDriver::play, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("stop"), &SiONDriver::stop);
	ClassDB::bind_method(D_METHOD("reset"), &SiONDriver::reset);
	ClassDB::bind_method(D_METHOD("pause"), &SiONDriver::pause);
	ClassDB::bind_method(D_METHOD("resume"), &SiONDriver::resume);

	ClassDB::bind_method(D_METHOD("is_streaming"), &SiONDriver::is_streaming);
	ClassDB::bind_method(D_METHOD("is_paused"), &SiONDriver::is_paused);

	ClassDB::bind_method(D_METHOD("sample_on", "sample_number", "length", "delay", "quantize", "track_id", "disposable"), &SiONDriver::sample_on, DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("note_on", "note", "voice", "length", "delay", "quantize", "track_id", "disposable"), &SiONDriver::note_on, DEFVAL((Object *)nullptr), DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("note_on_with_bend", "note", "note_to", "bend_length", "voice", "length", "delay", "quantize", "track_id", "disposable"), &SiONDriver::note_on_with_bend, DEFVAL((Object *)nullptr), DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("note_off", "note", "track_id", "delay", "quantize", "stop_immediately"), &SiONDriver::note_off, DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(false));

	ClassDB::bind_method(D_METHOD("sequence_on", "data", "voice", "length", "delay", "quantize", "track_id", "disposable"), &SiONDriver::sequence_on, DEFVAL((Object *)nullptr), DEFVAL(0), DEFVAL(0), DEFVAL(1), DEFVAL(0), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("sequence_off", "track_id", "delay", "quantize", "stop_with_reset"), &SiONDriver::sequence_off, DEFVAL(0), DEFVAL(1), DEFVAL(false));

	ClassDB::bind_method(D_METHOD("get_data"), &SiONDriver::get_data);
	ClassDB::bind_method(D_METHOD("clear_data"), &SiONDriver::clear_data);

	// Execution queue.

	ClassDB::bind_method(D_METHOD("get_queue_job_progress"), &SiONDriver::get_queue_job_progress);
	ClassDB::bind_method(D_METHOD("get_queue_total_progress"), &SiONDriver::get_queue_total_progress);
	ClassDB::bind_method(D_METHOD("get_queue_length"), &SiONDriver::get_queue_length);
	ClassDB::bind_method(D_METHOD("is_queue_executing"), &SiONDriver::is_queue_executing);
	ClassDB::bind_method(D_METHOD("start_queue", "interval"), &SiONDriver::start_queue, DEFVAL(500));

	// Performance and stats.

	ClassDB::bind_method(D_METHOD("get_compiling_time"), &SiONDriver::get_compiling_time);
	ClassDB::bind_method(D_METHOD("get_rendering_time"), &SiONDriver::get_rendering_time);
	ClassDB::bind_method(D_METHOD("get_processing_time"), &SiONDriver::get_processing_time);

	//

	ADD_SIGNAL(MethodInfo("timer_interval"));
	ADD_SIGNAL(MethodInfo("compilation_finished", PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, "SiONData")));
	ADD_SIGNAL(MethodInfo("render_finished", PropertyInfo(Variant::PACKED_FLOAT64_ARRAY, "buffer")));

	ADD_SIGNAL(MethodInfo(SiONEvent::QUEUE_EXECUTING, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONEvent")));
	ADD_SIGNAL(MethodInfo(SiONEvent::QUEUE_COMPLETED, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONEvent")));
	ADD_SIGNAL(MethodInfo(SiONEvent::QUEUE_CANCELLED, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONEvent")));
	ADD_SIGNAL(MethodInfo(SiONEvent::STREAMING, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONEvent")));
	ADD_SIGNAL(MethodInfo(SiONEvent::STREAM_STARTED, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONEvent")));
	ADD_SIGNAL(MethodInfo(SiONEvent::STREAM_STOPPED, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONEvent")));
	ADD_SIGNAL(MethodInfo(SiONEvent::SEQUENCE_FINISHED, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONEvent")));
	ADD_SIGNAL(MethodInfo(SiONEvent::FADING, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONEvent")));
	ADD_SIGNAL(MethodInfo(SiONEvent::FADE_IN_COMPLETED, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONEvent")));
	ADD_SIGNAL(MethodInfo(SiONEvent::FADE_OUT_COMPLETED, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONEvent")));

	ADD_SIGNAL(MethodInfo(SiONTrackEvent::NOTE_ON_STREAM, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONTrackEvent")));
	ADD_SIGNAL(MethodInfo(SiONTrackEvent::NOTE_OFF_STREAM, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONTrackEvent")));
	ADD_SIGNAL(MethodInfo(SiONTrackEvent::NOTE_ON_FRAME, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONTrackEvent")));
	ADD_SIGNAL(MethodInfo(SiONTrackEvent::NOTE_OFF_FRAME, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONTrackEvent")));
	ADD_SIGNAL(MethodInfo(SiONTrackEvent::STREAMING_BEAT, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONTrackEvent")));
	ADD_SIGNAL(MethodInfo(SiONTrackEvent::BPM_CHANGED, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONTrackEvent")));
	ADD_SIGNAL(MethodInfo(SiONTrackEvent::USER_DEFINED, PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "SiONTrackEvent")));

	//

	BIND_ENUM_CONSTANT(CHIP_AUTO);
	BIND_ENUM_CONSTANT(CHIP_SIOPM);
	BIND_ENUM_CONSTANT(CHIP_OPL);
	BIND_ENUM_CONSTANT(CHIP_OPM);
	BIND_ENUM_CONSTANT(CHIP_OPN);
	BIND_ENUM_CONSTANT(CHIP_OPX);
	BIND_ENUM_CONSTANT(CHIP_MA3);
	BIND_ENUM_CONSTANT(CHIP_PMS_GUITAR);
	BIND_ENUM_CONSTANT(CHIP_ANALOG_LIKE);
	BIND_ENUM_CONSTANT(CHIP_MAX);

	BIND_ENUM_CONSTANT(MODULE_PSG);
	BIND_ENUM_CONSTANT(MODULE_APU);
	BIND_ENUM_CONSTANT(MODULE_NOISE);
	BIND_ENUM_CONSTANT(MODULE_MA3);
	BIND_ENUM_CONSTANT(MODULE_SCC);
	BIND_ENUM_CONSTANT(MODULE_GENERIC_PG);
	BIND_ENUM_CONSTANT(MODULE_FM);
	BIND_ENUM_CONSTANT(MODULE_PCM);
	BIND_ENUM_CONSTANT(MODULE_PULSE);
	BIND_ENUM_CONSTANT(MODULE_RAMP);
	BIND_ENUM_CONSTANT(MODULE_SAMPLE);
	BIND_ENUM_CONSTANT(MODULE_KS);
	BIND_ENUM_CONSTANT(MODULE_GB);
	BIND_ENUM_CONSTANT(MODULE_VRC6);
	BIND_ENUM_CONSTANT(MODULE_SID);
	BIND_ENUM_CONSTANT(MODULE_FM_OPM);
	BIND_ENUM_CONSTANT(MODULE_FM_OPN);
	BIND_ENUM_CONSTANT(MODULE_FM_OPNA);
	BIND_ENUM_CONSTANT(MODULE_FM_OPLL);
	BIND_ENUM_CONSTANT(MODULE_FM_OPL3);
	BIND_ENUM_CONSTANT(MODULE_FM_MA3);
	BIND_ENUM_CONSTANT(MODULE_MAX);

	BIND_ENUM_CONSTANT(PITCH_TABLE_OPM);
	BIND_ENUM_CONSTANT(PITCH_TABLE_PCM);
	BIND_ENUM_CONSTANT(PITCH_TABLE_PSG);
	BIND_ENUM_CONSTANT(PITCH_TABLE_OPM_NOISE);
	BIND_ENUM_CONSTANT(PITCH_TABLE_PSG_NOISE);
	BIND_ENUM_CONSTANT(PITCH_TABLE_APU_NOISE);
	BIND_ENUM_CONSTANT(PITCH_TABLE_GB_NOISE);
	BIND_ENUM_CONSTANT(PITCH_TABLE_MAX);

	BIND_ENUM_CONSTANT(PULSE_SINE);
	BIND_ENUM_CONSTANT(PULSE_SAW_UP);
	BIND_ENUM_CONSTANT(PULSE_SAW_DOWN);
	BIND_ENUM_CONSTANT(PULSE_TRIANGLE_FC);
	BIND_ENUM_CONSTANT(PULSE_TRIANGLE);
	BIND_ENUM_CONSTANT(PULSE_SQUARE);
	BIND_ENUM_CONSTANT(PULSE_NOISE);
	BIND_ENUM_CONSTANT(PULSE_KNM_BUBBLE);
	BIND_ENUM_CONSTANT(PULSE_SYNC_LOW);
	BIND_ENUM_CONSTANT(PULSE_SYNC_HIGH);
	BIND_ENUM_CONSTANT(PULSE_OFFSET);
	BIND_ENUM_CONSTANT(PULSE_SAW_VC6);
	BIND_ENUM_CONSTANT(PULSE_NOISE_WHITE);
	BIND_ENUM_CONSTANT(PULSE_NOISE_PULSE);
	BIND_ENUM_CONSTANT(PULSE_NOISE_SHORT);
	BIND_ENUM_CONSTANT(PULSE_NOISE_HIPASS);
	BIND_ENUM_CONSTANT(PULSE_NOISE_PINK);
	BIND_ENUM_CONSTANT(PULSE_NOISE_GB_SHORT);
	BIND_ENUM_CONSTANT(PULSE_PC_NZ_16BIT);
	BIND_ENUM_CONSTANT(PULSE_PC_NZ_SHORT);
	BIND_ENUM_CONSTANT(PULSE_PC_NZ_OPM);
	BIND_ENUM_CONSTANT(PULSE_MA3_SINE);
	BIND_ENUM_CONSTANT(PULSE_MA3_SINE_HALF);
	BIND_ENUM_CONSTANT(PULSE_MA3_SINE_HALF_DOUBLE);
	BIND_ENUM_CONSTANT(PULSE_MA3_SINE_QUART_DOUBLE);
	BIND_ENUM_CONSTANT(PULSE_MA3_SINE_X2);
	BIND_ENUM_CONSTANT(PULSE_MA3_SINE_HALF_DOUBLE_X2);
	BIND_ENUM_CONSTANT(PULSE_MA3_SQUARE);
	BIND_ENUM_CONSTANT(PULSE_MA3_SAW_SINE);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_SINE);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_SINE_HALF);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_SINE_HALF_DOUBLE);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_SINE_QUART_DOUBLE);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_SINE_X2);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_SINE_HALF_DOUBLE_X2);
	BIND_ENUM_CONSTANT(PULSE_MA3_SQUARE_HALF);
	BIND_ENUM_CONSTANT(PULSE_MA3_USER1);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_HALF);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_HALF_DOUBLE);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_QUART_DOUBLE);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_X2);
	BIND_ENUM_CONSTANT(PULSE_MA3_TRI_HALF_DOUBLE_X2);
	BIND_ENUM_CONSTANT(PULSE_MA3_SQUARE_QUART_DOUBLE);
	BIND_ENUM_CONSTANT(PULSE_MA3_USER2);
	BIND_ENUM_CONSTANT(PULSE_MA3_SAW);
	BIND_ENUM_CONSTANT(PULSE_MA3_SAW_HALF);
	BIND_ENUM_CONSTANT(PULSE_MA3_SAW_HALF_DOUBLE);
	BIND_ENUM_CONSTANT(PULSE_MA3_SAW_QUART_DOUBLE);
	BIND_ENUM_CONSTANT(PULSE_MA3_SAW_X2);
	BIND_ENUM_CONSTANT(PULSE_MA3_SAW_HALF_DOUBLE_X2);
	BIND_ENUM_CONSTANT(PULSE_MA3_SQUARE_QUART);
	BIND_ENUM_CONSTANT(PULSE_MA3_USER3);
	BIND_ENUM_CONSTANT(PULSE_PULSE);
	BIND_ENUM_CONSTANT(PULSE_PULSE_SPIKE);
	BIND_ENUM_CONSTANT(PULSE_RAMP);
	BIND_ENUM_CONSTANT(PULSE_CUSTOM);
	BIND_ENUM_CONSTANT(PULSE_PCM);
	BIND_ENUM_CONSTANT(PULSE_USER_CUSTOM);
	BIND_ENUM_CONSTANT(PULSE_USER_PCM);
}

SiONDriver::SiONDriver(int p_buffer_length, int p_channel_num, int p_sample_rate, int p_bitrate) {
	// Ensure reference tables use the correct sampling rate before any chip initialization. Or else the synthesizer will be out of tune!
	{
		SiOPMRefTable *ref_table = SiOPMRefTable::get_instance();
		if (!ref_table || ref_table->sampling_rate != p_sample_rate) {
			// Recreate the reference table with the requested rate.
			SiOPMRefTable::finalize();
			memnew(SiOPMRefTable(3580000, 1789772.5, p_sample_rate));
		}
	}
	ERR_FAIL_COND_MSG(!_allow_multiple_drivers && _mutex, "SiONDriver: Only one driver instance is allowed.");
	_mutex = this;

	// Check if buffer length is a power of 2 and between 32 and 8192
	bool is_valid_buffer_length = p_buffer_length >= 2 && p_buffer_length <= 8192 && (p_buffer_length & (p_buffer_length - 1)) == 0;
	ERR_FAIL_COND_MSG(!is_valid_buffer_length, "SiONDriver: Buffer length must be a power of 2 between 32 and 8192 (e.g., 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192).");
	ERR_FAIL_COND_MSG((p_channel_num != 1 && p_channel_num != 2), "SiONDriver: Channel number can only be 1 (mono) or 2 (stereo).");
	ERR_FAIL_COND_MSG((p_sample_rate != 44100 && p_sample_rate != 48000), "SiONDriver: Sampling rate can only be 44100 or 48000.");

	sound_chip = memnew(SiOPMSoundChip);
	effector = memnew(SiEffector(sound_chip));
	sequencer = memnew(SiMMLSequencer(sound_chip));
	sequencer->set_note_on_callback(Callable(this, "_note_on_callback"));
	sequencer->set_note_off_callback(Callable(this, "_note_off_callback"));
	sequencer->set_tempo_changed_callback(Callable(this, "_tempo_changed_callback"));
	sequencer->set_beat_callback(Callable(this, "_beat_callback"));

	// Main sound.
	{
		_audio_player = memnew(AudioStreamPlayer);
		add_child(_audio_player);
		_update_volume();

		_audio_stream.instantiate();
		_audio_stream->set_driver(this);
		_audio_player->set_stream(_audio_stream);

		// Buffer management is now handled by the engine pull model; no explicit ring buffer needed.

		_fader = memnew(FaderUtil);
		_fader->set_callback(Callable(this, "_fade_callback"));
	}

	// Background sound.
	{
		Ref<SiONVoice> voice = memnew(SiONVoice(SiONModuleType::MODULE_SAMPLE));
		_background_voice = voice;
		_background_voice->set_update_volumes(true);
		_background_fader = memnew(FaderUtil);
		_background_fader->set_callback(Callable(this, "_fade_background_callback"));
	}

	// FIXME: Implement SMF/MIDI support.
	//_midi_module = memnew(MIDIModule);
	//_midi_converter = memnew(SiONDataConverterSMF(nullptr, _midi_module));

	{
		_buffer_length = p_buffer_length;
		_channel_num = p_channel_num;
		_sample_rate = p_sample_rate;
		_bitrate = p_bitrate;
	}

	{
		_timer_sequence = memnew(MMLSequence);
		_timer_sequence->initialize();
		_timer_sequence->append_new_event(MMLEvent::REPEAT_ALL, 0);
		_timer_sequence->append_new_event(MMLEvent::TIMER, 0);
		_timer_interval_event = _timer_sequence->append_new_event(MMLEvent::GLOBAL_WAIT, 0);
	}

	_performance_stats.processing_time_data = memnew(SinglyLinkedList<int>(TIME_AVERAGING_COUNT, 0, true));
	_performance_stats.total_processing_time_ratio = _sample_rate / (_buffer_length * TIME_AVERAGING_COUNT);
}

SiONDriver::~SiONDriver() {
	if (_mutex == this) {
		_mutex = nullptr;
	}

	_timer_interval_event = nullptr;
	memdelete(_timer_sequence);

	memdelete(_fader);
	memdelete(_background_fader);

	memdelete(sequencer);
	memdelete(effector);
	memdelete(sound_chip);
}

int32_t SiONDriver::generate_audio(AudioFrame *p_buffer, int32_t p_frames) {
    if (p_frames <= 0) {
        return 0;
    }

    int frames_generated = 0;
    const int block = _buffer_length; // internal SiON processing block

    while (frames_generated < p_frames) {
        // Process one internal block to ensure sound_chip output is ready.
        _drain_track_mailbox();
        sound_chip->begin_process();
        effector->begin_process();
        sequencer->process(); // generates _buffer_length stereo samples
        effector->end_process();
        sound_chip->end_process();

        Vector<double> *out_buf = sound_chip->get_output_buffer_ptr();

        int frames_to_copy = std::min(block, p_frames - frames_generated);
        // Copy left/right pairs into AudioFrame array.
        for (int i = 0; i < frames_to_copy; ++i) {
            int src_index = i * 2; // stereo in SiON buffer
            p_buffer[frames_generated + i].left  = (float)(*out_buf)[src_index];
            p_buffer[frames_generated + i].right = (float)(*out_buf)[src_index + 1];
        }

        frames_generated += frames_to_copy;

        // If we copied less than a full block, keep residual samples for next pull (TODO: implement ring if needed)
        // Simplification: we discard extra samples beyond frames_to_copy.
    }

    return frames_generated;
}

// Pull model: _streaming() is obsolete, kept as no-op for notification hook compatibility.
void SiONDriver::_streaming() {
    // Intentionally empty – audio is now delivered via generate_audio() inside _mix().
}

Vector2 SiONDriver::track_get_level(Object *p_track_obj, int p_window_length) {
	SiMMLTrack *track = Object::cast_to<SiMMLTrack>(p_track_obj);
	if (!track) {
		return Vector2(0.0, 0.0);
	}
	SiOPMChannelBase *ch = track->get_channel();
	if (!ch) {
		return Vector2(0.0, 0.0);
	}
	// Post-fader approximation: scale channel meter by current master volume.
	// Pan is constant-power, so we ignore it; future: implement bus-level post-mix meter.
	Vector2 v = ch->get_recent_level(p_window_length);
	double vol = ch->get_master_volume() * 0.0078125; // 1/128
	v.x *= vol;
	v.y *= vol;
	return v;
}

// --- Mailbox setters (main thread) --------------------------------------------
void SiONDriver::mailbox_set_track_volume(int p_track_id, double p_linear_volume, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_vol = true;
    u.vol_linear = p_linear_volume;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_track_pan(int p_track_id, int p_pan, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_pan = true;
    u.pan = p_pan;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_track_filter(int p_track_id, int p_cutoff, int p_resonance, int p_type, int p_attack_rate, int p_decay_rate1, int p_decay_rate2, int p_release_rate, int p_decay_cutoff1, int p_decay_cutoff2, int p_sustain_cutoff, int p_release_cutoff, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_filter = true;
    u.filter_cutoff = p_cutoff;
    u.filter_resonance = p_resonance;
    if (p_type >= 0) { u.has_filter_type = true; u.filter_type = p_type; }
    if (p_attack_rate >= 0) { u.has_filter_ar = true; u.filter_ar = p_attack_rate; }
    if (p_decay_rate1 >= 0) { u.has_filter_dr1 = true; u.filter_dr1 = p_decay_rate1; }
    if (p_decay_rate2 >= 0) { u.has_filter_dr2 = true; u.filter_dr2 = p_decay_rate2; }
    if (p_release_rate >= 0) { u.has_filter_rr = true; u.filter_rr = p_release_rate; }
    if (p_decay_cutoff1 >= 0) { u.has_filter_dc1 = true; u.filter_dc1 = p_decay_cutoff1; }
    if (p_decay_cutoff2 >= 0) { u.has_filter_dc2 = true; u.filter_dc2 = p_decay_cutoff2; }
    if (p_sustain_cutoff >= 0) { u.has_filter_sc = true; u.filter_sc = p_sustain_cutoff; }
    if (p_release_cutoff >= 0) { u.has_filter_rc = true; u.filter_rc = p_release_cutoff; }
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_track_filter_type(int p_track_id, int p_type, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_type = true; u.filter_type = p_type; _mb_try_push(u);
}
void SiONDriver::mailbox_set_track_filter_cutoff(int p_track_id, int p_cutoff, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_cutoff = true; u.filter_cutoff = p_cutoff; _mb_try_push(u);
}
void SiONDriver::mailbox_set_track_filter_resonance(int p_track_id, int p_resonance, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_resonance = true; u.filter_resonance = p_resonance; _mb_try_push(u);
}
void SiONDriver::mailbox_set_track_filter_attack_rate(int p_track_id, int p_value, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_ar = true; u.filter_ar = p_value; _mb_try_push(u);
}
void SiONDriver::mailbox_set_track_filter_decay_rate1(int p_track_id, int p_value, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_dr1 = true; u.filter_dr1 = p_value; _mb_try_push(u);
}
void SiONDriver::mailbox_set_track_filter_decay_rate2(int p_track_id, int p_value, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_dr2 = true; u.filter_dr2 = p_value; _mb_try_push(u);
}
void SiONDriver::mailbox_set_track_filter_release_rate(int p_track_id, int p_value, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_rr = true; u.filter_rr = p_value; _mb_try_push(u);
}
void SiONDriver::mailbox_set_track_filter_decay_cutoff1(int p_track_id, int p_value, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_dc1 = true; u.filter_dc1 = p_value; _mb_try_push(u);
}
void SiONDriver::mailbox_set_track_filter_decay_cutoff2(int p_track_id, int p_value, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_dc2 = true; u.filter_dc2 = p_value; _mb_try_push(u);
}
void SiONDriver::mailbox_set_track_filter_sustain_cutoff(int p_track_id, int p_value, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_sc = true; u.filter_sc = p_value; _mb_try_push(u);
}
void SiONDriver::mailbox_set_track_filter_release_cutoff(int p_track_id, int p_value, int64_t p_voice_scope_id) {
    _TrackUpdate u; u.track_id = p_track_id; u.voice_scope_id = p_voice_scope_id; u.has_filter_rc = true; u.filter_rc = p_value; _mb_try_push(u);
}

void SiONDriver::mailbox_set_fm_op_total_level(int p_track_id, int p_op_index, int p_value) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.has_fm_op_tl = true;
    u.op_index = p_op_index;
    u.fm_value = p_value;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_fm_op_multiple(int p_track_id, int p_op_index, int p_value) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.has_fm_op_mul = true;
    u.op_index = p_op_index;
    u.fm_value = p_value;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_fm_op_fine_multiple(int p_track_id, int p_op_index, int p_value) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.has_fm_op_fmul = true;
    u.op_index = p_op_index;
    u.fm_value = p_value;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_fm_op_detune1(int p_track_id, int p_op_index, int p_value) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.has_fm_op_dt1 = true;
    u.op_index = p_op_index;
    u.fm_value = p_value;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_fm_op_detune2(int p_track_id, int p_op_index, int p_value) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.has_fm_op_dt2 = true;
    u.op_index = p_op_index;
    u.fm_value = p_value;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_ch_am_depth(int p_track_id, int p_depth, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_ch_am = true;
    u.ch_am_depth = p_depth;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_ch_pm_depth(int p_track_id, int p_depth, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_ch_pm = true;
    u.ch_pm_depth = p_depth;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_pitch_bend(int p_track_id, int p_value, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_pitch_bend = true;
    u.pitch_bend = CLAMP(p_value, -8192, 8191);
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_lfo_frequency_step(int p_track_id, int p_step, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_lfo_step = true;
    u.lfo_frequency_step = p_step;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_lfo_wave_shape(int p_track_id, int p_wave_shape, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_lfo_wave = true;
    u.lfo_wave_shape = p_wave_shape;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_envelope_freq_ratio(int p_track_id, int p_ratio, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_env_freq_ratio = true;
    u.env_freq_ratio = p_ratio;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_ch_al_ws1(int p_track_id, int p_wave_shape, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_al_ws1 = true;
    u.al_ws1 = p_wave_shape;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_ch_al_ws2(int p_track_id, int p_wave_shape, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_al_ws2 = true;
    u.al_ws2 = p_wave_shape;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_ch_al_balance(int p_track_id, int p_balance, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_al_balance = true;
    u.al_balance = p_balance;
    _mb_try_push(u);
}

void SiONDriver::mailbox_set_ch_al_detune2(int p_track_id, int p_detune2, int64_t p_voice_scope_id) {
    _TrackUpdate u;
    u.track_id = p_track_id;
    u.voice_scope_id = p_voice_scope_id;
    u.has_al_detune2 = true;
    u.al_detune2 = p_detune2;
    _mb_try_push(u);
}

// Thread-safe signal emission helper.
void SiONDriver::_emit_signal_thread_safe(const StringName &signal_name, const Variant &arg) {
    if (arg.get_type() == Variant::NIL) {
        call_deferred("emit_signal", signal_name);
    } else {
        call_deferred("emit_signal", signal_name, arg);
    }
}

// --- Mailbox ring helpers -----------------------------------------------------

bool SiONDriver::_mb_try_push(const _TrackUpdate &p_update) {
    int head = _mb_head.load(std::memory_order_relaxed);
    int tail = _mb_tail.load(std::memory_order_acquire);
    int next = (head + 1) & (_MB_CAPACITY - 1);
    if (next == tail) {
        // Ring full: drop oldest by advancing tail
        _mb_tail.store((tail + 1) & (_MB_CAPACITY - 1), std::memory_order_release);
    }
    _mb_ring[head] = p_update;
    _mb_head.store(next, std::memory_order_release);
    return true;
}

// --- Mailbox drain ------------------------------------------------------------
void SiONDriver::_drain_track_mailbox() {
    int tail = _mb_tail.load(std::memory_order_relaxed);
    int head = _mb_head.load(std::memory_order_acquire);
    while (tail != head) {
        const _TrackUpdate &u = _mb_ring[tail];
        tail = (tail + 1) & (_MB_CAPACITY - 1);
        // Apply to all live tracks that match this track_id (and optional voice scope)
        for (SiMMLTrack *trk : sequencer->get_tracks()) {
            if (!trk) continue;
            if (trk->get_track_id() != u.track_id) continue;
            if (u.voice_scope_id != -1) {
                // Skip tracks that do not match the voice scope id
                // (Only channels stamped with this voice should receive the update)
                if ((int64_t)trk->get_voice_scope_id() != u.voice_scope_id) continue;
            }
            SiOPMChannelBase *ch = trk->get_channel();
            if (!ch) continue;
            if (u.has_vol) {
                int vol128 = (int)Math::round(CLAMP(u.vol_linear, 0.0, 2.0) * 128.0);
                ch->set_master_volume(vol128);
            }
            if (u.has_pan) {
                ch->set_pan(CLAMP(u.pan, -64, 64));
            }
            // Merge and apply filter state
            if (u.has_filter || u.has_filter_type || u.has_filter_ar || u.has_filter_dr1 || u.has_filter_dr2 || u.has_filter_rr || u.has_filter_dc1 || u.has_filter_dc2 || u.has_filter_sc || u.has_filter_rc || u.has_filter_cutoff || u.has_filter_resonance) {
                _FilterState &fs = _ensure_filter_state(u.track_id);
                if (u.has_filter_type) fs.type = CLAMP(u.filter_type, 0, 2);
                if (u.has_filter) {
                    fs.cutoff = CLAMP(u.filter_cutoff, 0, 128);
                    fs.resonance = CLAMP(u.filter_resonance, 0, 9);
                }
                if (u.has_filter_cutoff) fs.cutoff = CLAMP(u.filter_cutoff, 0, 128);
                if (u.has_filter_resonance) fs.resonance = CLAMP(u.filter_resonance, 0, 9);
                if (u.has_filter_ar) fs.ar = CLAMP(u.filter_ar, 0, 255);
                if (u.has_filter_dr1) fs.dr1 = CLAMP(u.filter_dr1, 0, 255);
                if (u.has_filter_dr2) fs.dr2 = CLAMP(u.filter_dr2, 0, 255);
                if (u.has_filter_rr) fs.rr = CLAMP(u.filter_rr, 0, 255);
                if (u.has_filter_dc1) fs.dc1 = CLAMP(u.filter_dc1, 0, 128);
                if (u.has_filter_dc2) fs.dc2 = CLAMP(u.filter_dc2, 0, 128);
                if (u.has_filter_sc) fs.sc = CLAMP(u.filter_sc, 0, 128);
                if (u.has_filter_rc) fs.rc = CLAMP(u.filter_rc, 0, 128);
                // Decide apply policy: full restamp vs lightweight updates
                bool need_full_apply = u.has_filter || u.has_filter_type || u.has_filter_ar || u.has_filter_dr1 || u.has_filter_dr2 || u.has_filter_rr || u.has_filter_dc1 || u.has_filter_dc2 || u.has_filter_sc || u.has_filter_rc;
                auto restamp = [&]() {
                    fs.initialized = true;
                    ch->activate_filter(true);
                    ch->set_filter_type(fs.type);
                    ch->set_sv_filter(fs.cutoff, fs.resonance, fs.ar, fs.dr1, fs.dr2, fs.rr, fs.dc1, fs.dc2, fs.sc, fs.rc);
                };
                auto bootstrap_min = [&]() {
                    fs.initialized = true;
                    ch->activate_filter(true);
                    ch->set_filter_type(fs.type);
                };
                if (need_full_apply) {
                    restamp();
                    ch->set_filter_cutoff_now(fs.cutoff);
                } else if (u.has_filter_cutoff) {
                    // Bootstrap: only restamp if filter is not active yet
                    if (!fs.initialized || !ch->is_filter_active()) {
                        bootstrap_min();
                    } else {
                        fs.initialized = true;
                    }
                    ch->set_filter_cutoff_now(fs.cutoff);
                } else if (u.has_filter_resonance) {
                    // Bootstrap: only restamp if filter is not active yet
                    if (!fs.initialized || !ch->is_filter_active()) {
                        bootstrap_min();
                    } else {
                        fs.initialized = true;
                    }
                    // Lightweight: adjust resonance without re-stamp, with smoothing inside the channel
                    ch->set_filter_resonance_now(fs.resonance);
                }
            }
            if (u.has_ch_am) {
                ch->set_amplitude_modulation(u.ch_am_depth);
            }
            if (u.has_ch_pm) {
                ch->set_pitch_modulation(u.ch_pm_depth);
            }
            if (u.has_pitch_bend) {
                trk->set_pitch_bend(CLAMP(u.pitch_bend, -8192, 8191));
            }
            if (u.has_lfo_step) {
                ch->set_lfo_frequency_step(u.lfo_frequency_step);
            }
            if (u.has_env_freq_ratio) {
                ch->set_frequency_ratio(u.env_freq_ratio);
            }
            // FM operator updates and Analog-Like live params
            SiOPMChannelFM *fm = Object::cast_to<SiOPMChannelFM>(ch);
            if (fm) {
                if (u.has_fm_op_tl) {
                    fm->set_active_operator_index(CLAMP(u.op_index, 0, 3));
                    fm->set_total_level(u.fm_value);
                }
                if (u.has_fm_op_mul) {
                    fm->set_active_operator_index(CLAMP(u.op_index, 0, 3));
                    fm->set_multiple(u.fm_value);
                }
                if (u.has_fm_op_fmul) {
                    fm->set_active_operator_index(CLAMP(u.op_index, 0, 3));
                    fm->set_fine_multiple(u.fm_value);
                }
                if (u.has_fm_op_dt1) {
                    fm->set_active_operator_index(CLAMP(u.op_index, 0, 3));
                    fm->set_detune1(u.fm_value);
                }
                if (u.has_fm_op_dt2) {
                    fm->set_active_operator_index(CLAMP(u.op_index, 0, 3));
                    // Interpret fm_value as PTSS detune index (engine uses ptss_detune)
                    fm->set_detune(u.fm_value);
                }
                // Apply Analog-Like live params if present. These map to AL operator/channel fields.
                if (u.has_al_ws1) {
                    fm->set_active_operator_index(0);
                    Ref<SiOPMWaveTable> wt = SiOPMRefTable::get_instance()->get_wave_table(u.al_ws1);
                    SiONPitchTableType pt = wt.is_valid() ? wt->get_default_pitch_table_type() : (SiONPitchTableType)0;
                    fm->set_types(u.al_ws1, pt);
                }
                if (u.has_al_ws2) {
                    fm->set_active_operator_index(1);
                    Ref<SiOPMWaveTable> wt2 = SiOPMRefTable::get_instance()->get_wave_table(u.al_ws2);
                    SiONPitchTableType pt2 = wt2.is_valid() ? wt2->get_default_pitch_table_type() : (SiONPitchTableType)0;
                    fm->set_types(u.al_ws2, pt2);
                }
                if (u.has_al_balance) {
                    int bal = CLAMP(u.al_balance, -64, 64);
                    int (&level_table)[129] = SiOPMRefTable::get_instance()->eg_linear_to_total_level_table;
                    // Operator 0 TL = 64 - bal, Operator 1 TL = 64 + bal
                    fm->set_active_operator_index(0);
                    fm->set_total_level(level_table[64 - bal]);
                    fm->set_active_operator_index(1);
                    fm->set_total_level(level_table[bal + 64]);
                }
                if (u.has_al_detune2) {
                    // Apply as PTSS detune on operator 1 only (op1 is the second operator)
                    fm->set_active_operator_index(1);
                    fm->set_detune(u.al_detune2);
                }
            }
            if (u.has_lfo_wave) {
                ch->initialize_lfo(u.lfo_wave_shape); // resets LFO with new wave shape
            }
        }
    }
    _mb_tail.store(tail, std::memory_order_release);
}

SiONDriver::_FilterState &SiONDriver::_ensure_filter_state(int p_track_id) {
    if (!_filter_state_cache.has(p_track_id)) {
        _FilterState fresh;
        _filter_state_cache[p_track_id] = fresh;
    }
    return _filter_state_cache[p_track_id];
}
