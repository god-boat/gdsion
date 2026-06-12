/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_wave_sampler_data.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/classes/audio_stream.hpp>

#include "sion_enums.h"
#include <cmath>
#include "templates/singly_linked_list.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include "utils/transformer_util.h"

using namespace godot;

static constexpr int kDEFAULT_SAMPLE_RATE = 48000;
static constexpr int kSAMPLER_GAIN_MIN_DB = -36;
static constexpr int kSAMPLER_GAIN_MAX_DB = 36;

static _FORCE_INLINE_ int _get_sampler_boundary_fade_samples(int p_sample_rate, int p_window_length) {
	if (p_window_length <= 0) {
		return 0;
	}

	const int rate = (p_sample_rate > 0) ? p_sample_rate : kDEFAULT_SAMPLE_RATE;
	const int base_fade_samples = MAX(1, (int)Math::round(((double)rate * 128.0) / 48000.0));
	return MIN(base_fade_samples, p_window_length);
}

void SiOPMWaveSamplerData::_prepare_wave_data(const Variant &p_data, int p_src_channel_count, int p_channel_count) {
	int source_channels = CLAMP(p_src_channel_count, 1, 2);
	int target_channels = (p_channel_count == 0 ? source_channels : CLAMP(p_channel_count, 1, 2));

	Variant::Type data_type = p_data.get_type();
	switch (data_type) {
		case Variant::PACKED_FLOAT32_ARRAY: {
			// TODO: If someday Vector<T> and Packed*Arrays become friends, this can be simplified.
			Vector<double> raw_data;
			for (double value : (PackedFloat32Array)p_data) {
				raw_data.append(value);
			}

			_wave_data = TransformerUtil::transform_sampler_data(raw_data, source_channels, target_channels);
		} break;

		case Variant::OBJECT: {
			Ref<AudioStream> audio_stream = p_data;
			if (audio_stream.is_valid()) {
				Vector<double> raw_data = SiOPMWaveBase::extract_wave_data(audio_stream, &source_channels);

				int src_rate = kDEFAULT_SAMPLE_RATE;
				if (audio_stream->has_method("get_mix_rate")) {
					Variant v_rate = audio_stream->call("get_mix_rate");
					if (v_rate.get_type() == Variant::INT) {
						src_rate = (int)v_rate;
					} else if (v_rate.get_type() == Variant::FLOAT) {
						src_rate = (int)((double)v_rate);
					}
				}
				_sample_rate = (src_rate > 0) ? src_rate : kDEFAULT_SAMPLE_RATE;

				if (p_channel_count == 0) { // Update if necessary.
					target_channels = source_channels;
				}

				_wave_data = TransformerUtil::transform_sampler_data(raw_data, source_channels, target_channels);
				break;
			}

			ERR_FAIL_MSG("SiOPMWaveSamplerData: Unsupported data type.");
		} break;

		case Variant::NIL: {
			// Nothing to do.
		} break;

		default: {
			ERR_FAIL_MSG("SiOPMWaveSamplerData: Unsupported data type.");
		} break;
	}

	_channel_count = target_channels;
	_end_point = get_length();
	_cache_effective_window_defaults();
}

int SiOPMWaveSamplerData::_get_samples_for_duration_ms(double p_ms, int p_fallback) const {
	if (p_ms <= 0.0) {
		return MAX(1, p_fallback);
	}

	const int rate = (_sample_rate > 0) ? _sample_rate : kDEFAULT_SAMPLE_RATE;
	const int samples = (int)Math::round((double)rate * p_ms * 0.001);
	return MAX(1, samples);
}

int SiOPMWaveSamplerData::get_length() const {
	if (_channel_count > 0) {
		return _wave_data.size() >> (_channel_count - 1);
	}

	return 0;
}

void SiOPMWaveSamplerData::set_ignore_note_off(bool p_ignore) {
	_ignore_note_off = p_ignore;
}

//

SiOPMWaveSamplerData::PlaybackWindow SiOPMWaveSamplerData::resolve_playback_window() const {
	PlaybackWindow window;

	const int length = get_length();
	if (length <= 0) {
		return window;
	}

	const int resolved_start = (_start_point >= 0) ? _start_point : _auto_start_point;
	window.start_point = CLAMP(resolved_start, 0, length - 1);

	int resolved_end = (_end_point >= 0) ? _end_point : _auto_end_point;
	resolved_end = CLAMP(resolved_end, 0, length);
	if (resolved_end <= window.start_point) {
		// Keep authored values untouched, but avoid collapsing playback to a 1-sample
		// window when start/end are authored independently or temporarily cross over.
		resolved_end = length;
	}
	window.end_point = MAX(window.start_point + 1, resolved_end);

	if (_loop_point >= 0) {
		const int resolved_loop = CLAMP(_loop_point, 0, length - 1);
		if (resolved_loop >= window.start_point && resolved_loop < window.end_point) {
			window.loop_point = resolved_loop;
		}
	}

	window.ignore_note_off = _ignore_note_off && window.loop_point == -1;
	window.boundary_fade_samples = _get_sampler_boundary_fade_samples(_sample_rate, window.end_point - window.start_point);
	return window;
}

int SiOPMWaveSamplerData::get_initial_sample_index(double p_phase) const {
	const PlaybackWindow window = resolve_playback_window();
	return (int)(window.start_point * (1 - p_phase) + window.end_point * p_phase);
}

int SiOPMWaveSamplerData::_seek_head_silence() {
	if (_wave_data.is_empty()) {
		return 0;
	}

	// Note: Original code is pretty much broken here. The intent seems to be to keep track of the
	// last sample points and check if their sum goes over a threshold. However, the order of
	// calls was wrong, and we ended up adding and immediately removing the value from the sum, and
	// constantly overriding our ring buffer without reusing its values.
	// This method has been adjusted to fix the code according to the assumed intent. But it's not
	// tested, and I can't say if the original idea behind the code is wrong somehow.
	const int window_samples = _get_samples_for_duration_ms(0.5, 22);
	const double threshold_scale = (double)window_samples / 22.0;

	SinglyLinkedList<double> *ms_window = memnew(SinglyLinkedList<double>(window_samples, 0.0, true));
	int i = 0;

	if (_channel_count == 1) {
		double ms = 0.0;

		for (; i < _wave_data.size(); i++) {
			ms -= ms_window->get()->value;
			ms_window->get()->value = _wave_data[i] * _wave_data[i];
			ms += ms_window->get()->value;

			ms_window->next();

			if (ms > (0.0011 * threshold_scale)) {
				break;
			}
		}

	} else {
		double ms = 0.0;

		for (; i < _wave_data.size(); i += 2) {
			ms -= ms_window->get()->value;
			ms_window->get()->value = _wave_data[i] * _wave_data[i];
			ms_window->get()->value += _wave_data[i + 1] * _wave_data[i + 1];
			ms += ms_window->get()->value;

			ms_window->next();

			if (ms > (0.0022 * threshold_scale)) {
				break;
			}
		}

		i >>= 1;
	}

	memdelete(ms_window);
	return i - window_samples;
}

int SiOPMWaveSamplerData::_seek_end_gap() {
	if (_wave_data.is_empty()) {
		return 0;
	}

	int i = _wave_data.size() - 1;

	if (_channel_count == 1) {
		for (; i >= 0; i--) {
			double ms = _wave_data[i] * _wave_data[i];

			if (ms > 0.0001) {
				break;
			}
		}
	} else {
		for (; i >= 0; i -= 2) {
			double ms = _wave_data[i] * _wave_data[i];
			ms += _wave_data[i - 1] * _wave_data[i - 1];

			if (ms > 0.0002) {
				break;
			}
		}

		i >>= 1;
	}

	const int tail_margin = _get_samples_for_duration_ms(24.0, 1152);
	return MAX(i, (get_length() - tail_margin));
}

void SiOPMWaveSamplerData::_cache_effective_window_defaults() {
	const int length = get_length();
	if (length <= 0) {
		_auto_start_point = 0;
		_auto_end_point = 0;
		return;
	}

	_auto_start_point = CLAMP(_seek_head_silence(), 0, length - 1);
	_auto_end_point = CLAMP(_seek_end_gap() + 1, 1, length);
	if (_auto_end_point <= _auto_start_point) {
		_auto_end_point = length;
	}
}

int SiOPMWaveSamplerData::_clamp_authored_start_point(int p_start) const {
	if (p_start < 0) {
		return -1;
	}

	const int length = get_length();
	if (length <= 0) {
		return 0;
	}

	return CLAMP(p_start, 0, length - 1);
}

int SiOPMWaveSamplerData::_clamp_authored_end_point(int p_end) const {
	if (p_end < 0) {
		return -1;
	}

	const int length = get_length();
	if (length <= 0) {
		return 0;
	}

	return CLAMP(p_end, 0, length);
}

int SiOPMWaveSamplerData::_clamp_authored_loop_point(int p_loop) const {
	if (p_loop < 0) {
		return -1;
	}

	const int length = get_length();
	if (length <= 0) {
		return -1;
	}

	return CLAMP(p_loop, 0, length - 1);
}

void SiOPMWaveSamplerData::slice(int p_start_point, int p_end_point, int p_loop_point) {
	_start_point = _clamp_authored_start_point(p_start_point);
	_end_point = _clamp_authored_end_point(p_end_point);
	_loop_point = _clamp_authored_loop_point(p_loop_point);
}

//

SiOPMWaveSamplerData::SiOPMWaveSamplerData(const Variant &p_data, bool p_ignore_note_off, int p_pan, int p_src_channel_count, int p_channel_count, bool p_fixed_pitch) :
		SiOPMWaveBase(SiONModuleType::MODULE_SAMPLE) {

	_prepare_wave_data(p_data, p_src_channel_count, p_channel_count);
	set_ignore_note_off(p_ignore_note_off);
	_pan = p_pan;
	_fixed_pitch = p_fixed_pitch;
}

// --- New setters ------------------------------------------------------------

void SiOPMWaveSamplerData::set_pan(int p_pan) {
	_pan = CLAMP(p_pan, -64, 63);
}

void SiOPMWaveSamplerData::set_gain_db(int p_db) {
	int clamped = CLAMP(p_db, kSAMPLER_GAIN_MIN_DB, kSAMPLER_GAIN_MAX_DB);
	_gain_db = clamped;
	_gain_linear = std::pow(2.0, (double)clamped / 6.0);
}

void SiOPMWaveSamplerData::set_start_point(int p_start) {
	_start_point = _clamp_authored_start_point(p_start);
}

void SiOPMWaveSamplerData::set_end_point(int p_end) {
	_end_point = _clamp_authored_end_point(p_end);
}

void SiOPMWaveSamplerData::set_loop_point(int p_loop) {
	_loop_point = _clamp_authored_loop_point(p_loop);
}

void SiOPMWaveSamplerData::set_root_offset(int p_semitones) {
	_root_offset = CLAMP(p_semitones, -48, 48);
}

void SiOPMWaveSamplerData::set_coarse_offset(int p_semitones) {
	_coarse_offset = CLAMP(p_semitones, -24, 24);
}

void SiOPMWaveSamplerData::set_fine_offset(int p_cents) {
	_fine_offset = CLAMP(p_cents, -100, 100);
}

// ---------------------------------------------------------------------------

void SiOPMWaveSamplerData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_pan", "pan"), &SiOPMWaveSamplerData::set_pan);
	ClassDB::bind_method(D_METHOD("get_pan"), &SiOPMWaveSamplerData::get_pan);
	ClassDB::bind_method(D_METHOD("set_gain_db", "db"), &SiOPMWaveSamplerData::set_gain_db);
	ClassDB::bind_method(D_METHOD("get_gain_db"), &SiOPMWaveSamplerData::get_gain_db);
	ClassDB::bind_method(D_METHOD("get_sample_rate"), &SiOPMWaveSamplerData::get_sample_rate);
	ClassDB::bind_method(D_METHOD("set_ignore_note_off", "ignore"), &SiOPMWaveSamplerData::set_ignore_note_off);
	ClassDB::bind_method(D_METHOD("get_ignore_note_off"), &SiOPMWaveSamplerData::get_ignore_note_off);
	ClassDB::bind_method(D_METHOD("set_start_point", "start"), &SiOPMWaveSamplerData::set_start_point);
	ClassDB::bind_method(D_METHOD("get_start_point"), &SiOPMWaveSamplerData::get_start_point);
	ClassDB::bind_method(D_METHOD("set_end_point", "end"), &SiOPMWaveSamplerData::set_end_point);
	ClassDB::bind_method(D_METHOD("get_end_point"), &SiOPMWaveSamplerData::get_end_point);
	ClassDB::bind_method(D_METHOD("set_loop_point", "loop"), &SiOPMWaveSamplerData::set_loop_point);
	ClassDB::bind_method(D_METHOD("get_loop_point"), &SiOPMWaveSamplerData::get_loop_point);
	ClassDB::bind_method(D_METHOD("slice", "start", "end", "loop"), &SiOPMWaveSamplerData::slice, DEFVAL(-1), DEFVAL(-1), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_length"), &SiOPMWaveSamplerData::get_length);
	ClassDB::bind_method(D_METHOD("set_fixed_pitch", "fixed"), &SiOPMWaveSamplerData::set_fixed_pitch);
	ClassDB::bind_method(D_METHOD("is_fixed_pitch"), &SiOPMWaveSamplerData::is_fixed_pitch);
	ClassDB::bind_method(D_METHOD("get_root_offset"), &SiOPMWaveSamplerData::get_root_offset);
	ClassDB::bind_method(D_METHOD("set_root_offset", "semitones"), &SiOPMWaveSamplerData::set_root_offset);
	ClassDB::bind_method(D_METHOD("get_coarse_offset"), &SiOPMWaveSamplerData::get_coarse_offset);
	ClassDB::bind_method(D_METHOD("set_coarse_offset", "semitones"), &SiOPMWaveSamplerData::set_coarse_offset);
	ClassDB::bind_method(D_METHOD("get_fine_offset"), &SiOPMWaveSamplerData::get_fine_offset);
	ClassDB::bind_method(D_METHOD("set_fine_offset", "cents"), &SiOPMWaveSamplerData::set_fine_offset);
	ClassDB::bind_method(D_METHOD("duplicate"), &SiOPMWaveSamplerData::duplicate);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "pan"), "set_pan", "get_pan");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "start_point"), "set_start_point", "get_start_point");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "end_point"), "set_end_point", "get_end_point");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loop_point"), "set_loop_point", "get_loop_point");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fixed_pitch"), "set_fixed_pitch", "is_fixed_pitch");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ignore_note_off"), "set_ignore_note_off", "get_ignore_note_off");
}

Ref<SiOPMWaveSamplerData> SiOPMWaveSamplerData::duplicate() const {
	Ref<SiOPMWaveSamplerData> copy = Ref<SiOPMWaveSamplerData>(memnew(SiOPMWaveSamplerData));

	// Shallow-copy primitive members.
	copy->_wave_data = _wave_data; // Shared buffer (copy-on-write)
	copy->_channel_count = _channel_count;
	copy->_pan = _pan;
	copy->_gain_db = _gain_db;
	copy->_gain_linear = _gain_linear;
	copy->_sample_rate = _sample_rate;
	copy->_ignore_note_off = _ignore_note_off;
	copy->_fixed_pitch = _fixed_pitch;

	copy->_root_offset = _root_offset;
	copy->_coarse_offset = _coarse_offset;
	copy->_fine_offset = _fine_offset;

	copy->_start_point = _start_point;
	copy->_end_point = _end_point;
	copy->_loop_point = _loop_point;
	copy->_auto_start_point = _auto_start_point;
	copy->_auto_end_point = _auto_end_point;

	return copy;
}
