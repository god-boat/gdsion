/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "offline_renderer.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include "sion_driver.h"

using namespace godot;

void SiONOfflineRenderer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("begin", "driver"), &SiONOfflineRenderer::begin);
	ClassDB::bind_method(D_METHOD("render_block"), &SiONOfflineRenderer::render_block);
	ClassDB::bind_method(D_METHOD("render_blocks", "block_count"), &SiONOfflineRenderer::render_blocks);
	ClassDB::bind_method(D_METHOD("finish"), &SiONOfflineRenderer::finish);
	ClassDB::bind_method(D_METHOD("is_active"), &SiONOfflineRenderer::is_active);
	ClassDB::bind_method(D_METHOD("get_block_size_frames"), &SiONOfflineRenderer::get_block_size_frames);
	ClassDB::bind_method(D_METHOD("get_block_size_samples"), &SiONOfflineRenderer::get_block_size_samples);
	ClassDB::bind_method(D_METHOD("get_total_frames_rendered"), &SiONOfflineRenderer::get_total_frames_rendered);
	ClassDB::bind_method(D_METHOD("get_total_time_rendered"), &SiONOfflineRenderer::get_total_time_rendered);
	ClassDB::bind_method(D_METHOD("get_sample_rate"), &SiONOfflineRenderer::get_sample_rate);
}

bool SiONOfflineRenderer::begin(SiONDriver *p_driver) {
	ERR_FAIL_NULL_V_MSG(p_driver, false, "SiONOfflineRenderer: Driver is null.");
	ERR_FAIL_COND_V_MSG(_active, false, "SiONOfflineRenderer: Already active. Call finish() first.");

	_driver = p_driver;
	_buffer_length = _driver->get_buffer_length();
	ERR_FAIL_COND_V_MSG(_buffer_length <= 0, false, "SiONOfflineRenderer: Driver buffer length is invalid.");

	// Discard any residual frames left over from a previous runtime audio callback,
	// so the export starts from a clean block boundary.
	_driver->_residual_buffer_frame_count = 0;
	_driver->_residual_frame_offset = 0;

	// Pre-size scratch buffer for one block of stereo audio.
	_scratch.resize(_buffer_length);

	_total_frames_rendered = 0;
	_active = true;

	return true;
}

PackedFloat32Array SiONOfflineRenderer::render_block() {
	PackedFloat32Array result;

	ERR_FAIL_COND_V_MSG(!_active, result, "SiONOfflineRenderer: Not active. Call begin() first.");
	ERR_FAIL_NULL_V(_driver, result);

	// Use generate_audio() — the exact runtime audio path — so offline output is
	// bit-identical to what the audio thread would have produced. generate_audio()
	// drains the track + fx mailboxes internally.
	_driver->generate_audio(_scratch.ptrw(), _buffer_length);

	int sample_count = _buffer_length * 2; // stereo interleaved
	result.resize(sample_count);
	float *dst = result.ptrw();
	const AudioFrame *src = _scratch.ptr();
	for (int i = 0; i < _buffer_length; ++i) {
		dst[i * 2 + 0] = src[i].left;
		dst[i * 2 + 1] = src[i].right;
	}

	_total_frames_rendered += _buffer_length;
	return result;
}

PackedFloat32Array SiONOfflineRenderer::render_blocks(int p_block_count) {
	PackedFloat32Array result;

	ERR_FAIL_COND_V_MSG(p_block_count <= 0, result, "SiONOfflineRenderer: Block count must be positive.");
	ERR_FAIL_COND_V_MSG(!_active, result, "SiONOfflineRenderer: Not active. Call begin() first.");
	ERR_FAIL_NULL_V(_driver, result);

	int samples_per_block = _buffer_length * 2;
	int total_samples = p_block_count * samples_per_block;
	result.resize(total_samples);

	float *dst = result.ptrw();
	int offset = 0;

	for (int b = 0; b < p_block_count; ++b) {
		_driver->generate_audio(_scratch.ptrw(), _buffer_length);

		const AudioFrame *src = _scratch.ptr();
		for (int i = 0; i < _buffer_length; ++i) {
			dst[offset + i * 2 + 0] = src[i].left;
			dst[offset + i * 2 + 1] = src[i].right;
		}

		offset += samples_per_block;
		_total_frames_rendered += _buffer_length;
	}

	return result;
}

void SiONOfflineRenderer::finish() {
	if (!_active) {
		return;
	}

	_active = false;
	_driver = nullptr;
	_buffer_length = 0;
	_total_frames_rendered = 0;
	_scratch.clear();
}

double SiONOfflineRenderer::get_total_time_rendered() const {
	if (!_driver) {
		return 0.0;
	}
	double sr = _driver->get_sample_rate();
	if (sr <= 0.0) {
		return 0.0;
	}
	return (double)_total_frames_rendered / sr;
}

double SiONOfflineRenderer::get_sample_rate() const {
	if (!_driver) {
		return 0.0;
	}
	return _driver->get_sample_rate();
}
