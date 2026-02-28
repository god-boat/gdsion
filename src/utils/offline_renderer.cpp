/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "offline_renderer.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include "sion_driver.h"
#include "chip/siopm_sound_chip.h"
#include "effector/si_effector.h"
#include "sequencer/simml_sequencer.h"

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

bool SiONOfflineRenderer::_cache_driver_internals() {
	ERR_FAIL_NULL_V(_driver, false);

	_sound_chip = _driver->get_sound_chip();
	_effector = _driver->get_effector();
	_sequencer = _driver->get_sequencer();
	_buffer_length = _driver->get_buffer_length();

	ERR_FAIL_NULL_V_MSG(_sound_chip, false, "SiONOfflineRenderer: Driver sound chip is null.");
	ERR_FAIL_NULL_V_MSG(_effector, false, "SiONOfflineRenderer: Driver effector is null.");
	ERR_FAIL_NULL_V_MSG(_sequencer, false, "SiONOfflineRenderer: Driver sequencer is null.");
	ERR_FAIL_COND_V_MSG(_buffer_length <= 0, false, "SiONOfflineRenderer: Driver buffer length is invalid.");

	return true;
}

bool SiONOfflineRenderer::begin(SiONDriver *p_driver) {
	ERR_FAIL_NULL_V_MSG(p_driver, false, "SiONOfflineRenderer: Driver is null.");
	ERR_FAIL_COND_V_MSG(_active, false, "SiONOfflineRenderer: Already active. Call finish() first.");

	_driver = p_driver;

	if (!_cache_driver_internals()) {
		_driver = nullptr;
		return false;
	}

	_total_frames_rendered = 0;
	_active = true;

	return true;
}

PackedFloat32Array SiONOfflineRenderer::render_block() {
	PackedFloat32Array result;

	ERR_FAIL_COND_V_MSG(!_active, result, "SiONOfflineRenderer: Not active. Call begin() first.");
	ERR_FAIL_NULL_V(_driver, result);

	int sample_count = _buffer_length * 2; // stereo interleaved
	result.resize(sample_count);

	// Drain queued track updates (notes, volume, effects, etc.)
	_driver->_drain_track_mailbox();

	// Process one internal block — same pipeline as generate_audio().
	_sound_chip->begin_process();
	_effector->begin_process();
	_sequencer->process();
	_driver->_update_track_effect_post_fader();
	_effector->end_process();
	_sound_chip->end_process();

	// Read output and convert double -> float32.
	Vector<double> *out_buf = _sound_chip->get_output_buffer_ptr();
	float *dst = result.ptrw();
	for (int i = 0; i < sample_count; ++i) {
		dst[i] = (float)(*out_buf)[i];
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
		_driver->_drain_track_mailbox();

		_sound_chip->begin_process();
		_effector->begin_process();
		_sequencer->process();
		_driver->_update_track_effect_post_fader();
		_effector->end_process();
		_sound_chip->end_process();

		Vector<double> *out_buf = _sound_chip->get_output_buffer_ptr();

		for (int i = 0; i < samples_per_block; ++i) {
			dst[offset + i] = (float)(*out_buf)[i];
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
	_sound_chip = nullptr;
	_effector = nullptr;
	_sequencer = nullptr;
	_buffer_length = 0;
	_total_frames_rendered = 0;
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
