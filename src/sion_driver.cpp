/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "sion_driver.h"
#include "engine_param_api.h"
#include "sion_stream.h"
#include "sion_stream_playback.h"
#include "utils/denormals.h"
#include <algorithm>
#include <chrono>
#include <thread>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/thread.hpp>

#include "sion_data.h"
#include "sion_enums.h"
#include "sion_voice.h"
#include "chip/channels/siopm_channel_base.h"
#include "chip/siopm_channel_params.h"
#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"
#include "chip/wave/siopm_wave_pcm_data.h"
#include "chip/wave/siopm_wave_pcm_table.h"
#include "chip/wave/siopm_wave_sampler_data.h"
#include "chip/wave/siopm_wave_table.h"
#include "effector/si_effector.h"
#include "effector/si_effect_stream.h"
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
#include "chip/channels/siopm_channel_sampler.h"
#include "chip/channels/siopm_channel_stream.h"
#include "chip/channels/siopm_channel_monolith.h"
#include "chip/channels/siopm_channel_iron.h"
#include "chip/channels/siopm_channel_talus.h"
#include "chip/channels/siopm_channel_ks.h"
#include "chip/channels/siopm_channel_guitar6.h"
#include "utils/fader_util.h"
#include "utils/transformer_util.h"
#include <atomic>

// TODO: Extract somewhere more manageable?
const char *SiONDriver::VERSION = "0.7.0.0"; // Original code was last versioned as 0.6.6.0.
const char *SiONDriver::VERSION_FLAVOR = "beta8";

SiONDriver *SiONDriver::_mutex = nullptr;
bool SiONDriver::_allow_multiple_drivers = false;

bool SiONDriver::_is_supported_backend_sample_rate(int p_sample_rate) {
	return p_sample_rate == 44100 || p_sample_rate == 48000;
}

int SiONDriver::get_backend_sample_rate() {
	Engine *engine = Engine::get_singleton();
	if (!engine || !engine->has_singleton(AudioServer::get_class_static())) {
		return 0;
	}

	AudioServer *audio_server = AudioServer::get_singleton();
	if (!audio_server) {
		return 0;
	}

	const int backend_sample_rate = (int)Math::round((double)audio_server->get_mix_rate());
	return backend_sample_rate > 0 ? backend_sample_rate : 0;
}

int SiONDriver::_resolve_supported_backend_sample_rate() {
	const int backend_sample_rate = get_backend_sample_rate();
	ERR_FAIL_COND_V_MSG(backend_sample_rate <= 0, 0, "SiONDriver: Failed to read the backend mix rate from AudioServer.");
	ERR_FAIL_COND_V_MSG(
			!_is_supported_backend_sample_rate(backend_sample_rate),
			0,
			vformat("SiONDriver: Audio backend mix rate %d Hz is unsupported. Supported backend output rates are 44100 Hz and 48000 Hz.", backend_sample_rate));
	return backend_sample_rate;
}

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

	SiMMLTrack *track = sequencer->create_controllable_track(internal_track_id, false);
	_bind_track_effect_stream(track, p_track_id);
	return track;
}

void SiONDriver::notify_user_defined_track(int p_event_trigger_id, int p_note) {
	SiONTrackEvent *event = memnew(SiONTrackEvent(SiONTrackEvent::USER_DEFINED, this, nullptr, sequencer->get_stream_writing_residue(), p_note, p_event_trigger_id));
	_track_event_queue.push_back(event);
}

// Background sound.

void SiONDriver::_set_background_sample(const Ref<AudioStream> &p_sound) {
	ERR_FAIL_COND_MSG(_is_render_live(), "SiONDriver: The background sample can only change while the render path is quiescent.");
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
		_background_sample_data->set_loop_enabled(_background_loop_point != -1);
		if (_background_loop_point != -1) {
			_background_sample_data->set_start_point(-1);
			_background_sample_data->set_end_point(-1);
			_background_sample_data->set_loop_point(int(_background_loop_point * _sample_rate));
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
	ERR_FAIL_COND_MSG(_is_render_live(), "SiONDriver: The background sample volume can only change while the render path is quiescent.");
	_background_voice->get_channel_params()->set_master_volume(0, p_value);
	if (_background_track) {
		_background_track->set_master_volume_linear(p_value);
	}
	if (_background_fade_out_track) {
		_background_fade_out_track->set_master_volume_linear(p_value);
	}
}

// Sound parameters.

int SiONDriver::get_max_track_count() const {
	return sequencer->get_max_track_count();
}

void SiONDriver::set_max_track_count(int p_value) {
	ERR_FAIL_COND_MSG(p_value < 1, "SiONDriver: Max track limit cannot be lower than 1.");

	sequencer->set_max_track_count(p_value);
}

void SiONDriver::_update_volume() {
	if (!_audio_player) {
		return;
	}
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
	return _published_bpm.load(std::memory_order_acquire);
}

void SiONDriver::set_bpm(double p_value) {
	// Scholars debate whether BPM even has the upper limit. At some point it definitely turns into tone for most people,
	// with no discernible beat in earshot. But having no limit at all for the API feels strange. Besides, we don't have
	// infinitely scalable performance. So as a compromise you can set the BPM to up to ???? beats per minute.
	// You're welcome!

	sequencer->set_effective_bpm(p_value);
	_published_bpm.store(sequencer->get_effective_bpm(), std::memory_order_release);
}

// Streaming and rendering.

double SiONDriver::get_streaming_position() const {
	return sequencer->get_processed_sample_count() * 1000.0 / _sample_rate;
}

int64_t SiONDriver::get_rendered_frame_count() const {
	return (int64_t)_rendered_frame_count.load(std::memory_order_relaxed);
}

void SiONDriver::_publish_frame_clock_anchor() {
	const int64_t host_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count();
	_frame_clock_sequence.fetch_add(1); // Odd: write in progress.
	_frame_clock_anchor_frame.store((int64_t)_rendered_frame_count.load(std::memory_order_relaxed));
	_frame_clock_anchor_host_time_ns.store(host_time_ns);
	_frame_clock_sequence.fetch_add(1); // Even: anchor consistent.
}

bool SiONDriver::get_frame_clock(PoolyFrameClockSnapshot &r_snapshot) const {
	// The render thread holds the sequence odd for two stores, so a few retries
	// are plenty; give up rather than spin if it keeps moving.
	for (int attempt = 0; attempt < 8; ++attempt) {
		const uint32_t sequence = _frame_clock_sequence.load();
		if (sequence == 0) {
			return false; // Nothing rendered yet.
		}
		if (sequence & 1u) {
			std::this_thread::yield();
			continue;
		}
		const int64_t frame = _frame_clock_anchor_frame.load();
		const int64_t host_time_ns = _frame_clock_anchor_host_time_ns.load();
		if (_frame_clock_sequence.load() != sequence) {
			continue;
		}
		r_snapshot.frame_position = frame;
		r_snapshot.host_time_ns = host_time_ns;
		r_snapshot.sample_rate = (int)_sample_rate;
		return true;
	}
	return false;
}

void SiONDriver::set_start_position(double p_value) {
	ERR_FAIL_COND_MSG(_is_render_live(), "SiONDriver: The start position can only change while the render path is quiescent.");
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
	_drain_commands();
	_process_one_block();

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

	_residual_buffer_frame_count = 0;
	_residual_frame_offset = 0;
	_rendered_frame_count.store(0, std::memory_order_relaxed);

	_is_streaming = true;
	_start_godot_output();

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

	// Any native consumer was stopped by its lifecycle owner; the driver stops its
	// own Godot output before anything the render owner holds changes hands.
	_stop_godot_output();

	_preserve_stop = false;
	_is_paused = false;
	_is_streaming = false;

	clear_data(); // Original SiON doesn't do that, but that seems like an oversight.
	clear_background_sample();
	_clear_processing();

	_fader->stop();
	_fader_volume = 1;

	_update_volume();
	sequencer->stop_sequence();

	_residual_buffer_frame_count = 0;
	_residual_frame_offset = 0;

	_dispatch_event(memnew(SiONEvent(SiONEvent::STREAM_STOPPED, this)));

	_performance_stats.streaming_latency = 0;
}

void SiONDriver::reset() {
	ERR_FAIL_COND_MSG(_is_render_live(), "SiONDriver: reset() needs a quiescent render path.");
	sequencer->reset_all_tracks();

	// Clear residual buffer to ensure clean state
	_residual_buffer_frame_count = 0;
	_residual_frame_offset = 0;
}

void SiONDriver::pause() {
	if (_is_streaming) {
		_is_paused = true;
	}
}

void SiONDriver::resume() {
	_is_paused = false;
}

void SiONDriver::set_note_on_exception_mode(ExceptionMode p_mode) {
	ERR_FAIL_INDEX(p_mode, NEM_MAX);

	_note_on_exception_mode = p_mode;
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
	SiMMLTrack *track = _find_or_create_track(p_track_id, p_delay, p_quant, p_disposable, &delay_samples);
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
	SiMMLTrack *track = _find_or_create_track(p_track_id, p_delay, p_quant, p_disposable, &delay_samples);
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
	SiMMLTrack *track = _find_or_create_track(p_track_id, p_delay, p_quant, p_disposable, &delay_samples);
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
	for (SiMMLTrack *track : sequencer->get_tracks_ref()) {
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

void SiONDriver::_start_godot_output() {
	if (!_godot_output_enabled) {
		return;
	}
	_audio_player->play();
	_audio_playback = _audio_player->get_stream_playback();
}

void SiONDriver::_stop_godot_output() {
	if (_audio_playback.is_null()) {
		return;
	}
	_audio_player->stop();
	// The player may still mix the stopped playback while it fades out. Cutting
	// the playback off from the driver under the audio server lock also waits out
	// a mix that is already inside render_interleaved().
	AudioServer *audio_server = AudioServer::get_singleton();
	audio_server->lock();
	_audio_playback->set_driver(nullptr);
	audio_server->unlock();
	_audio_playback = Ref<SiONStreamPlayback>();
}

bool SiONDriver::begin_offline_render() {
	ERR_FAIL_COND_V_MSG(_offline_render_active, false, "SiONDriver: An offline render is already active.");
	_offline_resumes_godot_output = _audio_playback.is_valid();
	_stop_godot_output();
	_offline_render_active = true;

	// The render starts from a clean block boundary.
	_residual_buffer_frame_count = 0;
	_residual_frame_offset = 0;
	return true;
}

void SiONDriver::render_offline_block(AudioFrame *p_buffer, int32_t p_frames) {
	_service_command_queue();
	generate_audio(p_buffer, p_frames);
}

void SiONDriver::end_offline_render() {
	ERR_FAIL_COND_MSG(!_offline_render_active, "SiONDriver: No offline render is active.");
	_offline_render_active = false;
	if (_offline_resumes_godot_output) {
		_start_godot_output();
	}
	_offline_resumes_godot_output = false;
}

int SiONDriver::convert_event_length(double p_length_16th) const {
	return (int)Math::round(_convert_event_length(p_length_16th));
}

TypedArray<SiMMLTrack> SiONDriver::sequence_on(const Ref<SiONData> &p_data, const Ref<SiONVoice> &p_voice, double p_length, double p_delay, double p_quant, int p_track_id, bool p_disposable) {
	ERR_FAIL_COND_V_MSG(_is_render_live(), TypedArray<SiMMLTrack>(), "SiONDriver: Sequences need a quiescent render path.");
	ERR_FAIL_COND_V(p_data.is_null(), TypedArray<SiMMLTrack>());
	ERR_FAIL_COND_V_MSG(p_length < 0, TypedArray<SiMMLTrack>(), "SiONDriver: Sequence length cannot be less than zero.");
	ERR_FAIL_COND_V_MSG(p_delay < 0, TypedArray<SiMMLTrack>(), "SiONDriver: Sequence delay cannot be less than zero.");

	Ref<BeatsPerMinute> bpm_settings = p_data->get_bpm_settings();
	if (bpm_settings.is_valid() && bpm_settings->get_bpm() > 0) {
		bpm_settings->update(bpm_settings->get_bpm(), _sample_rate);
	}

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
	ERR_FAIL_COND_V_MSG(_is_render_live(), TypedArray<SiMMLTrack>(), "SiONDriver: Sequences need a quiescent render path.");
	ERR_FAIL_COND_V_MSG(p_delay < 0, TypedArray<SiMMLTrack>(), "SiONDriver: Sequence off delay cannot be less than zero.");

	int internal_track_id = (p_track_id & SiMMLTrack::TRACK_ID_FILTER) | SiMMLTrack::DRIVER_SEQUENCE;
	int delay_samples = sequencer->calculate_sample_delay(0, p_delay, p_quant);

	TypedArray<SiMMLTrack> tracks;
	for (SiMMLTrack *track : sequencer->get_tracks_ref()) {
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
	// Every caller stops the driver first, so no consumer is inside a render call.

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

	_discard_commands();                             // Discard commands from the previous lifetime.

	sound_chip->initialize(_channel_num, _bitrate, _buffer_length);  // Initialize DSP.
	sound_chip->reset();                                             // Reset all channels.

	if (p_reset_effector) {                                          // Initialize or reset effectors.
		effector->initialize();
		_clear_track_effect_streams(false);
	} else {
		effector->reset();
		// Retained streams keep their registry; the old channels are no longer current.
		for (KeyValue<int, SiOPMChannelBase *> &entry : _track_effect_channels) {
			entry.value = nullptr;
		}
	}

	sequencer->prepare_process(_data, _sample_rate, _buffer_length); // Set sequencer tracks (should be called after sound_chip::reset()).
	// Reserve the current track capacity while the render path is quiescent.
	effector->reserve_local_effects(sequencer->get_max_track_count());
	_track_effect_channels.reserve(sequencer->get_max_track_count());
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

	_published_bpm.store(sequencer->get_effective_bpm(), std::memory_order_release);
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
	if (sound_chip && sequencer) {
		for (SiMMLTrack *trk : sequencer->get_tracks_ref()) {
			if (!trk) {
				continue;
			}
			SiOPMChannelBase *ch = trk->get_channel();
			if (!ch) {
				continue;
			}
			ch->update_lfo_for_bpm();
		}
	}
	_published_bpm.store(sequencer->get_effective_bpm(), std::memory_order_release);

	// MML tempo changes can still arrive on render; dispatch without touching
	// the main-thread frame queue.
	_dispatch_event(memnew(SiONTrackEvent(SiONTrackEvent::BPM_CHANGED, this, nullptr, p_buffer_index)));
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

void SiONDriver::set_timer_interval_beats(double p_length_beats) {
	ERR_FAIL_COND_MSG(p_length_beats < 0, "SiONDriver: Timer interval value cannot be less than zero.");

	_timer_interval_event->set_length(_convert_event_length(p_length_beats * 16.0));

	if (p_length_beats > 0) {
		sequencer->set_timer_callback(Callable(this, "_timer_callback"));
	} else {
		sequencer->set_timer_callback(Callable());
	}
}

// --- Output capture (for export/resampling) ---

bool SiONDriver::begin_output_capture(int p_max_seconds, bool p_post_master) {
	if (_capture_active) {
		ERR_PRINT("SiONDriver: Cannot start capture while already capturing.");
		return false;
	}

	size_t target_capacity = 0;
	if (p_max_seconds > 0) {
		// Stereo float samples: seconds * sample_rate * 2 channels.
		target_capacity = (size_t)p_max_seconds * (size_t)_sample_rate * 2;
	} else {
		// Unlimited capture still needs writable storage. Reuse current allocation
		// when possible, otherwise default to 10 seconds.
		const size_t default_unlimited_capacity = 10 * (size_t)_sample_rate * 2;
		target_capacity = std::max((size_t)_capture_buffer.size(), default_unlimited_capacity);
	}
	if ((size_t)_capture_buffer.size() != target_capacity) {
		_capture_buffer.resize(target_capacity);
	}
	_capture_post_master = p_post_master;
	_capture_write_pos = 0;
	_capture_active = true;
	return true;
}

PackedFloat32Array SiONDriver::end_output_capture() {
	if (!_capture_active) {
		return PackedFloat32Array();
	}

	// std::lock_guard<std::mutex> lock(_capture_mutex);
	_capture_active = false;

	// Return captured samples (up to write position)
	PackedFloat32Array result;
	result.resize(_capture_write_pos);
	for (size_t i = 0; i < _capture_write_pos; i++) {
		result[i] = _capture_buffer[i];
	}

	_capture_write_pos = 0;
	return result;
}

void SiONDriver::abort_output_capture() {
	if (!_capture_active) {
		return;
	}

	// std::lock_guard<std::mutex> lock(_capture_mutex);
	_capture_active = false;
	_capture_write_pos = 0;
}

bool SiONDriver::is_output_capturing() const {
	return _capture_active;
}

PackedFloat32Array SiONDriver::poll_output_capture_chunk(int p_max_frames) {
	if (!_capture_active || _capture_write_pos == 0) {
		return PackedFloat32Array();
	}

	// std::lock_guard<std::mutex> lock(_capture_mutex);

	size_t available_floats = _capture_write_pos;
	size_t frames_available = available_floats / 2;  // stereo

	if (p_max_frames > 0 && frames_available > (size_t)p_max_frames) {
		frames_available = p_max_frames;
	}

	size_t floats_to_return = frames_available * 2;

	PackedFloat32Array result;
	result.resize(floats_to_return);
	for (size_t i = 0; i < floats_to_return; i++) {
		result[i] = _capture_buffer[i];
	}

	// Shift remaining data to front
	size_t remaining = available_floats - floats_to_return;
	if (remaining > 0) {
		memmove(_capture_buffer.ptrw(),
				_capture_buffer.ptr() + floats_to_return,
				remaining * sizeof(float));
	}
	_capture_write_pos = remaining;

	return result;
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
	SiONDriver *driver = memnew(SiONDriver(p_buffer_length, p_channel_num, p_sample_rate, p_bitrate));
	if (driver && !driver->_is_initialized) {
		memdelete(driver);
		return nullptr;
	}
	return driver;
}

SiONDriver *SiONDriver::create_with_config(const RuntimeConfig &p_config) {
	SiONDriver *driver = memnew(SiONDriver(p_config.engine_block_frames, p_config.output_channels, p_config.preferred_sample_rate, 0));
	if (driver && !driver->_is_initialized) {
		memdelete(driver);
		return nullptr;
	}

	if (driver && !p_config.create_godot_output) {
		driver->_godot_output_enabled = false;
		if (driver->_audio_player) {
			driver->_audio_player->stop();
			driver->remove_child(driver->_audio_player);
			memdelete(driver->_audio_player);
			driver->_audio_player = nullptr;
		}
		driver->_audio_stream.unref();
		driver->_audio_playback.unref();
	}

	if (driver && p_config.actual_sample_rate != (int)driver->_sample_rate) {
		driver->_sample_rate = p_config.actual_sample_rate;
	}

	return driver;
}

SiONDriver *SiONDriver::create_native(int p_engine_block_frames, int p_output_channels, int p_preferred_sample_rate, int p_actual_sample_rate, bool p_create_godot_output) {
	RuntimeConfig config;
	config.engine_block_frames = p_engine_block_frames;
	config.output_channels = p_output_channels;
	config.preferred_sample_rate = p_preferred_sample_rate;
	config.actual_sample_rate = p_actual_sample_rate;
	config.create_godot_output = p_create_godot_output;
	return create_with_config(config);
}

int64_t SiONDriver::get_render_client_handle() {
	return reinterpret_cast<int64_t>(static_cast<PoolyRenderClient *>(this));
}

int64_t SiONDriver::get_timing_client_handle() {
	return reinterpret_cast<int64_t>(static_cast<PoolyTimingClient *>(this));
}

void SiONDriver::stream_without_output(bool p_reset_effector) {
	stop();
	_prepare_process(nullptr, p_reset_effector);

	_performance_stats.total_processing_time = 0;
	_performance_stats.processing_time_data->reset();

	_is_paused = false;
	_is_finish_sequence_dispatched = true;

	_residual_buffer_frame_count = 0;
	_residual_frame_offset = 0;
	_rendered_frame_count.store(0, std::memory_order_relaxed);

	_is_streaming = true;
	_set_processing_immediate();
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
	ClassDB::bind_static_method("SiONDriver", D_METHOD("get_backend_sample_rate"), &SiONDriver::get_backend_sample_rate);

	// Factory.

	ClassDB::bind_static_method("SiONDriver", D_METHOD("create", "buffer_size", "channel_num", "sample_rate", "bitrate"), &SiONDriver::create, DEFVAL(512), DEFVAL(2), DEFVAL(48000), DEFVAL(0));
	ClassDB::bind_static_method("SiONDriver", D_METHOD("create_native", "engine_block_frames", "output_channels", "preferred_sample_rate", "actual_sample_rate", "create_godot_output"), &SiONDriver::create_native, DEFVAL(2), DEFVAL(48000), DEFVAL(48000), DEFVAL(false));

	// Internal components. The sound chip, effector and sequencer belong to the
	// render owner while streaming and are not exposed to scripts.

	ClassDB::bind_method(D_METHOD("get_audio_player"), &SiONDriver::get_audio_player);
	ClassDB::bind_method(D_METHOD("get_audio_stream"), &SiONDriver::get_audio_stream);
	ClassDB::bind_method(D_METHOD("get_audio_playback"), &SiONDriver::get_audio_playback);

	// Mailbox bindings
	ClassDB::bind_method(D_METHOD("mailbox_set_track_volume", "track_id", "linear_volume", "entity_scope_id", "slot_scope_id"), &SiONDriver::mailbox_set_track_volume, DEFVAL(-1), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_track_pan", "track_id", "pan", "entity_scope_id", "slot_scope_id"), &SiONDriver::mailbox_set_track_pan, DEFVAL(-1), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_fm_operator_count", "track_id", "operator_count", "algorithm", "analog_like", "feedback", "feedback_connection", "entity_scope_id", "slot_scope_id"), &SiONDriver::mailbox_set_fm_operator_count, DEFVAL(-1), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_set_engine_param", "track_id", "engine_kind_id", "param_id", "value", "index", "entity_scope_id", "slot_scope_id"), &SiONDriver::mailbox_set_engine_param, DEFVAL(0), DEFVAL(-1), DEFVAL(-1));
	// Stream channel params (thread-safe via mailbox)
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_gain", "track_id", "gain"), &SiONDriver::mailbox_stream_set_gain);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_pan", "track_id", "pan"), &SiONDriver::mailbox_stream_set_pan);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_pitch_cents", "track_id", "cents"), &SiONDriver::mailbox_stream_set_pitch_cents);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_fade_in", "track_id", "frames"), &SiONDriver::mailbox_stream_set_fade_in);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_fade_out", "track_id", "frames"), &SiONDriver::mailbox_stream_set_fade_out);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_in_sample", "track_id", "sample"), &SiONDriver::mailbox_stream_set_in_sample);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_out_sample", "track_id", "sample"), &SiONDriver::mailbox_stream_set_out_sample);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_warp_mode", "track_id", "mode"), &SiONDriver::mailbox_stream_set_warp_mode);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_clip_bpm", "track_id", "bpm"), &SiONDriver::mailbox_stream_set_clip_bpm);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_grain_size", "track_id", "grain_size"), &SiONDriver::mailbox_stream_set_grain_size);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_flux", "track_id", "flux"), &SiONDriver::mailbox_stream_set_flux);
	ClassDB::bind_method(D_METHOD("mailbox_stream_seek", "track_id", "position_sample", "track_instance_id"), &SiONDriver::mailbox_stream_seek, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_looping", "track_id", "looping"), &SiONDriver::mailbox_stream_set_looping);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_loop_region", "track_id", "start_sample", "end_sample"), &SiONDriver::mailbox_stream_set_loop_region);
	ClassDB::bind_method(D_METHOD("mailbox_stream_set_clip_envelope", "track_id", "clip_time_beats", "fade_in_beats", "fade_out_start_beats", "clip_end_beats"), &SiONDriver::mailbox_stream_set_clip_envelope);
	// Note control (thread-safe) - track_instance_id targets specific track by Godot object ID
	ClassDB::bind_method(D_METHOD("mailbox_key_on", "track_id", "note", "tick_length", "key_velocity_16", "release_velocity_16", "track_instance_id", "legato", "glide_from_note"), &SiONDriver::mailbox_key_on, DEFVAL(0), DEFVAL(-1), DEFVAL(-1), DEFVAL(0), DEFVAL(false), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_stream_key_on", "track_id", "note", "tick_length", "start_sample", "track_instance_id"), &SiONDriver::mailbox_stream_key_on, DEFVAL(0), DEFVAL(-1), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("mailbox_key_off", "track_id", "immediate", "track_instance_id", "delay_16th_beats"), &SiONDriver::mailbox_key_off, DEFVAL(false), DEFVAL(0), DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("mailbox_stream_key_off", "track_id", "track_instance_id"), &SiONDriver::mailbox_stream_key_off, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("mailbox_set_expression", "track_id", "value", "track_instance_id"), &SiONDriver::mailbox_set_expression, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("mailbox_set_velocity", "track_id", "value", "track_instance_id"), &SiONDriver::mailbox_set_velocity, DEFVAL(0));
	// Track lifetime and setup (thread-safe), each addressed to one linked track instance
	ClassDB::bind_method(D_METHOD("mailbox_retire_track", "track_id", "track_instance_id"), &SiONDriver::mailbox_retire_track);

	// Track effects (thread-safe via mailbox)
	ClassDB::bind_method(D_METHOD("mailbox_track_effects_set_chain", "track_id", "slots"), &SiONDriver::mailbox_track_effects_set_chain);
	ClassDB::bind_method(D_METHOD("mailbox_track_effects_insert_effect", "track_id", "slot", "index"), &SiONDriver::mailbox_track_effects_insert_effect, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("mailbox_track_effects_remove_effect", "track_id", "index"), &SiONDriver::mailbox_track_effects_remove_effect);
	ClassDB::bind_method(D_METHOD("mailbox_track_effects_swap_effects", "track_id", "index_a", "index_b"), &SiONDriver::mailbox_track_effects_swap_effects);
	ClassDB::bind_method(D_METHOD("mailbox_track_effects_set_effect_args", "track_id", "index", "args"), &SiONDriver::mailbox_track_effects_set_effect_args);
	ClassDB::bind_method(D_METHOD("mailbox_track_effects_set_effect_arg", "track_id", "fx_index", "arg_index", "value"), &SiONDriver::mailbox_track_effects_set_effect_arg);
	ClassDB::bind_method(D_METHOD("mailbox_track_effects_set_bypass", "track_id", "index", "bypassed"), &SiONDriver::mailbox_track_effects_set_bypass);
	ClassDB::bind_method(D_METHOD("mailbox_track_effects_set_mute", "track_id", "mute"), &SiONDriver::mailbox_track_effects_set_mute);

	// User-controllable track API
	ClassDB::bind_method(D_METHOD("create_user_controllable_track", "track_id"), &SiONDriver::create_user_controllable_track, DEFVAL(0));

	// Configuration.

	ClassDB::bind_method(D_METHOD("get_max_track_count"), &SiONDriver::get_max_track_count);
	ClassDB::bind_method(D_METHOD("set_max_track_count", "value"), &SiONDriver::set_max_track_count);

	ClassDB::add_property("SiONDriver", PropertyInfo(Variant::INT, "max_track_count"), "set_max_track_count", "get_max_track_count");

	ClassDB::bind_method(D_METHOD("get_buffer_length"), &SiONDriver::get_buffer_length);
	ClassDB::bind_method(D_METHOD("get_channel_num"), &SiONDriver::get_channel_num);
	ClassDB::bind_method(D_METHOD("get_preferred_sample_rate"), &SiONDriver::get_preferred_sample_rate);
	ClassDB::bind_method(D_METHOD("get_sample_rate"), &SiONDriver::get_sample_rate);
	ClassDB::bind_method(D_METHOD("get_streaming_position"), &SiONDriver::get_streaming_position);
	ClassDB::bind_method(D_METHOD("get_rendered_frame_count"), &SiONDriver::get_rendered_frame_count);
	ClassDB::bind_method(D_METHOD("get_bitrate"), &SiONDriver::get_bitrate);

	ClassDB::bind_method(D_METHOD("get_volume"), &SiONDriver::get_volume);
	ClassDB::bind_method(D_METHOD("set_volume", "value"), &SiONDriver::set_volume);
	ClassDB::bind_method(D_METHOD("get_bpm"), &SiONDriver::get_bpm);
	ClassDB::bind_method(D_METHOD("set_bpm", "bpm"), &SiONDriver::set_bpm);

	ClassDB::add_property("SiONDriver", PropertyInfo(Variant::FLOAT, "volume"), "set_volume", "get_volume");
	ClassDB::add_property("SiONDriver", PropertyInfo(Variant::FLOAT, "bpm"), "set_bpm", "get_bpm");

	// Events.

	ClassDB::bind_method(D_METHOD("set_beat_callback_interval", "length_16th"), &SiONDriver::set_beat_callback_interval);
	ClassDB::bind_method(D_METHOD("set_timer_interval_beats", "length_beats"), &SiONDriver::set_timer_interval_beats);

	// Output capture (for export/resampling).
	ClassDB::bind_method(D_METHOD("begin_output_capture", "max_seconds", "post_master"), &SiONDriver::begin_output_capture, DEFVAL(0), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("end_output_capture"), &SiONDriver::end_output_capture);
	ClassDB::bind_method(D_METHOD("abort_output_capture"), &SiONDriver::abort_output_capture);
	ClassDB::bind_method(D_METHOD("is_output_capturing"), &SiONDriver::is_output_capturing);
	ClassDB::bind_method(D_METHOD("poll_output_capture_chunk", "max_frames"), &SiONDriver::poll_output_capture_chunk, DEFVAL(0));

	// Metering API (professional post-effects, post-fader metering).
	ClassDB::bind_method(D_METHOD("set_denormal_flush_enabled", "enabled"), &SiONDriver::set_denormal_flush_enabled);
	ClassDB::bind_method(D_METHOD("is_denormal_flush_enabled"), &SiONDriver::is_denormal_flush_enabled);

	ClassDB::bind_method(D_METHOD("set_metering_enabled", "enabled"), &SiONDriver::set_metering_enabled);
	ClassDB::bind_method(D_METHOD("is_metering_enabled"), &SiONDriver::is_metering_enabled);
	ClassDB::bind_method(D_METHOD("set_meter_downsample_factor", "factor"), &SiONDriver::set_meter_downsample_factor);
	ClassDB::bind_method(D_METHOD("get_meter_downsample_factor"), &SiONDriver::get_meter_downsample_factor);
	ClassDB::bind_method(D_METHOD("take_master_meter"), &SiONDriver::take_master_meter);
	ClassDB::bind_method(D_METHOD("take_track_meter", "track_id"), &SiONDriver::take_track_meter);

	ClassDB::bind_method(D_METHOD("set_beat_event_enabled", "enabled"), &SiONDriver::set_beat_event_enabled);
	ClassDB::bind_method(D_METHOD("set_stream_event_enabled", "enabled"), &SiONDriver::set_stream_event_enabled);
	ClassDB::bind_method(D_METHOD("set_fading_event_enabled", "enabled"), &SiONDriver::set_fading_event_enabled);

	// Processing, compiling, rendering.

	ClassDB::bind_method(D_METHOD("compile", "mml"), &SiONDriver::compile);
	ClassDB::bind_method(D_METHOD("queue_compile", "mml"), &SiONDriver::queue_compile);

	ClassDB::bind_method(D_METHOD("render", "data", "buffer_size", "buffer_channel_num", "reset_effector"), &SiONDriver::render, DEFVAL(2), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("queue_render", "data", "buffer_size", "buffer_channel_num", "reset_effector"), &SiONDriver::queue_render, DEFVAL(2), DEFVAL(false));

	ClassDB::bind_method(D_METHOD("stream", "reset_effector"), &SiONDriver::stream, DEFVAL(true));
	// Native-backend streaming: enter streaming mode without the Godot AudioStreamPlayer.
	ClassDB::bind_method(D_METHOD("stream_without_output", "reset_effector"), &SiONDriver::stream_without_output, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("is_godot_output_enabled"), &SiONDriver::is_godot_output_enabled);
	ClassDB::bind_method(D_METHOD("get_render_client_handle"), &SiONDriver::get_render_client_handle);
	ClassDB::bind_method(D_METHOD("get_timing_client_handle"), &SiONDriver::get_timing_client_handle);
	ClassDB::bind_method(D_METHOD("play", "data", "reset_effector"), &SiONDriver::play, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("stop"), &SiONDriver::stop);
	ClassDB::bind_method(D_METHOD("reset"), &SiONDriver::reset);
	ClassDB::bind_method(D_METHOD("pause"), &SiONDriver::pause);
	ClassDB::bind_method(D_METHOD("resume"), &SiONDriver::resume);

	ClassDB::bind_method(D_METHOD("is_streaming"), &SiONDriver::is_streaming);
	ClassDB::bind_method(D_METHOD("is_paused"), &SiONDriver::is_paused);

	ClassDB::bind_method(D_METHOD("convert_event_length", "length_16th"), &SiONDriver::convert_event_length);

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
	BIND_ENUM_CONSTANT(CHIP_GUITAR6);
	BIND_ENUM_CONSTANT(CHIP_STRATA);
	BIND_ENUM_CONSTANT(CHIP_MONOLITH);
	BIND_ENUM_CONSTANT(CHIP_IRON);
	BIND_ENUM_CONSTANT(CHIP_TALUS);
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
	BIND_ENUM_CONSTANT(MODULE_STREAM);
	BIND_ENUM_CONSTANT(MODULE_GUITAR6);
	BIND_ENUM_CONSTANT(MODULE_STRATA);
	BIND_ENUM_CONSTANT(MODULE_MONOLITH);
	BIND_ENUM_CONSTANT(MODULE_IRON);
	BIND_ENUM_CONSTANT(MODULE_TALUS);
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
	// Check if buffer length is a power of 2 and between 32 and 8192
	bool is_valid_buffer_length = p_buffer_length >= 2 && p_buffer_length <= 8192 && (p_buffer_length & (p_buffer_length - 1)) == 0;
	ERR_FAIL_COND_MSG(!is_valid_buffer_length, "SiONDriver: Buffer length must be a power of 2 between 32 and 8192 (e.g., 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192).");
	ERR_FAIL_COND_MSG((p_channel_num != 1 && p_channel_num != 2), "SiONDriver: Channel number can only be 1 (mono) or 2 (stereo).");
	ERR_FAIL_COND_MSG(!_is_supported_backend_sample_rate(p_sample_rate), "SiONDriver: Preferred sample rate can only be 44100 or 48000.");

	const int actual_sample_rate = _resolve_supported_backend_sample_rate();
	if (actual_sample_rate <= 0) {
		return;
	}

	_preferred_sample_rate = p_sample_rate;
	_sample_rate = actual_sample_rate;
	_bitrate = p_bitrate;
	_buffer_length = p_buffer_length;
	_channel_num = p_channel_num;

	// Ensure reference tables use the actual backend rate before any chip initialization.
	{
		SiOPMRefTable *ref_table = SiOPMRefTable::get_instance();
		if (!ref_table || ref_table->sampling_rate != actual_sample_rate) {
			SiOPMRefTable::finalize();
			memnew(SiOPMRefTable(3580000, 1789772.5, actual_sample_rate));
		}
	}
	ERR_FAIL_COND_MSG(!_allow_multiple_drivers && _mutex, "SiONDriver: Only one driver instance is allowed.");
	_mutex = this;

	sound_chip = memnew(SiOPMSoundChip);
	effector = memnew(SiEffector(sound_chip));
	sequencer = memnew(SiMMLSequencer(sound_chip));
	sound_chip->set_sequencer(sequencer);
	sequencer->set_note_on_callback(Callable(this, "_note_on_callback"));
	sequencer->set_note_off_callback(Callable(this, "_note_off_callback"));
	sequencer->set_tempo_changed_callback(Callable(this, "_tempo_changed_callback"));
	sequencer->set_beat_callback(Callable(this, "_beat_callback"));
	_published_bpm.store(sequencer->get_effective_bpm(), std::memory_order_release);

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
		_timer_sequence = memnew(MMLSequence);
		_timer_sequence->initialize();
		_timer_sequence->append_new_event(MMLEvent::REPEAT_ALL, 0);
		_timer_sequence->append_new_event(MMLEvent::TIMER, 0);
		_timer_interval_event = _timer_sequence->append_new_event(MMLEvent::GLOBAL_WAIT, 0);
	}

	_performance_stats.processing_time_data = memnew(SinglyLinkedList<int>(TIME_AVERAGING_COUNT, 0, true));
	_performance_stats.total_processing_time_ratio = _sample_rate / (_buffer_length * TIME_AVERAGING_COUNT);

	// AUDIO THREAD SAFETY: Preallocate buffers to avoid dynamic allocation in generate_audio()
	// Residual buffer needs to hold one full processing block (stereo samples)
	_residual_buffer.resize(_buffer_length * 2);
	_residual_buffer_frame_count = 0;
	_residual_frame_offset = 0;
	
	// Capture buffer: preallocate for 10 seconds max (or configurable max)
	// 10 seconds * sample_rate * 2 channels = reasonable upper bound
	_capture_buffer.resize(10 * _sample_rate * 2);
	_capture_write_pos = 0;
	_is_initialized = true;
}

SiONDriver::~SiONDriver() {
	if (_mutex == this) {
		_mutex = nullptr;
	}

	_timer_interval_event = nullptr;
	memdelete(_timer_sequence);

	memdelete(_fader);
	memdelete(_background_fader);

	// The lifecycle owner stopped every render consumer before freeing the driver.
	{
		// Discard old queued work, then delete the registered streams.
		_discard_commands();
		_clear_track_effect_streams(true);

		// Delete audio processing objects
		memdelete(sequencer);
		memdelete(effector);
		memdelete(sound_chip);

		// Set pointers to nullptr so generate_audio() can detect deletion
		sequencer = nullptr;
		effector = nullptr;
		sound_chip = nullptr;
	}
}

int SiONDriver::render_interleaved(float *p_output, int p_frames, int p_channels) {
	if (p_frames <= 0 || p_channels < 1) {
		return 0;
	}

	_publish_frame_clock_anchor();

	// FTZ/DAZ are per-thread CPU state, so they have to be armed on whichever thread
	// is doing the rendering -- the audio callback thread live, or the caller's thread
	// during an offline render. This is the single entry point both go through.
	sion::set_denormals_flushed(_denormal_flush_enabled.load(std::memory_order_relaxed));

	if (!effector || !sequencer || !sound_chip) {
		memset(p_output, 0, sizeof(float) * p_frames * p_channels);
		_rendered_frame_count.fetch_add((uint64_t)p_frames, std::memory_order_relaxed);
		return p_frames;
	}

	int frames_generated = 0;
	const int block = _buffer_length;

	while (frames_generated < p_frames) {
		if (_residual_buffer_frame_count == 0) {
			// Residual frames were rendered before these commands; only a new block
			// sees them.
			_drain_commands();
			_process_one_block();

			Vector<double> *out_buf = sound_chip->get_output_buffer_ptr();

			if (_metering_enabled.load(std::memory_order_relaxed)) {
				if (++_meter_downsample_counter >= _meter_downsample_factor.load(std::memory_order_relaxed)) {
					_meter_downsample_counter = 0;
					_meter_all_track_outputs(block);
					_update_batch_meters(out_buf, block);
				}
			}

			_residual_buffer_frame_count = block;
			_residual_frame_offset = 0;
			int sample_count = block * 2;
			for (int i = 0; i < sample_count; ++i) {
				_residual_buffer.write[i] = (*out_buf)[i];
			}
		}

		int frames_available = _residual_buffer_frame_count - _residual_frame_offset;
		int frames_to_copy = std::min(frames_available, p_frames - frames_generated);

		int out_offset = frames_generated * p_channels;
		for (int i = 0; i < frames_to_copy; ++i) {
			int sample_index = (_residual_frame_offset + i) * 2;
			float left = (float)_residual_buffer[sample_index];
			float right = (float)_residual_buffer[sample_index + 1];

			if (p_channels == 2) {
				p_output[out_offset + i * 2] = left;
				p_output[out_offset + i * 2 + 1] = right;
			} else {
				p_output[out_offset + i] = (left + right) * 0.5f;
			}
		}

		if (_capture_active) {
			size_t needed_size = _capture_write_pos + (frames_to_copy * 2);
			if (needed_size > (size_t)_capture_buffer.size()) {
				ERR_PRINT_ONCE("SiONDriver: Capture buffer overflow! Increase pre-allocation size.");
			} else {
				for (int i = 0; i < frames_to_copy; ++i) {
					int sample_index = (_residual_frame_offset + i) * 2;
					_capture_buffer.write[_capture_write_pos++] = (float)_residual_buffer[sample_index];
					_capture_buffer.write[_capture_write_pos++] = (float)_residual_buffer[sample_index + 1];
				}
			}
		}

		_residual_frame_offset += frames_to_copy;
		frames_generated += frames_to_copy;

		if (_residual_frame_offset >= _residual_buffer_frame_count) {
			_residual_buffer_frame_count = 0;
			_residual_frame_offset = 0;
		}
	}

	_rendered_frame_count.fetch_add((uint64_t)frames_generated, std::memory_order_relaxed);
	return frames_generated;
}

int32_t SiONDriver::generate_audio(AudioFrame *p_buffer, int32_t p_frames) {
	if (p_frames <= 0) {
		return 0;
	}

	if (!effector || !sequencer || !sound_chip) {
		memset(p_buffer, 0, sizeof(AudioFrame) * p_frames);
		return p_frames;
	}

	// Use a stack-allocated interleaved buffer for small frame counts,
	// fall back to heap for large requests (unlikely in practice).
	constexpr int STACK_LIMIT = 2048;
	float stack_buf[STACK_LIMIT * 2];
	float *interleaved = (p_frames <= STACK_LIMIT) ? stack_buf : new float[p_frames * 2];

	int written = render_interleaved(interleaved, p_frames, 2);

	for (int i = 0; i < written; ++i) {
		p_buffer[i].left = interleaved[i * 2];
		p_buffer[i].right = interleaved[i * 2 + 1];
	}

	for (int i = written; i < p_frames; ++i) {
		p_buffer[i].left = 0.0f;
		p_buffer[i].right = 0.0f;
	}

	if (interleaved != stack_buf) {
		delete[] interleaved;
	}

	return written;
}

// Pull model: audio is delivered via generate_audio() inside _mix(). The frame
// hook only services the command queue.
void SiONDriver::_streaming() {
	_service_command_queue();
}

// --- Professional Metering Implementation ------------------------------------

void SiONDriver::_update_batch_meters(const Vector<double> *out_buf, int frames) {
	if (!out_buf || frames <= 0) {
		return;
	}

	float peak_left = 0.0f;
	float peak_right = 0.0f;
	double sum_sq_left = 0.0;
	double sum_sq_right = 0.0;

	// Process all samples in the buffer for accurate RMS and peak detection
	for (int i = 0; i < frames; ++i) {
		float left = (float)(*out_buf)[i * 2];
		float right = (float)(*out_buf)[i * 2 + 1];

		// Track peaks (absolute max)
		peak_left = std::max(peak_left, fabsf(left));
		peak_right = std::max(peak_right, fabsf(right));

		// Accumulate for RMS
		sum_sq_left += left * left;
		sum_sq_right += right * right;
	}

	_master_meter.rms_left.store(sqrtf(float(sum_sq_left / frames)), std::memory_order_relaxed);
	_master_meter.rms_right.store(sqrtf(float(sum_sq_right / frames)), std::memory_order_relaxed);
	_master_meter.peak_left.raise(peak_left);
	_master_meter.peak_right.raise(peak_right);
}

void SiONDriver::_meter_track_output(MeterState &p_meter, const Vector<double> *track_buf, int frames, double p_post_fader_gain, int p_post_pan) {
	if (!_metering_enabled.load(std::memory_order_relaxed)) {
		return;
	}

	if (!track_buf || frames <= 0) {
		return;
	}

	double (&pan_table)[129] = SiOPMRefTable::get_instance()->pan_table;
	const int clamped_pan = CLAMP(p_post_pan, 0, 128);
	const double post_fader_gain = std::max(p_post_fader_gain, 0.0);
	const double gain_l = pan_table[128 - clamped_pan] * post_fader_gain;
	const double gain_r = pan_table[clamped_pan] * post_fader_gain;

	float peak_left = 0.0f;
	float peak_right = 0.0f;
	double sum_sq_left = 0.0;
	double sum_sq_right = 0.0;

	for (int i = 0; i < frames; ++i) {
		float left = fabsf((float)((*track_buf)[i * 2] * gain_l));
		float right = fabsf((float)((*track_buf)[i * 2 + 1] * gain_r));

		peak_left = std::max(peak_left, left);
		peak_right = std::max(peak_right, right);

		sum_sq_left += left * left;
		sum_sq_right += right * right;
	}

	p_meter.rms_left.store(sqrtf(float(sum_sq_left / frames)), std::memory_order_relaxed);
	p_meter.rms_right.store(sqrtf(float(sum_sq_right / frames)), std::memory_order_relaxed);
	p_meter.peak_left.raise(peak_left);
	p_meter.peak_right.raise(peak_right);
}

void SiONDriver::_meter_all_track_outputs(int frames) {
	if (!_metering_enabled.load(std::memory_order_relaxed)) {
		return;
	}

	// Iterate through all track effect streams
	for (const KeyValue<int, SiEffectStream *> &entry : _track_effect_streams) {
		SiEffectStream *stream = entry.value;

		if (!stream) {
			continue;
		}

		// Get the stream's output buffer (post-effects)
		SiOPMStream *opm_stream = stream->get_stream();
		if (!opm_stream) {
			continue;
		}

		Vector<double> *track_buf = opm_stream->get_buffer_ptr();
		if (!track_buf || track_buf->is_empty()) {
			continue;
		}

		// Verify buffer has enough samples for the requested frames
		if (track_buf->size() < frames * 2) {
			continue;
		}

		// Meter this track's post-effects output
		_meter_track_output(stream->get_meter(), track_buf, frames, stream->get_post_fader_gain(), stream->get_post_pan());
	}
}

void SiONDriver::set_denormal_flush_enabled(bool p_enabled) {
	_denormal_flush_enabled.store(p_enabled, std::memory_order_release);
}

bool SiONDriver::is_denormal_flush_enabled() const {
	return _denormal_flush_enabled.load(std::memory_order_acquire);
}

void SiONDriver::set_metering_enabled(bool p_enabled) {
	_metering_enabled.store(p_enabled, std::memory_order_release);
}

bool SiONDriver::is_metering_enabled() const {
	return _metering_enabled.load(std::memory_order_acquire);
}

void SiONDriver::set_meter_downsample_factor(int p_factor) {
	int clamped = CLAMP(p_factor, 1, 16);
	_meter_downsample_factor.store(clamped, std::memory_order_release);
}

int SiONDriver::get_meter_downsample_factor() const {
	return _meter_downsample_factor.load(std::memory_order_acquire);
}

Dictionary SiONDriver::take_master_meter() {
	return _master_meter.take();
}

Dictionary SiONDriver::take_track_meter(int p_track_id) {
	// Main-thread lookup of a registered effect stream.
	SiEffectStream **stream = _track_effect_streams.getptr(p_track_id);
	if (!stream) {
		return Dictionary();  // Track has no effect stream yet
	}
	return (*stream)->get_meter().take();
}

// --- Mailbox setters (main thread) --------------------------------------------
void SiONDriver::mailbox_set_track_volume(int p_track_id, double p_linear_volume, int64_t p_entity_scope_id, int64_t p_slot_scope_id) {
    _DriverCommand u;
    u.track_id = p_track_id;
    u.entity_scope_id = p_entity_scope_id;
    u.slot_scope_id = p_slot_scope_id;
    u.has_vol = true;
    u.vol_linear = p_linear_volume;
    _push_command(u);
}

void SiONDriver::mailbox_set_track_pan(int p_track_id, int p_pan, int64_t p_entity_scope_id, int64_t p_slot_scope_id) {
    _DriverCommand u;
    u.track_id = p_track_id;
    u.entity_scope_id = p_entity_scope_id;
    u.slot_scope_id = p_slot_scope_id;
    u.has_pan = true;
    u.pan = p_pan;
    _push_command(u);
}

void SiONDriver::mailbox_set_fm_operator_count(int p_track_id, int p_operator_count, int p_algorithm, bool p_analog_like, int p_feedback, int p_feedback_connection, int64_t p_entity_scope_id, int64_t p_slot_scope_id) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.entity_scope_id = p_entity_scope_id;
	u.slot_scope_id = p_slot_scope_id;
	u.has_fm_operator_count = true;
	u.fm_operator_count = p_operator_count;
	u.fm_algorithm = p_algorithm;
	u.fm_analog_like = p_analog_like;
	u.fm_feedback = p_feedback;
	u.fm_feedback_connection = p_feedback_connection;
	_push_command(u);
}

void SiONDriver::mailbox_set_engine_param(int p_track_id, int p_engine_kind_id, int p_param_id, const Variant &p_value, int p_index, int64_t p_entity_scope_id, int64_t p_slot_scope_id) {
	const EngineParamSchema *schema = EngineParamSchema::find_engine(p_engine_kind_id);
	ERR_FAIL_NULL_MSG(schema, "Engine kind has no registered scalar parameter contract.");
	const EngineParamDescriptor *d = schema->find(p_param_id);
	ERR_FAIL_NULL_MSG(d, "Unknown engine parameter ID.");
	ERR_FAIL_COND_MSG(!d->realtime, "Engine parameter does not support live updates.");
	ERR_FAIL_COND_MSG(!d->has_instance(p_index), "Engine parameter index is outside its registered instances.");
	_DriverCommand u;
	u.op = _DriverCommand::OP_ENGINE_PARAM;
	u.track_id = p_track_id;
	u.entity_scope_id = p_entity_scope_id;
	u.slot_scope_id = p_slot_scope_id;
	u.engine_kind_id = p_engine_kind_id;
	u.engine_param_id = p_param_id;
	u.engine_param_index = p_index;
	if (!decode_engine_param(*d, p_value, u.engine_value)) return;
	_push_command(u);
}

// --- Stream channel mailbox methods -------------------------------------------

void SiONDriver::mailbox_stream_set_gain(int p_track_id, double p_gain) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_gain = true;
	u.stream_gain = p_gain;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_pan(int p_track_id, int p_pan) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_pan = true;
	u.stream_pan = p_pan;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_pitch_cents(int p_track_id, int p_cents) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_pitch_cents = true;
	u.stream_pitch_cents = p_cents;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_fade_in(int p_track_id, int p_frames) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_fade_in = true;
	u.stream_fade_in = p_frames;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_fade_out(int p_track_id, int p_frames) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_fade_out = true;
	u.stream_fade_out = p_frames;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_in_sample(int p_track_id, int64_t p_sample) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_in_sample = true;
	u.stream_in_sample = p_sample;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_out_sample(int p_track_id, int64_t p_sample) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_out_sample = true;
	u.stream_out_sample = p_sample;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_warp_mode(int p_track_id, int p_mode) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_warp_mode = true;
	u.stream_warp_mode = p_mode;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_grain_size(int p_track_id, double p_grain_size) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_grain_size = true;
	u.stream_grain_size = p_grain_size;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_flux(int p_track_id, double p_flux) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_flux = true;
	u.stream_flux = p_flux;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_clip_bpm(int p_track_id, double p_bpm) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_clip_bpm = true;
	u.stream_clip_bpm = p_bpm;
	_push_command(u);
}


void SiONDriver::mailbox_stream_seek(int p_track_id, int64_t p_position_sample, uint64_t p_track_instance_id) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.track_instance_id = p_track_instance_id;
	u.has_stream_seek = true;
	u.stream_seek_sample = p_position_sample;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_looping(int p_track_id, bool p_looping) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_looping = true;
	u.stream_looping = p_looping;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_loop_region(int p_track_id, int64_t p_start_sample, int64_t p_end_sample) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_loop_region = true;
	u.stream_loop_start_sample = p_start_sample;
	u.stream_loop_end_sample = p_end_sample;
	_push_command(u);
}

void SiONDriver::mailbox_stream_set_clip_envelope(int p_track_id, double p_clip_time_beats, double p_fade_in_beats, double p_fade_out_start_beats, double p_clip_end_beats) {
	_DriverCommand u;
	u.track_id = p_track_id;
	u.has_stream_clip_envelope = true;
	u.stream_clip_time_beats = p_clip_time_beats;
	u.stream_clip_fade_in_beats = p_fade_in_beats;
	u.stream_clip_fade_out_start_beats = p_fade_out_start_beats;
	u.stream_clip_end_beats = p_clip_end_beats;
	_push_command(u);
}

void SiONDriver::mailbox_key_on(int p_track_id, int p_note, int p_tick_length, int p_key_velocity_16, int p_release_velocity_16, uint64_t p_track_instance_id, bool p_legato, int p_glide_from_note) {
    _DriverCommand u;
    u.track_id = p_track_id;
    u.track_instance_id = p_track_instance_id;
    u.has_key_on = true;
    u.key_on_note = p_note;
    u.key_on_length = p_tick_length;
    u.key_velocity_16 = p_key_velocity_16;
    u.release_velocity_16 = p_release_velocity_16;
    u.key_on_legato = p_legato;
    u.key_on_glide_from_note = p_glide_from_note;
    _push_command(u);
}

void SiONDriver::mailbox_stream_key_on(int p_track_id, int p_note, int p_tick_length, int64_t p_start_sample, uint64_t p_track_instance_id) {
    _DriverCommand u;
    u.track_id = p_track_id;
    u.track_instance_id = p_track_instance_id;
    u.has_key_on = true;
    u.key_on_note = p_note;
    u.key_on_length = p_tick_length;
    if (p_start_sample >= 0) {
        u.has_key_on_stream_start_sample = true;
        u.key_on_stream_start_sample = p_start_sample;
    }
    _push_command(u);
}

void SiONDriver::mailbox_key_off(int p_track_id, bool p_immediate, uint64_t p_track_instance_id, double p_delay_16th_beats) {
    _DriverCommand u;
    u.track_id = p_track_id;
    u.track_instance_id = p_track_instance_id;
    u.has_key_off = true;
    u.key_off_immediate = p_immediate;
    u.key_off_delay_16th_beats = MAX(0.0, p_delay_16th_beats);
    _push_command(u);
}

void SiONDriver::mailbox_stream_key_off(int p_track_id, uint64_t p_track_instance_id) {
    _DriverCommand u;
    u.track_id = p_track_id;
    u.track_instance_id = p_track_instance_id;
    u.has_stream_key_off = true;
    _push_command(u);
}

void SiONDriver::mailbox_set_expression(int p_track_id, int p_value, uint64_t p_track_instance_id) {
    _DriverCommand u;
    u.track_id = p_track_id;
    u.track_instance_id = p_track_instance_id;
    u.has_expression = true;
    u.expression_value = p_value;
    _push_command(u);
}

void SiONDriver::mailbox_set_velocity(int p_track_id, int p_value, uint64_t p_track_instance_id) {
    _DriverCommand u;
    u.track_id = p_track_id;
    u.track_instance_id = p_track_instance_id;
    u.has_velocity = true;
    u.velocity_value = p_value;
    _push_command(u);
}

void SiONDriver::mailbox_retire_track(int p_track_id, uint64_t p_track_instance_id) {
	_DriverCommand u;
	u.op = _DriverCommand::OP_RETIRE_TRACK;
	u.track_id = p_track_id;
	u.track_instance_id = p_track_instance_id;
	_push_command(u);
}

void SiONDriver::mailbox_track_effects_set_effect_args(int p_track_id, int p_index, const Variant &p_args) {
	ERR_FAIL_COND_MSG(p_track_id < 0, vformat("SiONDriver: Invalid track id %d for mailbox track effects.", p_track_id));
	_ensure_track_effect_stream(p_track_id);
	_DriverCommand u;
	u.op = _DriverCommand::OP_EFFECT;
	u.fx_op = _DriverCommand::FX_OP_SET_ARGS;
	u.track_id = p_track_id;
	u.fx_index = p_index;
	u.fx_args = _args_from_variant(p_args);
	_push_command(u);
}

void SiONDriver::mailbox_track_effects_set_effect_arg(int p_track_id, int p_fx_index, int p_arg_index, double p_value) {
	ERR_FAIL_COND_MSG(p_track_id < 0, vformat("SiONDriver: Invalid track id %d for mailbox FX scalar arg.", p_track_id));
	_ensure_track_effect_stream(p_track_id);
	_DriverCommand u;
	u.op = _DriverCommand::OP_EFFECT;
	u.fx_op = _DriverCommand::FX_OP_SET_ARG;
	u.track_id = p_track_id;
	u.fx_index = p_fx_index;
	u.fx_arg_index = p_arg_index;
	u.fx_arg_value = p_value;
	_push_command(u);
}

void SiONDriver::mailbox_track_effects_set_bypass(int p_track_id, int p_index, bool p_bypassed) {
	ERR_FAIL_COND_MSG(p_track_id < 0, vformat("SiONDriver: Invalid track id %d for mailbox track effects.", p_track_id));
	_ensure_track_effect_stream(p_track_id);
	_DriverCommand u;
	u.op = _DriverCommand::OP_EFFECT;
	u.fx_op = _DriverCommand::FX_OP_SET_BYPASS;
	u.track_id = p_track_id;
	u.fx_index = p_index;
	u.fx_bypassed = p_bypassed;
	_push_command(u);
}

void SiONDriver::mailbox_track_effects_set_mute(int p_track_id, bool p_mute) {
	_ensure_track_effect_stream(p_track_id);
	_DriverCommand u;
	u.op = _DriverCommand::OP_EFFECT;
	u.fx_op = _DriverCommand::FX_OP_SET_MUTE;
	u.track_id = p_track_id;
	u.fx_muted = p_mute;
	_push_command(u);
}

void SiONDriver::mailbox_track_effects_set_chain(int p_track_id, const Array &p_slots) {
	ERR_FAIL_COND_MSG(p_track_id < 0, vformat("SiONDriver: Invalid track id %d for mailbox track effects.", p_track_id));
	_ensure_track_effect_stream(p_track_id);
	_DriverCommand u;
	u.op = _DriverCommand::OP_EFFECT;
	u.fx_op = _DriverCommand::FX_OP_SET_CHAIN;
	u.track_id = p_track_id;
	for (int i = 0; i < p_slots.size(); i++) {
		const Dictionary slot = p_slots[i];
		Ref<SiEffectBase> effect = _prepare_effect(slot);
		if (effect.is_null()) {
			continue; // Unknown kind, reported by the effector.
		}
		u.fx_chain.push_back(effect);
		u.fx_chain_bypassed.push_back(bool(slot.get("bypassed", false)));
	}
	_push_command(u);
}

void SiONDriver::mailbox_track_effects_insert_effect(int p_track_id, const Dictionary &p_slot, int p_index) {
	ERR_FAIL_COND_MSG(p_track_id < 0, vformat("SiONDriver: Invalid track id %d for mailbox track effects.", p_track_id));
	Ref<SiEffectBase> effect = _prepare_effect(p_slot);
	if (effect.is_null()) {
		return; // Unknown kind, reported by the effector.
	}
	_ensure_track_effect_stream(p_track_id);
	_DriverCommand u;
	u.op = _DriverCommand::OP_EFFECT;
	u.fx_op = _DriverCommand::FX_OP_INSERT;
	u.track_id = p_track_id;
	u.fx_index = p_index;
	u.fx_effect = effect;
	_push_command(u);
}

void SiONDriver::mailbox_track_effects_remove_effect(int p_track_id, int p_index) {
	ERR_FAIL_COND_MSG(p_track_id < 0, vformat("SiONDriver: Invalid track id %d for mailbox track effects.", p_track_id));
	_ensure_track_effect_stream(p_track_id);
	_DriverCommand u;
	u.op = _DriverCommand::OP_EFFECT;
	u.fx_op = _DriverCommand::FX_OP_REMOVE;
	u.track_id = p_track_id;
	u.fx_index = p_index;
	_push_command(u);
}

void SiONDriver::mailbox_track_effects_swap_effects(int p_track_id, int p_index_a, int p_index_b) {
	ERR_FAIL_COND_MSG(p_track_id < 0, vformat("SiONDriver: Invalid track id %d for mailbox track effects.", p_track_id));
	_ensure_track_effect_stream(p_track_id);
	_DriverCommand u;
	u.op = _DriverCommand::OP_EFFECT;
	u.fx_op = _DriverCommand::FX_OP_SWAP;
	u.track_id = p_track_id;
	u.fx_index = p_index_a;
	u.fx_index_b = p_index_b;
	_push_command(u);
}

// Thread-safe signal emission helper.
void SiONDriver::_emit_signal_thread_safe(const StringName &signal_name, const Variant &arg) {
    if (arg.get_type() == Variant::NIL) {
        call_deferred("emit_signal", signal_name);
    } else {
        call_deferred("emit_signal", signal_name, arg);
    }
}

// --- Driver command queue -----------------------------------------------------

void SiONDriver::_push_command(_DriverCommand &p_command) {
	// A command never overtakes one that waits in the backlog.
	_flush_command_backlog();
	if (!_command_backlog.is_empty() || !_enqueue_command(p_command)) {
		_command_backlog.push_back(p_command);
		p_command = _DriverCommand();
	}
}

bool SiONDriver::_enqueue_command(_DriverCommand &p_command) {
	const int head = _mb_head.load(std::memory_order_relaxed);
	const int next = (head + 1) & (_MB_CAPACITY - 1);
	if (next == _mb_tail.load(std::memory_order_acquire)) {
		return false;
	}
	// Move before publication: the consumer receives sole chain storage.
	_mb_ring[head] = std::move(p_command);
	_mb_head.store(next, std::memory_order_release);
	return true;
}

void SiONDriver::_flush_command_backlog() {
	while (!_command_backlog.is_empty() && _enqueue_command(_command_backlog.front()->get())) {
		_command_backlog.pop_front();
	}
}

void SiONDriver::_service_command_queue() {
	// A payload only the main thread still holds is one the render owner let go of.
	LocalVector<RefCounted *> released;
	for (const KeyValue<RefCounted *, Ref<RefCounted>> &E : _render_payloads) {
		if (E.value->get_reference_count() == 1) {
			released.push_back(E.key);
		}
	}
	for (RefCounted *payload : released) {
		_render_payloads.erase(payload);
	}
	_flush_command_backlog();
}

void SiONDriver::_retain_render_payload(const Ref<RefCounted> &p_payload) {
	if (p_payload.is_valid()) {
		_render_payloads.insert(p_payload.ptr(), p_payload);
	}
}

void SiONDriver::_drain_commands() {
	int tail = _mb_tail.load(std::memory_order_relaxed);
	const int head = _mb_head.load(std::memory_order_acquire);
	while (tail != head) {
		_DriverCommand &command = _mb_ring[tail];
		_apply_command(command);
		// Let go of consumed payloads now rather than when this slot is reused, so
		// the main thread can release them. It still holds each one.
		command.fx_effect.unref();
		tail = (tail + 1) & (_MB_CAPACITY - 1);
	}
	_mb_tail.store(tail, std::memory_order_release);
}

void SiONDriver::_discard_commands() {
	_command_backlog.clear();
	// Quiescent: discard queued work from the previous driver lifetime.
	for (_DriverCommand &command : _mb_ring) {
		command = _DriverCommand();
	}
	_mb_head.store(0, std::memory_order_relaxed);
	_mb_tail.store(0, std::memory_order_relaxed);
}

void SiONDriver::_apply_command(_DriverCommand &p_command) {
	switch (p_command.op) {
		case _DriverCommand::OP_TRACK_UPDATE: {
			_apply_track_update(p_command);
		} break;
		case _DriverCommand::OP_ENGINE_PARAM: {
			_apply_engine_param(p_command);
		} break;

		case _DriverCommand::OP_EFFECT: {
			_apply_effect_command(p_command);
		} break;

		case _DriverCommand::OP_RETIRE_TRACK: {
			_get_linked_track(p_command.track_instance_id)->retire();
		} break;
	}
}

SiMMLTrack *SiONDriver::_get_linked_track(uint64_t p_track_instance_id) const {
	for (SiMMLTrack *track : sequencer->get_tracks_ref()) {
		if (track->get_instance_id() == p_track_instance_id) {
			return track;
		}
	}
	ERR_FAIL_V_MSG(nullptr, "SiONDriver: A command addressed a track that is not linked. Tracks are addressable from their creation until their retirement.");
}

void SiONDriver::_apply_effect_command(_DriverCommand &p_command) {
	SiEffectStream *stream = _get_track_effect_stream(p_command.track_id);

	switch (p_command.fx_op) {
		case _DriverCommand::FX_OP_SET_CHAIN: {
			stream->set_chain(std::move(p_command.fx_chain));
			stream->prepare_process();
			// Apply the bypass snapshot (also clears any stale bypass values).
			for (int i = 0; i < p_command.fx_chain_bypassed.size(); i++) {
				stream->set_effect_bypass(i, p_command.fx_chain_bypassed[i]);
			}
		} break;

		case _DriverCommand::FX_OP_INSERT: {
			int insert_index = p_command.fx_index;
			if (insert_index < 0 || insert_index > stream->get_effect_count()) {
				insert_index = stream->get_effect_count();
			}
			stream->insert_effect(insert_index, p_command.fx_effect);
			stream->prepare_process();
		} break;

		case _DriverCommand::FX_OP_REMOVE: {
			stream->remove_effect(p_command.fx_index);
			stream->prepare_process();
		} break;

		case _DriverCommand::FX_OP_SWAP: {
			stream->swap_effects(p_command.fx_index, p_command.fx_index_b);
			stream->prepare_process();
		} break;

		case _DriverCommand::FX_OP_SET_ARGS: {
			stream->set_effect_args(p_command.fx_index, p_command.fx_args);
		} break;

		case _DriverCommand::FX_OP_SET_ARG: {
			stream->set_effect_arg(p_command.fx_index, p_command.fx_arg_index, p_command.fx_arg_value);
		} break;

		case _DriverCommand::FX_OP_SET_BYPASS: {
			stream->set_effect_bypass(p_command.fx_index, p_command.fx_bypassed);
		} break;

		case _DriverCommand::FX_OP_SET_MUTE: {
			stream->set_mute(p_command.fx_muted);
		} break;
	}
}

// The scalar and legacy lanes use the same exact scope selection. A retired
// track remains linked until its queue-ordered retirement, but receives no edit.
static bool matches_param_scope(const SiMMLTrack &p_track, int p_track_id, int64_t p_entity_scope_id, int64_t p_slot_scope_id) {
	return p_track.get_track_id() == p_track_id
		&& (p_entity_scope_id == -1 || p_track.get_entity_scope_id() == p_entity_scope_id)
		&& (p_slot_scope_id == -1 || p_track.get_slot_scope_id() == p_slot_scope_id);
}

void SiONDriver::_apply_engine_param(const _DriverCommand &u) {
	for (SiMMLTrack *track : sequencer->get_tracks_ref()) {
		if (track->is_pending_disposal()) continue;
		if (!matches_param_scope(*track, u.track_id, u.entity_scope_id, u.slot_scope_id)) continue;
		// Parameter IDs belong to an engine kind. A track-wide write skips leaves of other engines.
		if (track->get_engine_kind_id() != u.engine_kind_id) continue;
		track->apply_engine_param(u.engine_param_id, u.engine_param_index, u.engine_value);
	}
}

void SiONDriver::_apply_track_update(const _DriverCommand &u) {
	int key_off_delay_samples = 0;
	if (u.has_key_off && u.key_off_delay_16th_beats > 0.0) {
		key_off_delay_samples = (int)Math::round(sequencer->calculate_sample_length(u.key_off_delay_16th_beats));
	}

    // Apply to all live tracks that match this track_id (and optional entity/slot scopes)
    // AUDIO THREAD SAFETY: Use get_tracks_ref() to avoid vector copy allocation
    const LocalVector<SiMMLTrack *> &tracks = sequencer->get_tracks_ref();
    for (SiMMLTrack *trk : tracks) {
        if (!trk) continue;
        if (!matches_param_scope(*trk, u.track_id, u.entity_scope_id, u.slot_scope_id)) continue;
        SiOPMChannelBase *ch = trk->get_channel();
        if (!ch) continue;
        if (u.has_vol) {
            // Allow up to 2.0× (200%) for hot mixing headroom.
            // This lets individual tracks exceed 0dBFS during mixing; downstream gain staging
            // (driver volume / Godot buses) is expected to keep the final output in range.
            ch->set_master_volume_linear(CLAMP(u.vol_linear, 0.0, 2.0));
        }
        if (u.has_pan) {
            ch->set_pan(CLAMP(u.pan, -64, 64));
        }
        if (u.has_fm_operator_count) {
            SiOPMChannelFM *fm = Object::cast_to<SiOPMChannelFM>(ch);
            if (fm) {
                fm->set_algorithm(u.fm_operator_count, u.fm_analog_like, u.fm_algorithm);
                fm->set_feedback(u.fm_feedback, u.fm_feedback_connection);
            }
        }
        // Stream channel updates (apply to SiOPMChannelStream).
        SiOPMChannelStream *stream_ch = Object::cast_to<SiOPMChannelStream>(ch);
        if (stream_ch) {
            if (u.has_stream_gain) {
                stream_ch->set_stream_gain(u.stream_gain);
            }
            if (u.has_stream_pan) {
                stream_ch->set_stream_pan(u.stream_pan);
            }
            if (u.has_stream_pitch_cents) {
                stream_ch->set_stream_pitch_cents(u.stream_pitch_cents);
            }
            if (u.has_stream_fade_in) {
                stream_ch->set_stream_fade_in(u.stream_fade_in);
            }
            if (u.has_stream_fade_out) {
                stream_ch->set_stream_fade_out(u.stream_fade_out);
            }
            if (u.has_stream_in_sample) {
                stream_ch->set_stream_in_sample(u.stream_in_sample);
            }
            if (u.has_stream_out_sample) {
                stream_ch->set_stream_out_sample(u.stream_out_sample);
            }
            if (u.has_stream_warp_mode) {
                stream_ch->set_stream_warp_mode(u.stream_warp_mode);
            }
            if (u.has_stream_clip_bpm) {
                stream_ch->set_stream_clip_bpm(u.stream_clip_bpm);
            }
            if (u.has_stream_grain_size) {
                stream_ch->set_stream_grain_size(u.stream_grain_size);
            }
            if (u.has_stream_flux) {
                stream_ch->set_stream_flux(u.stream_flux);
            }
            if (u.has_stream_looping) {
                stream_ch->set_stream_looping(u.stream_looping);
            }
            if (u.has_stream_loop_region) {
                stream_ch->set_stream_loop_region(u.stream_loop_start_sample, u.stream_loop_end_sample);
            }
            if (u.has_stream_clip_envelope) {
                stream_ch->set_stream_clip_envelope(
                    u.stream_clip_time_beats,
                    u.stream_clip_fade_in_beats,
                    u.stream_clip_fade_out_start_beats,
                    u.stream_clip_end_beats
                );
            }
        }
        // Note control commands - these can target specific track instances
        bool instance_match = (u.track_instance_id == 0 || trk->get_instance_id() == u.track_instance_id);
        if (instance_match) {
            // Live seek (immediate transport reposition, instance-scoped).
            if (u.has_stream_seek) {
                SiOPMChannelStream *seek_ch = Object::cast_to<SiOPMChannelStream>(ch);
                if (seek_ch) {
                    seek_ch->seek_to(u.stream_seek_sample);
                }
            }
            if (u.has_key_on) {
                // If a stream start sample is bundled with this key-on,
                // set it as pending context so the deferred _key_on() path
                // calls note_on_at() instead of plain note_on().
                if (u.has_key_on_stream_start_sample) {
                    trk->set_pending_key_on_stream_start(u.key_on_stream_start_sample);
                }
				// TODO: this needs to be cleaned up, maybe a real key on velocity
                if (u.key_velocity_16 >= 0) {
                    const int key_expression = (int)(((int64_t)CLAMP(u.key_velocity_16, 0, 65535) * 127 + 32767) / 65535);
                    trk->set_expression(CLAMP(key_expression, 0, 128));
                }
                if (u.key_on_glide_from_note >= 0) {
                    trk->set_pending_key_on_glide_from(u.key_on_glide_from_note);
                }
                trk->key_on(u.key_on_note, u.key_on_length, 0, u.key_on_legato);
            }
            if (u.has_stream_key_off) {
                trk->stream_key_off();
            }
            if (u.has_key_off) {
                trk->key_off(key_off_delay_samples, u.key_off_immediate);
            }
            if (u.has_expression) {
                trk->set_expression(CLAMP(u.expression_value, 0, 128));
            }
            if (u.has_velocity) {
                trk->set_velocity(CLAMP(u.velocity_value, 0, 512));
            }
        }
    }
}

// --- Track effect streams -----------------------------------------------------

SiEffectStream *SiONDriver::_get_track_effect_stream(int p_track_id) {
	if (p_track_id < 0 || !_track_effect_streams.has(p_track_id)) {
		return nullptr;
	}
	return _track_effect_streams[p_track_id];
}

void SiONDriver::_bind_track_effect_stream(SiMMLTrack *p_track, int p_track_id) {
	if (!p_track) {
		return;
	}
	SiEffectStream *stream = _ensure_track_effect_stream(p_track_id);
	if (!stream) {
		return;
	}
	SiOPMChannelBase *channel = p_track->get_channel();
	if (!channel) {
		return;
	}
	_track_effect_channels[p_track_id] = channel;
	stream->set_post_fader_gain(channel->get_stream_send(0));
	stream->set_post_pan(CLAMP(channel->get_pan(), -64, 64) + 64);
	channel->set_stream_buffer(0, stream->get_stream());
}

SiEffectStream *SiONDriver::_ensure_track_effect_stream(int p_track_id) {
	ERR_FAIL_COND_V_MSG(p_track_id < 0, nullptr, vformat("SiONDriver: Invalid track id %d for effect stream.", p_track_id));
	if (!effector) {
		return nullptr;
	}
	if (_track_effect_streams.has(p_track_id)) {
		return _track_effect_streams[p_track_id];
	}

	Vector<Ref<SiEffectBase>> empty_chain;
	SiEffectStream *stream = effector->create_local_effect(0, empty_chain);
	if (!stream) {
		ERR_PRINT(vformat("SiONDriver: Failed to allocate effect stream for track %d.", p_track_id));
		return nullptr;
	}
	_track_effect_streams[p_track_id] = stream;
	if (!_track_effect_channels.has(p_track_id)) {
		// Keep channel map keys in sync with stream keys so the audio thread can
		// update pointers without inserting into the map.
		_track_effect_channels[p_track_id] = nullptr;
	}
	return stream;
}

void SiONDriver::_process_one_block() {
	sound_chip->begin_process();
	effector->begin_process();
	sequencer->process();
	_update_track_effect_post_fader();
	effector->end_process();
	sound_chip->end_process();
}

void SiONDriver::_update_track_effect_post_fader() {
	// Pass 1: mark all effect-track channel slots unresolved for this block.
	// We only touch existing keys to avoid map insertions in the audio thread.
	for (const KeyValue<int, SiEffectStream *> &entry : _track_effect_streams) {
		SiOPMChannelBase **slot = _track_effect_channels.getptr(entry.key);
		if (slot) {
			*slot = nullptr;
		}
	}

	// Pass 2: resolve current channels in one scan over live tracks.
	// Reverse iteration preserves previous "latest track wins" behavior.
	const LocalVector<SiMMLTrack *> &tracks = sequencer->get_tracks_ref();
	for (int i = tracks.size() - 1; i >= 0; i--) {
		SiMMLTrack *track = tracks[i];
		if (!track) {
			continue;
		}

		const int track_id = track->get_track_id();
		SiOPMChannelBase **slot = _track_effect_channels.getptr(track_id);
		if (!slot || *slot != nullptr) {
			continue;
		}

		SiOPMChannelBase *candidate = track->get_channel();
		if (!candidate) {
			continue;
		}
		*slot = candidate;
	}

	// Pass 3: apply post-fader/pan using resolved channels.
	for (const KeyValue<int, SiEffectStream *> &entry : _track_effect_streams) {
		SiEffectStream *stream = entry.value;
		if (!stream) {
			continue;
		}

		SiOPMChannelBase **channel_ptr = _track_effect_channels.getptr(entry.key);
		if (!channel_ptr || !*channel_ptr) {
			continue;
		}
		SiOPMChannelBase *channel = *channel_ptr;

		// Keep stream routing attached to the currently resolved channel.
		channel->set_stream_buffer(0, stream->get_stream());
		stream->set_post_fader_gain(channel->get_stream_send(0));
		stream->set_post_pan(CLAMP(channel->get_pan(), -64, 64) + 64);
	}
}

void SiONDriver::_clear_track_effect_streams(bool p_delete_streams) {
	if (p_delete_streams && effector) {
		for (const KeyValue<int, SiEffectStream *> &entry : _track_effect_streams) {
			if (entry.value) {
				effector->delete_local_effect(entry.value);
			}
		}
	}
	_track_effect_streams.clear();
	_track_effect_channels.clear();
}

Vector<double> SiONDriver::_args_from_variant(const Variant &p_value) const {
	Vector<double> args;
	switch (p_value.get_type()) {
		case Variant::PACKED_FLOAT64_ARRAY: {
			PackedFloat64Array arr = p_value;
			args.resize(arr.size());
			for (int i = 0; i < arr.size(); i++) {
				args.write[i] = arr[i];
			}
		} break;
		case Variant::PACKED_FLOAT32_ARRAY: {
			PackedFloat32Array arr = p_value;
			args.resize(arr.size());
			for (int i = 0; i < arr.size(); i++) {
				args.write[i] = arr[i];
			}
		} break;
		case Variant::ARRAY: {
			Array arr = p_value;
			args.resize(arr.size());
			for (int i = 0; i < arr.size(); i++) {
				args.write[i] = (double)arr[i];
			}
		} break;
		case Variant::NIL:
			break;
		default: {
			args.resize(1);
			args.write[0] = (double)p_value;
		} break;
	}
	return args;
}

Ref<SiEffectBase> SiONDriver::_prepare_effect(const Dictionary &p_slot) {
	Ref<SiEffectBase> effect = SiEffector::get_effect_instance(p_slot.get("kind", String()));
	if (effect.is_valid()) {
		effect->set_by_mml(_args_from_variant(p_slot.get("args", Variant())));
		// Preparing allocates the effect's buffers, here rather than on the render owner.
		effect->prepare_process();
		_retain_render_payload(effect);
	}
	return effect;
}
