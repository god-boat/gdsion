/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_wave_sampler_data.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>

#include "sion_enums.h"
#include <cmath>
#include "templates/singly_linked_list.h"
#include <godot_cpp/core/class_db.hpp>
#include "utils/transformer_util.h"
#include <utility> // std::move

using namespace godot;

// Target sample rate that GDSiON operates at.
static constexpr int kTARGET_SR = 48000;
static constexpr int kSAMPLER_GAIN_MIN_DB = -36;
static constexpr int kSAMPLER_GAIN_MAX_DB = 36;

// ---------------------------------------------------------------------------
// Helper: simple linear resampler for mono/stereo interleaved PCM in [-1, 1]
// ---------------------------------------------------------------------------
// Simple linear resampler for mono/stereo interleaved PCM in the range [-1, 1].
// The result is written into r_dst to avoid an extra copy when the caller moves
// it back into the original Vector.
static void _resample_linear(const Vector<double> &p_src, int p_channels, int p_src_rate, int p_dst_rate, Vector<double> &r_dst) {
    // Sanity checks & trivial cases.
    if (p_src_rate == p_dst_rate || p_src_rate <= 0 || p_dst_rate <= 0) {
        r_dst = p_src;
        return;
    }

    int src_frame_count = p_src.size() / p_channels;
    // Guard against very short clips (<= 1 frame per channel).
    if (src_frame_count < 2) {
        r_dst = p_src;
        return;
    }

    double ratio = static_cast<double>(p_dst_rate) / static_cast<double>(p_src_rate);
    double inv_ratio = 1.0 / ratio;
    int dst_frame_count = static_cast<int>(std::ceil(src_frame_count * ratio));

    r_dst.resize_zeroed(dst_frame_count * p_channels);

    for (int ch = 0; ch < p_channels; ch++) {
        double src_pos = 0.0;
        for (int i = 0; i < dst_frame_count; ++i, src_pos += inv_ratio) {
            int idx = static_cast<int>(std::floor(src_pos));
            double frac = src_pos - idx;
            if (idx >= src_frame_count - 1) {
                idx  = src_frame_count - 2;
                frac = 1.0;
            }
            double s0 = p_src[(idx * p_channels) + ch];
            double s1 = p_src[((idx + 1) * p_channels) + ch];
            r_dst.write[(i * p_channels) + ch] = s0 + (s1 - s0) * frac;
        }
    }
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

                // --- Sample-rate handling -----------------------------------
                int src_rate = kTARGET_SR;

                // Query mix rate generically – any AudioStream that reports one via `get_mix_rate`.
                if (audio_stream->has_method("get_mix_rate")) {
                    Variant v_rate = audio_stream->call("get_mix_rate");
                    if (v_rate.get_type() == Variant::INT) {
                        src_rate = (int)v_rate;
                    } else if (v_rate.get_type() == Variant::FLOAT) {
                        src_rate = (int)((double)v_rate);
                    }
                }

                _sample_rate = src_rate;

                if (src_rate > 0 && src_rate != kTARGET_SR) {
                    Vector<double> resampled;
                    _resample_linear(raw_data, source_channels, src_rate, kTARGET_SR, resampled);
                    raw_data = std::move(resampled);
                    _sample_rate = kTARGET_SR;
                }
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

	// Store unmodified copy for future fade reapplication.
	_original_wave_data = _wave_data;
}

int SiOPMWaveSamplerData::get_length() const {
	if (_channel_count > 0) {
		return _wave_data.size() >> (_channel_count - 1);
	}

	return 0;
}

void SiOPMWaveSamplerData::set_ignore_note_off(bool p_ignore) {
	_ignore_note_off = (_loop_point == -1) && p_ignore;
}

//

int SiOPMWaveSamplerData::get_initial_sample_index(double p_phase) const {
	return (int)(_start_point * (1 - p_phase) + _end_point * p_phase);
}

int SiOPMWaveSamplerData::_seek_head_silence() {
	if (_wave_data.is_empty()) {
		return 0;
	}

	// Note: Original code is pretty much broken here. The intent seems to be to keep track of the
	// last 22 sample points and check if their sum goes over a threshold. However, the order of
	// calls was wrong, and we ended up adding and immediately removing the value from the sum, and
	// constantly overriding our ring buffer without reusing its values.
	// This method has been adjusted to fix the code according to the assumed intent. But it's not
	// tested, and I can't say if the original idea behind the code is wrong somehow.

	SinglyLinkedList<double> *ms_window = memnew(SinglyLinkedList<double>(22, 0.0, true)); // 0.5ms
	int i = 0;

	if (_channel_count == 1) {
		double ms = 0.0;

		for (; i < _wave_data.size(); i++) {
			ms -= ms_window->get()->value;
			ms_window->get()->value = _wave_data[i] * _wave_data[i];
			ms += ms_window->get()->value;

			ms_window->next();

			if (ms > 0.0011) {
				break;
			}
		}

	} else {
		double ms = 0.0;

		for (; i < _wave_data.size(); i += 2) {
			ms -= ms_window->get()->value;
			ms_window->get()->value  = _wave_data[i]     * _wave_data[i];
			ms_window->get()->value += _wave_data[i + 1] * _wave_data[i + 1];
			ms += ms_window->get()->value;

			ms_window->next();

			if (ms > 0.0022) {
				break;
			}
		}

		i >>= 1;
	}

	memdelete(ms_window);
	return i - 22;
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
			double ms = _wave_data[i]     * _wave_data[i];
			ms       += _wave_data[i - 1] * _wave_data[i - 1];

			if (ms > 0.0002) {
				break;
			}
		}

		i >>= 1;
	}

	// SUS: What is 1152? Should be extracted into a clearly named constant.
	return MAX(i, (get_length() - 1152));
}

void SiOPMWaveSamplerData::_slice() {
	if (_start_point < 0) {
		_start_point = _seek_head_silence();
	}
	if (_loop_point < 0) {
		_loop_point = -1;
	}
	if (_end_point < 0) {
		_end_point = _seek_end_gap();
	}

	if (_end_point < _loop_point) {
		_loop_point = -1;
	}
	if (_end_point < _start_point) {
		_end_point = get_length() - 1;
	}

	// Finally, apply a tiny fade-in/out at the new boundaries to reduce clicks.
	_apply_fade();
}

void SiOPMWaveSamplerData::slice(int p_start_point, int p_end_point, int p_loop_point) {
	_start_point = p_start_point;
	_end_point = p_end_point;
	_loop_point = p_loop_point;

	_slice();
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
	_start_point = p_start;
	_slice();
}

void SiOPMWaveSamplerData::set_end_point(int p_end) {
	_end_point = p_end;
	_slice();
}

void SiOPMWaveSamplerData::set_loop_point(int p_loop) {
	_loop_point = p_loop;
	_slice();
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

// ---------------------------------------------------------------------------

// NOTE: This operation is intentionally simple – it linearly ramps the first and
// last FADE_SAMPLES samples of the active region to and from 0. The fade length
// is kept short (about 128 samples ≈ 3 ms @ 44.1 kHz) so that it is inaudible
// but still prevents hard discontinuities.

void SiOPMWaveSamplerData::_apply_fade() {
	const int FADE_SAMPLES = 128; // Must be even smaller than typical slice.

	if (_wave_data.is_empty() || _channel_count == 0) {
		return;
	}

	int fade_len = MIN(FADE_SAMPLES, _end_point - _start_point);
	if (fade_len <= 0) {
		return;
	}

	// Helper lambda to compute index for given mono sample index and channel.
	auto _get_sample_index = [this](int p_sample_idx, int p_channel) {
		return p_sample_idx * _channel_count + p_channel;
	};

	// 1. Restore the samples that were faded previously so we can re-apply with new
	//    boundaries. This touches at most FADE_SAMPLES * channel_count * 2 values
	//    and avoids copying the whole buffer.
	if (_prev_fade_len > 0) {
		for (int i = 0; i < _prev_fade_len; i++) {
			for (int ch = 0; ch < _channel_count; ch++) {
				int idx_start = _get_sample_index(_prev_fade_start + i, ch);
				int idx_end   = _get_sample_index(_prev_fade_end - _prev_fade_len + 1 + i, ch);

				if (idx_start >= 0 && idx_start < _wave_data.size()) {
					_wave_data.write[idx_start] = _original_wave_data[idx_start];
				}
				if (idx_end >= 0 && idx_end < _wave_data.size()) {
					_wave_data.write[idx_end] = _original_wave_data[idx_end];
				}
			}
		}
	}

	// Fade-in.
	for (int i = 0; i < fade_len; i++) {
		double factor = (double)i / (double)fade_len;
		for (int ch = 0; ch < _channel_count; ch++) {
			int idx = _get_sample_index(_start_point + i, ch);
			if (idx >= 0 && idx < _wave_data.size()) {
				_wave_data.write[idx] *= factor;
			}
		}
	}

	// Fade-out.
	for (int i = 0; i < fade_len; i++) {
		double factor = 1.0 - ((double)i / (double)fade_len);
		for (int ch = 0; ch < _channel_count; ch++) {
			int idx = _get_sample_index(_end_point - fade_len + i, ch);
			if (idx >= 0 && idx < _wave_data.size()) {
				_wave_data.write[idx] *= factor;
			}
		}
	}

	// Store current fade region for next invocation.
	_prev_fade_start = _start_point;
	_prev_fade_end   = _end_point;
	_prev_fade_len   = fade_len;
}

Ref<SiOPMWaveSamplerData> SiOPMWaveSamplerData::duplicate() const {
    Ref<SiOPMWaveSamplerData> copy = Ref<SiOPMWaveSamplerData>(memnew(SiOPMWaveSamplerData));

    // Shallow-copy primitive members.
    copy->_wave_data           = _wave_data;          // Shared buffer (copy-on-write)
    copy->_original_wave_data  = _original_wave_data; // Shared buffer as well
    copy->_channel_count       = _channel_count;
    copy->_pan                 = _pan;
    copy->_gain_db             = _gain_db;
    copy->_gain_linear         = _gain_linear;
    copy->_sample_rate         = _sample_rate;
    copy->_ignore_note_off     = _ignore_note_off;
    copy->_fixed_pitch         = _fixed_pitch;
    
    copy->_root_offset         = _root_offset;
    copy->_coarse_offset       = _coarse_offset;
    copy->_fine_offset         = _fine_offset;

    copy->_start_point         = _start_point;
    copy->_end_point           = _end_point;
    copy->_loop_point          = _loop_point;

    // Fade bookkeeping values; the copy will re-apply fades when its slice
    // window changes, so we reset these.
    copy->_prev_fade_start = -1;
    copy->_prev_fade_end   = -1;
    copy->_prev_fade_len   = 0;

    return copy;
}
