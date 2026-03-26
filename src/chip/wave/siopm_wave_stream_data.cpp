/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_wave_stream_data.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <cmath>
#include <cstring>
#include <chrono>

using namespace godot;

// WAV chunk IDs (little-endian uint32).
static constexpr uint32_t WAV_RIFF = 0x46464952; // "RIFF"
static constexpr uint32_t WAV_WAVE = 0x45564157; // "WAVE"
static constexpr uint32_t WAV_FMT  = 0x20746D66; // "fmt "
static constexpr uint32_t WAV_DATA = 0x61746164; // "data"

// WAV format codes.
static constexpr int WAV_FORMAT_PCM   = 1;
static constexpr int WAV_FORMAT_FLOAT = 3;

// ---------------------------------------------------------------------------
// Static loader thread state
// ---------------------------------------------------------------------------

std::thread SiOPMWaveStreamData::_s_loader_thread;
std::atomic<bool> SiOPMWaveStreamData::_s_loader_running{false};
std::atomic<SiOPMWaveStreamData *> SiOPMWaveStreamData::_s_queue_head{nullptr};

// ---------------------------------------------------------------------------
// Utility: next power of 2
// ---------------------------------------------------------------------------

static int _next_power_of_2(int p_value) {
	if (p_value <= 0) {
		return 1;
	}
	p_value--;
	p_value |= p_value >> 1;
	p_value |= p_value >> 2;
	p_value |= p_value >> 4;
	p_value |= p_value >> 8;
	p_value |= p_value >> 16;
	return p_value + 1;
}

// ---------------------------------------------------------------------------
// GDScript bindings
// ---------------------------------------------------------------------------

void SiOPMWaveStreamData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_wav", "file_path", "ring_capacity"), &SiOPMWaveStreamData::load_wav, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("is_valid"), &SiOPMWaveStreamData::is_valid);
	ClassDB::bind_method(D_METHOD("get_channel_count"), &SiOPMWaveStreamData::get_channel_count);
	ClassDB::bind_method(D_METHOD("get_source_sample_rate"), &SiOPMWaveStreamData::get_source_sample_rate);
	ClassDB::bind_method(D_METHOD("get_total_frames"), &SiOPMWaveStreamData::get_total_frames);
	ClassDB::bind_method(D_METHOD("set_in_sample", "sample"), &SiOPMWaveStreamData::set_in_sample);
	ClassDB::bind_method(D_METHOD("get_in_sample"), &SiOPMWaveStreamData::get_in_sample);
	ClassDB::bind_method(D_METHOD("set_out_sample", "sample"), &SiOPMWaveStreamData::set_out_sample);
	ClassDB::bind_method(D_METHOD("get_out_sample"), &SiOPMWaveStreamData::get_out_sample);
	ClassDB::bind_method(D_METHOD("set_looping", "looping"), &SiOPMWaveStreamData::set_looping);
	ClassDB::bind_method(D_METHOD("get_looping"), &SiOPMWaveStreamData::get_looping);
	ClassDB::bind_method(D_METHOD("set_loop_region", "start_sample", "end_sample"), &SiOPMWaveStreamData::set_loop_region);
	ClassDB::bind_method(D_METHOD("get_loop_start_sample"), &SiOPMWaveStreamData::get_loop_start_sample);
	ClassDB::bind_method(D_METHOD("get_loop_end_sample"), &SiOPMWaveStreamData::get_loop_end_sample);
	ClassDB::bind_method(D_METHOD("activate"), &SiOPMWaveStreamData::activate);
	ClassDB::bind_method(D_METHOD("deactivate"), &SiOPMWaveStreamData::deactivate);
	ClassDB::bind_method(D_METHOD("seek", "position_sample"), &SiOPMWaveStreamData::seek);
	ClassDB::bind_method(D_METHOD("prefill_sync"), &SiOPMWaveStreamData::prefill_sync);
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SiOPMWaveStreamData::SiOPMWaveStreamData(const String &p_file_path, int p_ring_capacity) :
		SiOPMWaveBase(SiONModuleType::MODULE_STREAM) {
	if (!p_file_path.is_empty()) {
		load_wav(p_file_path, p_ring_capacity);
	}
}

SiOPMWaveStreamData::~SiOPMWaveStreamData() {
	_active.store(false, std::memory_order_release);
	_s_wait_until_idle(this);
	_loader_file = Ref<FileAccess>();
}

// ---------------------------------------------------------------------------
// load_wav
// ---------------------------------------------------------------------------

bool SiOPMWaveStreamData::load_wav(const String &p_file_path, int p_ring_capacity) {
	// Clean up any existing state.
	deactivate();
	_s_wait_until_idle(this);
	_loader_file = Ref<FileAccess>();
	_valid = false;
	_file_path = p_file_path;

	if (!_parse_wav_header()) {
		ERR_PRINT("SiOPMWaveStreamData: Failed to parse WAV header: " + p_file_path);
		return false;
	}

	// Allocate ring buffer (power of 2).
	int capacity = (p_ring_capacity > 0) ? p_ring_capacity : DEFAULT_RING_CAPACITY;
	_ring_capacity = _next_power_of_2(capacity);
	_ring_mask = _ring_capacity - 1;
	_ring_data.resize(_ring_capacity * _channel_count, 0.0);
	_ring_read_pos.store(0, std::memory_order_relaxed);
	_ring_write_pos.store(0, std::memory_order_relaxed);

	// Reset loader state.
	_file_read_pos_frames = 0;
	_decode_pos_frames = 0;
	_decode_buf_valid = 0;
	_loader_file = Ref<FileAccess>();

	_valid = true;

	// Synchronous prefill so data is immediately available for playback.
	prefill_sync();

	return true;
}

// ---------------------------------------------------------------------------
// WAV header parsing
// ---------------------------------------------------------------------------

bool SiOPMWaveStreamData::_parse_wav_header() {
	Ref<FileAccess> file = FileAccess::open(_file_path, FileAccess::READ);
	if (file.is_null()) {
		return false;
	}

	// RIFF header.
	uint32_t riff_id = file->get_32();
	file->get_32(); // file size
	uint32_t wave_id = file->get_32();

	if (riff_id != WAV_RIFF || wave_id != WAV_WAVE) {
		ERR_PRINT("SiOPMWaveStreamData: Not a valid WAV file.");
		return false;
	}

	bool found_fmt = false;
	bool found_data = false;

	while (file->get_position() < file->get_length() - 8) {
		uint32_t chunk_id = file->get_32();
		uint32_t chunk_size = file->get_32();
		int64_t chunk_start = file->get_position();

		if (chunk_id == WAV_FMT) {
			if (chunk_size < 16) {
				ERR_PRINT("SiOPMWaveStreamData: fmt chunk too small.");
				return false;
			}

			_audio_format = file->get_16();
			_channel_count = file->get_16();
			_source_sample_rate = file->get_32();
			file->get_32(); // byte_rate
			file->get_16(); // block_align
			_bits_per_sample = file->get_16();

			if (_audio_format != WAV_FORMAT_PCM && _audio_format != WAV_FORMAT_FLOAT) {
				ERR_PRINT("SiOPMWaveStreamData: Unsupported WAV format code: " + itos(_audio_format));
				return false;
			}
			if (_channel_count < 1 || _channel_count > 2) {
				ERR_PRINT("SiOPMWaveStreamData: Unsupported channel count: " + itos(_channel_count));
				return false;
			}
			if (_bits_per_sample != 16 && _bits_per_sample != 24 && _bits_per_sample != 32) {
				ERR_PRINT("SiOPMWaveStreamData: Unsupported bits per sample: " + itos(_bits_per_sample));
				return false;
			}
			if (_audio_format == WAV_FORMAT_FLOAT && _bits_per_sample != 32) {
				ERR_PRINT("SiOPMWaveStreamData: Float format requires 32-bit samples.");
				return false;
			}

			_bytes_per_frame = _channel_count * (_bits_per_sample / 8);
			found_fmt = true;

		} else if (chunk_id == WAV_DATA) {
			_data_offset = file->get_position();
			_data_size = chunk_size;
			_total_source_frames = (int)(_data_size / _bytes_per_frame);
			found_data = true;
		}

		// Advance to next chunk (chunks are word-aligned).
		int64_t next_pos = chunk_start + chunk_size;
		if (chunk_size & 1) {
			next_pos++; // Padding byte.
		}
		file->seek(next_pos);

		if (found_fmt && found_data) {
			break;
		}
	}

	if (!found_fmt || !found_data) {
		ERR_PRINT("SiOPMWaveStreamData: Missing fmt or data chunk.");
		return false;
	}

	if (_total_source_frames <= 0) {
		ERR_PRINT("SiOPMWaveStreamData: Empty audio data.");
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Trim setters
// ---------------------------------------------------------------------------

void SiOPMWaveStreamData::set_in_sample(int64_t p_sample) {
	_in_sample.store(MAX(p_sample, (int64_t)0), std::memory_order_relaxed);
}

void SiOPMWaveStreamData::set_out_sample(int64_t p_sample) {
	_out_sample.store(MAX(p_sample, (int64_t)0), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Loop region setters
// ---------------------------------------------------------------------------

void SiOPMWaveStreamData::set_looping(bool p_looping) {
	_looping.store(p_looping, std::memory_order_relaxed);
}

void SiOPMWaveStreamData::set_loop_region(int64_t p_start_sample, int64_t p_end_sample) {
	_loop_start_sample.store(MAX(p_start_sample, (int64_t)0), std::memory_order_relaxed);
	_loop_end_sample.store(MAX(p_end_sample, (int64_t)0), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Audio-thread ring buffer API
// ---------------------------------------------------------------------------

int SiOPMWaveStreamData::ring_available() const {
	uint32_t wp = _ring_write_pos.load(std::memory_order_acquire);
	uint32_t rp = _ring_read_pos.load(std::memory_order_relaxed);
	return (int)(wp - rp);
}

double SiOPMWaveStreamData::ring_read_sample(int p_offset, int p_channel) const {
	uint32_t rp = _ring_read_pos.load(std::memory_order_relaxed);
	int idx = ((rp + p_offset) & _ring_mask) * _channel_count + p_channel;
	return _ring_data[idx];
}

void SiOPMWaveStreamData::ring_advance_read(int p_frames) {
	uint32_t rp = _ring_read_pos.load(std::memory_order_relaxed);
	_ring_read_pos.store(rp + p_frames, std::memory_order_release);
}

void SiOPMWaveStreamData::request_refill() {
	_s_enqueue(this);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SiOPMWaveStreamData::activate() {
	if (!_valid || _active.load(std::memory_order_relaxed)) {
		return;
	}
	_active.store(true, std::memory_order_release);
	_s_ensure_loader_running();
	_s_enqueue(this);
}

void SiOPMWaveStreamData::deactivate() {
	if (!_active.load(std::memory_order_relaxed)) {
		return;
	}
	_active.store(false, std::memory_order_release);
	// The loader thread will see _active == false and skip any pending or
	// future refill for this instance. _loader_file is cleaned up later by
	// the destructor or load_wav() after _s_wait_until_idle().
}

void SiOPMWaveStreamData::seek(int64_t p_position_sample) {
	_seek_target.store(p_position_sample, std::memory_order_relaxed);
	_seek_requested.store(true, std::memory_order_release);

	// Flush the ring buffer so the channel sees no stale data.
	uint32_t wp = _ring_write_pos.load(std::memory_order_relaxed);
	_ring_read_pos.store(wp, std::memory_order_release);

	_s_enqueue(this);
}

void SiOPMWaveStreamData::prefill_sync() {
	if (!_valid) {
		return;
	}

	// Reset loader state for a clean fill from the current position.
	int64_t in_sample = _in_sample.load(std::memory_order_relaxed);
	_reset_decode_to_sample(in_sample);

	_ring_read_pos.store(0, std::memory_order_relaxed);
	_ring_write_pos.store(0, std::memory_order_relaxed);

	// Open the file for reading.
	_loader_file = FileAccess::open(_file_path, FileAccess::READ);
	if (_loader_file.is_null()) {
		ERR_PRINT("SiOPMWaveStreamData: Failed to open file for prefill: " + _file_path);
		return;
	}

	// Fill the entire ring buffer synchronously.
	_fill_ring_buffer_impl();
}

// ---------------------------------------------------------------------------
// Decode: raw WAV bytes → interleaved doubles
// ---------------------------------------------------------------------------

void SiOPMWaveStreamData::_decode_raw_to_doubles(const PackedByteArray &p_raw, int p_frame_count) {
	int total_samples = p_frame_count * _channel_count;
	_decode_buffer.resize(total_samples);

	const uint8_t *raw = p_raw.ptr();

	if (_audio_format == WAV_FORMAT_PCM && _bits_per_sample == 16) {
		for (int i = 0; i < total_samples; i++) {
			int16_t sample = (int16_t)((uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
			_decode_buffer.write[i] = (double)sample / 32768.0;
		}
	} else if (_audio_format == WAV_FORMAT_PCM && _bits_per_sample == 24) {
		for (int i = 0; i < total_samples; i++) {
			int32_t sample = (int32_t)raw[i * 3]
				| ((int32_t)raw[i * 3 + 1] << 8)
				| ((int32_t)(int8_t)raw[i * 3 + 2] << 16);
			_decode_buffer.write[i] = (double)sample / 8388608.0;
		}
	} else if (_audio_format == WAV_FORMAT_FLOAT && _bits_per_sample == 32) {
		for (int i = 0; i < total_samples; i++) {
			float f;
			memcpy(&f, &raw[i * 4], sizeof(float));
			_decode_buffer.write[i] = (double)f;
		}
	}

	_decode_buf_valid = p_frame_count;
}

// ---------------------------------------------------------------------------
// Loader: read chunk from WAV file
// ---------------------------------------------------------------------------

bool SiOPMWaveStreamData::_refill_decode_buffer() {
	if (_loader_file.is_null()) {
		return false;
	}

	if (_file_read_pos_frames >= _total_source_frames) {
		_decode_buf_valid = 0;
		return false;
	}

	int frames_to_read = DECODE_CHUNK_FRAMES;
	if (_file_read_pos_frames + frames_to_read > _total_source_frames) {
		frames_to_read = _total_source_frames - (int)_file_read_pos_frames;
	}
	if (frames_to_read <= 0) {
		_decode_buf_valid = 0;
		return false;
	}

	int64_t byte_offset = _data_offset + _file_read_pos_frames * _bytes_per_frame;
	_loader_file->seek(byte_offset);
	PackedByteArray raw = _loader_file->get_buffer(frames_to_read * _bytes_per_frame);

	int actual_frames = raw.size() / _bytes_per_frame;
	if (actual_frames <= 0) {
		_decode_buf_valid = 0;
		return false;
	}

	_decode_raw_to_doubles(raw, actual_frames);
	_file_read_pos_frames += actual_frames;
	return true;
}

// ---------------------------------------------------------------------------
// Loader: produce source frames
// ---------------------------------------------------------------------------

int SiOPMWaveStreamData::_produce_frames(double *r_out, int p_max_frames) {
	int64_t effective_end = _out_sample.load(std::memory_order_relaxed);
	if (effective_end <= 0 || effective_end > _total_source_frames) {
		effective_end = _total_source_frames;
	}

	// Loop region: when looping is enabled, the loader wraps back to
	// loop_start when it reaches loop_end, producing continuous audio.
	bool looping = _looping.load(std::memory_order_relaxed);
	int64_t loop_start_raw = _loop_start_sample.load(std::memory_order_relaxed);
	int64_t loop_end_raw = _loop_end_sample.load(std::memory_order_relaxed);
	int64_t in_samp = _in_sample.load(std::memory_order_relaxed);
	// Resolve defaults: 0 means "use trim region".
	int64_t effective_loop_start = (looping && loop_start_raw > 0) ? loop_start_raw : in_samp;
	int64_t effective_loop_end = (looping && loop_end_raw > 0) ? loop_end_raw : effective_end;
	effective_loop_start = CLAMP(effective_loop_start, (int64_t)0, effective_end);
	effective_loop_end = CLAMP(effective_loop_end, effective_loop_start, effective_end);

	int frames_produced = 0;

	while (frames_produced < p_max_frames) {
		// Check for loop wrap or end-of-stream.
		if (_decode_pos_frames >= effective_loop_end) {
			if (looping && effective_loop_end > effective_loop_start) {
				_reset_decode_to_sample(effective_loop_start);
				continue;
			} else {
				break;
			}
		}

		// Ensure decode buffer has data.
		if (_decode_buf_valid <= 0) {
			if (!_refill_decode_buffer()) {
				if (looping && effective_loop_end > effective_loop_start) {
					// Hit EOF before loop_end — wrap back to loop start.
					_reset_decode_to_sample(effective_loop_start);
					continue;
				}
				break;
			}
		}

		int end_remaining = (int)(effective_loop_end - _decode_pos_frames);
		int to_copy = p_max_frames - frames_produced;
		if (to_copy > _decode_buf_valid) {
			to_copy = _decode_buf_valid;
		}
		if (to_copy > end_remaining) {
			to_copy = end_remaining;
		}
		if (to_copy <= 0) {
			break;
		}

		// Copy from the front of the decode buffer.
		int buf_offset = (_decode_buffer.size() / _channel_count) - _decode_buf_valid;
		for (int i = 0; i < to_copy * _channel_count; i++) {
			r_out[frames_produced * _channel_count + i] =
				_decode_buffer[buf_offset * _channel_count + i];
		}

		frames_produced += to_copy;
		_decode_pos_frames += to_copy;
		_decode_buf_valid -= to_copy;
	}

	return frames_produced;
}

// ---------------------------------------------------------------------------
// Loader: write frames to ring buffer
// ---------------------------------------------------------------------------

void SiOPMWaveStreamData::_ring_write_frames(const double *p_data, int p_frame_count) {
	uint32_t wp = _ring_write_pos.load(std::memory_order_relaxed);

	for (int i = 0; i < p_frame_count; i++) {
		int idx = ((wp + i) & _ring_mask) * _channel_count;
		for (int ch = 0; ch < _channel_count; ch++) {
			_ring_data[idx + ch] = p_data[i * _channel_count + ch];
		}
	}

	// Single release-store makes all frame writes visible to the audio thread.
	_ring_write_pos.store(wp + p_frame_count, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Loader: fill ring buffer implementation
// ---------------------------------------------------------------------------

void SiOPMWaveStreamData::_fill_ring_buffer_impl() {
	if (!_valid) {
		return;
	}

	// Handle pending seek.
	if (_seek_requested.load(std::memory_order_acquire)) {
		_handle_seek();
	}

	// Compute available space.
	uint32_t rp = _ring_read_pos.load(std::memory_order_acquire);
	uint32_t wp = _ring_write_pos.load(std::memory_order_relaxed);
	int space = _ring_capacity - (int)(wp - rp);

	if (space <= 0) {
		return;
	}

	int frames_to_fill = (space < FILL_CHUNK_SIZE) ? space : FILL_CHUNK_SIZE;

	// Open file if not already open (first fill on loader thread).
	if (_loader_file.is_null()) {
		_loader_file = FileAccess::open(_file_path, FileAccess::READ);
		if (_loader_file.is_null()) {
			_active.store(false, std::memory_order_release);
			return;
		}
	}

	// Temporary buffer for decoded source-frame output.
	std::vector<double> temp(frames_to_fill * _channel_count);
	int produced = _produce_frames(temp.data(), frames_to_fill);

	if (produced > 0) {
		_ring_write_frames(temp.data(), produced);
	}
}

// ---------------------------------------------------------------------------
// Loader: handle seek
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Loader: reset decode state to a source-frame position
// ---------------------------------------------------------------------------

void SiOPMWaveStreamData::_reset_decode_to_sample(int64_t p_pos_sample) {
	p_pos_sample = CLAMP(p_pos_sample, (int64_t)0, (int64_t)_total_source_frames);
	_file_read_pos_frames = p_pos_sample;
	_decode_pos_frames = p_pos_sample;
	_decode_buf_valid = 0;
}

void SiOPMWaveStreamData::_handle_seek() {
	int64_t target_sample = _seek_target.load(std::memory_order_relaxed);
	_seek_requested.store(false, std::memory_order_relaxed);

	// Clamp to valid range.
	if (target_sample < 0) {
		target_sample = 0;
	}
	if (target_sample > _total_source_frames) {
		target_sample = _total_source_frames;
	}

	_reset_decode_to_sample(target_sample);

	// Flush ring buffer.
	uint32_t pos = _ring_write_pos.load(std::memory_order_relaxed);
	_ring_read_pos.store(pos, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Lock-free MPSC work queue (Treiber stack)
// ---------------------------------------------------------------------------

void SiOPMWaveStreamData::_s_enqueue(SiOPMWaveStreamData *p_instance) {
	// Deduplicate: if this instance is already in the queue, skip.
	// exchange() is atomic — exactly one caller wins and pushes.
	if (p_instance->_enqueued.exchange(true, std::memory_order_acq_rel)) {
		return;
	}

	// Push onto the Treiber stack via CAS.
	SiOPMWaveStreamData *old_head = _s_queue_head.load(std::memory_order_relaxed);
	do {
		p_instance->_next_in_queue.store(old_head, std::memory_order_relaxed);
	} while (!_s_queue_head.compare_exchange_weak(old_head, p_instance,
			std::memory_order_release, std::memory_order_relaxed));
}

void SiOPMWaveStreamData::_s_wait_until_idle(SiOPMWaveStreamData *p_instance) {
	while (p_instance->_enqueued.load(std::memory_order_acquire) ||
			p_instance->_processing.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
}

// ---------------------------------------------------------------------------
// Static loader thread
// ---------------------------------------------------------------------------

void SiOPMWaveStreamData::_s_loader_thread_func() {
	while (_s_loader_running.load(std::memory_order_relaxed)) {
		// Atomically steal the entire queue.
		SiOPMWaveStreamData *batch = _s_queue_head.exchange(nullptr, std::memory_order_acquire);

		if (!batch) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		// Process each item in the stolen batch. No mutex is held during
		// _fill_ring_buffer_impl(), so activate()/deactivate() on any other
		// instance proceed without blocking.
		while (batch) {
			// Save the next pointer before we allow re-enqueue.
			SiOPMWaveStreamData *next = batch->_next_in_queue.load(std::memory_order_relaxed);
			batch->_next_in_queue.store(nullptr, std::memory_order_relaxed);

			// Mark as processing BEFORE clearing enqueued. This ensures
			// _s_wait_until_idle() always sees _processing == true if the
			// loader is between clearing _enqueued and finishing the fill.
			batch->_processing.store(true, std::memory_order_release);
			batch->_enqueued.store(false, std::memory_order_release);

			if (batch->_active.load(std::memory_order_acquire)) {
				batch->_fill_ring_buffer_impl();
			}

			batch->_processing.store(false, std::memory_order_release);
			batch = next;
		}
	}
}

void SiOPMWaveStreamData::_s_ensure_loader_running() {
	// CAS ensures only one thread creates the loader thread.
	bool expected = false;
	if (_s_loader_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		_s_loader_thread = std::thread(_s_loader_thread_func);
	}
}

void SiOPMWaveStreamData::shutdown_loader() {
	if (!_s_loader_running.load(std::memory_order_relaxed)) {
		return;
	}
	_s_loader_running.store(false, std::memory_order_release);
	if (_s_loader_thread.joinable()) {
		_s_loader_thread.join();
	}

	// Drain any remaining queue entries (clear enqueued/processing flags).
	SiOPMWaveStreamData *batch = _s_queue_head.exchange(nullptr, std::memory_order_acquire);
	while (batch) {
		SiOPMWaveStreamData *next = batch->_next_in_queue.load(std::memory_order_relaxed);
		batch->_next_in_queue.store(nullptr, std::memory_order_relaxed);
		batch->_enqueued.store(false, std::memory_order_release);
		batch->_processing.store(false, std::memory_order_release);
		batch = next;
	}
}
