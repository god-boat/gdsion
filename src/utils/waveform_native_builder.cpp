/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "waveform_native_builder.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <vector>

using namespace godot;

namespace {

static constexpr uint32_t WAV_RIFF = 0x46464952;
static constexpr uint32_t WAV_WAVE = 0x45564157;
static constexpr uint32_t WAV_FMT = 0x20746D66;
static constexpr uint32_t WAV_DATA = 0x61746164;
static constexpr int WAV_FORMAT_PCM = 1;
static constexpr int WAV_FORMAT_FLOAT = 3;

struct WavInfo {
	int audio_format = 0;
	int channel_count = 0;
	int sample_rate = 0;
	int bits_per_sample = 0;
	int bytes_per_frame = 0;
	int64_t data_offset = 0;
	int64_t data_size = 0;
	int frame_count = 0;
};

struct PeakStageData {
	int frames_per_bin = 1;
	int bin_count = 0;
	PackedFloat32Array mins;
	PackedFloat32Array maxs;
};

Dictionary _make_empty_result() {
	Dictionary result;
	result["loaded"] = false;
	result["load_failed"] = false;
	result["sample_rate"] = 0;
	result["channel_count"] = 0;
	result["frame_count"] = 0;
	result["duration_sec"] = 0.0;
	result["polyline_frame_stride"] = 1;
	result["polyline_samples"] = PackedFloat32Array();
	result["stages"] = Array();
	return result;
}

float _decode_wave_sample(const uint8_t *p_raw, int p_byte_index, const WavInfo &p_info) {
	if (p_info.audio_format == WAV_FORMAT_PCM) {
		switch (p_info.bits_per_sample) {
			case 8:
				return float(int(p_raw[p_byte_index]) - 128) / 128.0f;
			case 16: {
				int16_t sample = int16_t(uint16_t(p_raw[p_byte_index]) | (uint16_t(p_raw[p_byte_index + 1]) << 8));
				return float(sample) / 32768.0f;
			}
			case 24: {
				int32_t sample = int32_t(p_raw[p_byte_index])
					| (int32_t(p_raw[p_byte_index + 1]) << 8)
					| (int32_t(int8_t(p_raw[p_byte_index + 2])) << 16);
				return float(sample) / 8388608.0f;
			}
			case 32: {
				int32_t sample = int32_t(uint32_t(p_raw[p_byte_index])
					| (uint32_t(p_raw[p_byte_index + 1]) << 8)
					| (uint32_t(p_raw[p_byte_index + 2]) << 16)
					| (uint32_t(p_raw[p_byte_index + 3]) << 24));
				return float(sample) / 2147483648.0f;
			}
		}
	} else if (p_info.audio_format == WAV_FORMAT_FLOAT && p_info.bits_per_sample == 32) {
		float sample = 0.0f;
		memcpy(&sample, p_raw + p_byte_index, sizeof(float));
		return sample;
	}
	return 0.0f;
}

bool _parse_wav_header(const Ref<FileAccess> &p_file, WavInfo &r_info) {
	if (p_file.is_null()) {
		return false;
	}

	p_file->seek(0);
	uint32_t riff_id = p_file->get_32();
	p_file->get_32();
	uint32_t wave_id = p_file->get_32();
	if (riff_id != WAV_RIFF || wave_id != WAV_WAVE) {
		return false;
	}

	bool found_fmt = false;
	bool found_data = false;
	int audio_format = 0;

	while (p_file->get_position() < p_file->get_length() - 8) {
		uint32_t chunk_id = p_file->get_32();
		uint32_t chunk_size = p_file->get_32();
		int64_t chunk_start = p_file->get_position();

		if (chunk_id == WAV_FMT) {
			if (chunk_size < 16) {
				return false;
			}

			audio_format = p_file->get_16();
			r_info.audio_format = audio_format;
			r_info.channel_count = p_file->get_16();
			r_info.sample_rate = p_file->get_32();
			p_file->get_32();
			p_file->get_16();
			r_info.bits_per_sample = p_file->get_16();

			if (audio_format != WAV_FORMAT_PCM && audio_format != WAV_FORMAT_FLOAT) {
				return false;
			}
			if (r_info.channel_count < 1 || r_info.channel_count > 2) {
				return false;
			}
			if (audio_format == WAV_FORMAT_PCM) {
				if (r_info.bits_per_sample != 8 && r_info.bits_per_sample != 16 && r_info.bits_per_sample != 24 && r_info.bits_per_sample != 32) {
					return false;
				}
			} else if (r_info.bits_per_sample != 32) {
				return false;
			}

			r_info.bytes_per_frame = r_info.channel_count * (r_info.bits_per_sample / 8);
			found_fmt = true;
		} else if (chunk_id == WAV_DATA) {
			r_info.data_offset = p_file->get_position();
			r_info.data_size = chunk_size;
			found_data = true;
		}

		int64_t next_pos = chunk_start + chunk_size;
		if (chunk_size & 1) {
			next_pos++;
		}
		p_file->seek(next_pos);

		if (found_fmt && found_data) {
			break;
		}
	}

	if (!found_fmt || !found_data || r_info.bytes_per_frame <= 0) {
		return false;
	}

	r_info.frame_count = int(r_info.data_size / r_info.bytes_per_frame);
	return r_info.frame_count > 0;
}

std::vector<PeakStageData> _create_stages(int p_frame_count, int p_channel_count, int p_stage0_frames_per_bin, int p_peak_stage_scale) {
	std::vector<PeakStageData> stages;
	if (p_frame_count <= 0) {
		return stages;
	}

	int frames_per_bin = std::max(1, p_stage0_frames_per_bin);
	while (true) {
		PeakStageData stage;
		stage.frames_per_bin = frames_per_bin;
		stage.bin_count = int((p_frame_count + frames_per_bin - 1) / frames_per_bin);
		int value_count = stage.bin_count * std::max(1, p_channel_count);
		stage.mins.resize(value_count);
		stage.maxs.resize(value_count);
		stages.push_back(stage);

		if (frames_per_bin >= p_frame_count) {
			break;
		}

		int64_t next_frames_per_bin = int64_t(frames_per_bin) * int64_t(p_peak_stage_scale);
		if (next_frames_per_bin <= frames_per_bin) {
			break;
		}
		frames_per_bin = int(next_frames_per_bin);
	}

	return stages;
}

void _merge_stages(std::vector<PeakStageData> &r_stages, int p_channel_count, int p_peak_stage_scale) {
	if (r_stages.size() < 2) {
		return;
	}
	int safe_channel_count = std::max(1, p_channel_count);
	for (size_t stage_index = 1; stage_index < r_stages.size(); ++stage_index) {
		PeakStageData &previous_stage = r_stages[stage_index - 1];
		PeakStageData &current_stage = r_stages[stage_index];
		const float *previous_mins = previous_stage.mins.ptr();
		const float *previous_maxs = previous_stage.maxs.ptr();
		float *current_mins = current_stage.mins.ptrw();
		float *current_maxs = current_stage.maxs.ptrw();

		for (int bin_index = 0; bin_index < current_stage.bin_count; ++bin_index) {
			int previous_start = bin_index * p_peak_stage_scale;
			int previous_end = std::min(previous_start + p_peak_stage_scale, previous_stage.bin_count);
			for (int channel_index = 0; channel_index < safe_channel_count; ++channel_index) {
				int first_value_index = previous_start * safe_channel_count + channel_index;
				float min_value = previous_mins[first_value_index];
				float max_value = previous_maxs[first_value_index];
				for (int previous_bin_index = previous_start + 1; previous_bin_index < previous_end; ++previous_bin_index) {
					int previous_value_index = previous_bin_index * safe_channel_count + channel_index;
					min_value = std::min(min_value, previous_mins[previous_value_index]);
					max_value = std::max(max_value, previous_maxs[previous_value_index]);
				}
				int current_value_index = bin_index * safe_channel_count + channel_index;
				current_mins[current_value_index] = min_value;
				current_maxs[current_value_index] = max_value;
			}
		}
	}
}

Array _serialize_stages(const std::vector<PeakStageData> &p_stages) {
	Array stages;
	for (const PeakStageData &stage : p_stages) {
		Dictionary stage_dict;
		stage_dict["frames_per_bin"] = stage.frames_per_bin;
		stage_dict["bin_count"] = stage.bin_count;
		stage_dict["mins"] = stage.mins;
		stage_dict["maxs"] = stage.maxs;
		stages.push_back(stage_dict);
	}
	return stages;
}

} // namespace

void WaveformNativeBuilder::_bind_methods() {
	ClassDB::bind_method(
		D_METHOD("build_from_wav_path", "file_path", "stage0_frames_per_bin", "max_polyline_cache_samples", "peak_stage_shift_padding"),
		&WaveformNativeBuilder::build_from_wav_path,
		DEFVAL(64),
		DEFVAL(262144),
		DEFVAL(3)
	);
}

Dictionary WaveformNativeBuilder::build_from_wav_path(
	const String &p_file_path,
	int p_stage0_frames_per_bin,
	int p_max_polyline_cache_samples,
	int p_peak_stage_shift_padding
) {
	Dictionary result = _make_empty_result();
	if (p_file_path.is_empty()) {
		result["load_failed"] = true;
		return result;
	}

	Ref<FileAccess> file = FileAccess::open(p_file_path, FileAccess::READ);
	if (file.is_null()) {
		result["load_failed"] = true;
		return result;
	}

	WavInfo info;
	if (!_parse_wav_header(file, info)) {
		result["load_failed"] = true;
		return result;
	}

	file->seek(info.data_offset);
	PackedByteArray raw_data = file->get_buffer(info.data_size);
	int actual_frame_count = int(raw_data.size() / info.bytes_per_frame);
	if (actual_frame_count <= 0) {
		result["load_failed"] = true;
		return result;
	}

	int safe_stage0_frames_per_bin = std::max(1, p_stage0_frames_per_bin);
	int safe_max_polyline_cache_samples = std::max(1, p_max_polyline_cache_samples);
	int safe_shift_padding = std::max(1, p_peak_stage_shift_padding);
	int peak_stage_scale = 1 << std::min(safe_shift_padding, 8);
	int polyline_frame_stride = std::max(1, int((actual_frame_count + safe_max_polyline_cache_samples - 1) / safe_max_polyline_cache_samples));
	int polyline_sample_count = int((actual_frame_count + polyline_frame_stride - 1) / polyline_frame_stride);

	PackedFloat32Array polyline_samples;
	polyline_samples.resize(polyline_sample_count);
	float *polyline_write = polyline_samples.ptrw();

	std::vector<PeakStageData> stages = _create_stages(actual_frame_count, info.channel_count, safe_stage0_frames_per_bin, peak_stage_scale);
	if (stages.empty()) {
		result["load_failed"] = true;
		return result;
	}

	PeakStageData &stage0 = stages[0];
	float *stage0_mins = stage0.mins.ptrw();
	float *stage0_maxs = stage0.maxs.ptrw();

	const uint8_t *raw = raw_data.ptr();
	int last_frame_index = actual_frame_count - 1;
	int base_frames_seen = 0;
	float base_min_left = INFINITY;
	float base_max_left = -INFINITY;
	float base_min_right = INFINITY;
	float base_max_right = -INFINITY;
	int stage0_bin_index = 0;
	int polyline_index = 0;

	for (int frame_index = 0; frame_index < actual_frame_count; ++frame_index) {
		int byte_index = frame_index * info.bytes_per_frame;
		float left = _decode_wave_sample(raw, byte_index, info);
		float right = left;
		if (info.channel_count > 1) {
			right = _decode_wave_sample(raw, byte_index + (info.bits_per_sample / 8), info);
		}

		if ((frame_index % polyline_frame_stride) == 0 && polyline_index < polyline_sample_count) {
			polyline_write[polyline_index++] = (info.channel_count == 1) ? left : ((left + right) * 0.5f);
		}

		base_min_left = std::min(base_min_left, left);
		base_max_left = std::max(base_max_left, left);
		base_min_right = std::min(base_min_right, right);
		base_max_right = std::max(base_max_right, right);
		base_frames_seen++;

		if (base_frames_seen < safe_stage0_frames_per_bin && frame_index != last_frame_index) {
			continue;
		}

		int value_index = stage0_bin_index * std::max(1, info.channel_count);
		stage0_mins[value_index] = base_min_left;
		stage0_maxs[value_index] = base_max_left;
		if (info.channel_count > 1) {
			stage0_mins[value_index + 1] = base_min_right;
			stage0_maxs[value_index + 1] = base_max_right;
		}
		stage0_bin_index++;
		base_frames_seen = 0;
		base_min_left = INFINITY;
		base_max_left = -INFINITY;
		base_min_right = INFINITY;
		base_max_right = -INFINITY;
	}

	_merge_stages(stages, info.channel_count, peak_stage_scale);

	result["loaded"] = true;
	result["load_failed"] = false;
	result["sample_rate"] = info.sample_rate;
	result["channel_count"] = info.channel_count;
	result["frame_count"] = actual_frame_count;
	result["duration_sec"] = double(actual_frame_count) / double(info.sample_rate);
	result["polyline_frame_stride"] = polyline_frame_stride;
	result["polyline_samples"] = polyline_samples;
	result["stages"] = _serialize_stages(stages);
	return result;
}