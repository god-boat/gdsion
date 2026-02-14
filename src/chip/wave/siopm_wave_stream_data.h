/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SIOPM_WAVE_STREAM_DATA_H
#define SIOPM_WAVE_STREAM_DATA_H

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/templates/vector.hpp>
#include "chip/wave/siopm_wave_base.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace godot;

// Streaming wave data for audio clip playback.
//
// Provides a lock-free SPSC ring buffer of pre-resampled 48 kHz frames that the
// audio thread reads and a single shared background loader thread writes.
// Supports mono and stereo WAV files with 16-bit PCM, 24-bit PCM, or 32-bit float formats.
//
// Thread ownership:
//   - Immutable fields (file info, capacity) are safe to read from any thread after construction.
//   - Ring buffer: audio thread reads, loader thread writes. Atomic positions enforce ordering.
//   - Loader state (decode buffer, file handle, resample position): loader thread only.
//   - Shared flags (_active, _seek_requested, _enqueued, _processing): atomic.
class SiOPMWaveStreamData : public SiOPMWaveBase {
	GDCLASS(SiOPMWaveStreamData, SiOPMWaveBase)

public:
	// Default ring buffer capacity in 48 kHz frames (~340 ms stereo ≈ 256 KB).
	static constexpr int DEFAULT_RING_CAPACITY = 16384;
	// Number of 48 kHz frames to produce per loader fill cycle.
	static constexpr int FILL_CHUNK_SIZE = 4096;
	// Target sample rate for the engine.
	static constexpr int TARGET_SR = 48000;
	// Decode buffer size in source frames per read.
	static constexpr int DECODE_CHUNK_FRAMES = 4096;

private:
	// ---- Immutable after load_wav() (safe from any thread) ----

	String _file_path;
	int _source_sample_rate = 0;
	int _channel_count = 0;
	int _bits_per_sample = 0;
	int _audio_format = 0;        // 1 = PCM integer, 3 = IEEE float.
	int64_t _data_offset = 0;     // Byte offset of audio data chunk in the file.
	int64_t _data_size = 0;       // Byte size of the audio data chunk.
	int _bytes_per_frame = 0;     // = channel_count * (bits_per_sample / 8).
	int _total_source_frames = 0; // Total frames in the source file.
	int64_t _total_frames_48k = 0; // Total frames after resampling to 48 kHz.
	bool _valid = false;

	// ---- Ring buffer (SPSC: audio reads, loader writes) ----

	std::vector<double> _ring_data; // capacity * channel_count doubles.
	int _ring_capacity = 0;         // Power of 2, in frames.
	int _ring_mask = 0;             // = capacity - 1.
	std::atomic<uint32_t> _ring_read_pos{0};
	std::atomic<uint32_t> _ring_write_pos{0};

	// ---- Shared atomic flags ----

	std::atomic<bool> _active{false};
	std::atomic<bool> _seek_requested{false};
	std::atomic<int64_t> _seek_target{0};

	// ---- Lock-free MPSC queue (intrusive Treiber stack) ----
	//
	// Producers (audio thread via request_refill, main thread via activate/seek)
	// push this instance onto a shared stack using CAS on _s_queue_head.
	// The loader thread atomically steals the entire stack and processes items
	// without holding any mutex. This keeps activate()/deactivate() non-blocking
	// regardless of I/O happening on other instances.

	std::atomic<SiOPMWaveStreamData *> _next_in_queue{nullptr};
	std::atomic<bool> _enqueued{false};   // Deduplication: prevents double-enqueue.
	std::atomic<bool> _processing{false}; // True while the loader thread is in _fill_ring_buffer_impl().

	// Trim params (atomic for real-time updates via mailbox).
	std::atomic<int64_t> _in_sample{0};
	std::atomic<int64_t> _out_sample{0}; // 0 = play to EOF.

	// Loop region (48 kHz frame positions, absolute).
	// When looping is enabled, the loader wraps _decode_pos_48k back to
	// _loop_start_48k when it reaches _loop_end_48k, producing a continuous
	// stream of audio across loop boundaries with no seek/flush needed.
	std::atomic<int64_t> _loop_start_48k{0};  // Defaults to _in_sample.
	std::atomic<int64_t> _loop_end_48k{0};    // 0 = use _out_sample or EOF.
	std::atomic<bool> _looping{false};

	// ---- Loader-thread-only state ----

	Ref<FileAccess> _loader_file;
	Vector<double> _decode_buffer;       // Decoded source frames (interleaved doubles).
	int _decode_buf_valid = 0;           // Valid frames in _decode_buffer.
	int64_t _file_read_pos_frames = 0;   // Next source frame to read from file.
	double _resample_frac = 0.0;         // Fractional source position for streaming resampler.
	double _overlap_frame[2] = {0.0, 0.0}; // Last source frame of previous chunk (for continuity).
	bool _has_overlap = false;
	int64_t _decode_pos_48k = 0;         // 48 kHz frames produced into ring buffer so far.

	// WAV header parsing (called from main thread in load_wav).
	bool _parse_wav_header();

	// Decode raw WAV bytes to interleaved doubles in [-1, 1].
	void _decode_raw_to_doubles(const PackedByteArray &p_raw, int p_frame_count);

	// Loader-thread fill implementation.
	void _fill_ring_buffer_impl();

	// Read a chunk of source frames from the WAV file and decode to doubles.
	bool _refill_decode_buffer();

	// Produce resampled 48 kHz frames from the decode buffer into ring_out.
	// Returns the number of frames produced.
	int _produce_resampled_frames(double *r_out, int p_max_frames);

	// Handle pending seek on the loader thread.
	void _handle_seek();

	// Reset loader decode state to an absolute 48 kHz position.
	// Used by _handle_seek() and the loop wrap in _produce_resampled_frames().
	void _reset_decode_to_48k(int64_t p_pos_48k);

	// Write frames to the ring buffer (loader thread).
	void _ring_write_frames(const double *p_data, int p_frame_count);

	// ---- Static loader thread management ----

	static std::thread _s_loader_thread;
	static std::atomic<bool> _s_loader_running;

	// Global MPSC queue head. Producers push via CAS; the loader thread
	// atomically steals the entire list with exchange(nullptr).
	static std::atomic<SiOPMWaveStreamData *> _s_queue_head;

	static void _s_loader_thread_func();
	static void _s_ensure_loader_running();

	// Push an instance onto the MPSC work queue (lock-free, safe from any thread).
	static void _s_enqueue(SiOPMWaveStreamData *p_instance);

	// Spin-wait until the loader thread is no longer processing this instance
	// and it has been drained from the queue. Used by the destructor and
	// load_wav() to establish exclusive access before touching loader-thread state.
	static void _s_wait_until_idle(SiOPMWaveStreamData *p_instance);

protected:
	static void _bind_methods();

public:
	// ---- Construction / setup ----

	// Load a WAV file and prepare the ring buffer. Returns true on success.
	bool load_wav(const String &p_file_path, int p_ring_capacity = 0);

	bool is_valid() const { return _valid; }
	int get_channel_count() const { return _channel_count; }
	int get_source_sample_rate() const { return _source_sample_rate; }
	int64_t get_total_frames_48k() const { return _total_frames_48k; }

	// ---- Trim (real-time safe, atomic) ----

	int64_t get_in_sample() const { return _in_sample.load(std::memory_order_relaxed); }
	void set_in_sample(int64_t p_sample);

	int64_t get_out_sample() const { return _out_sample.load(std::memory_order_relaxed); }
	void set_out_sample(int64_t p_sample);

	// ---- Loop region (real-time safe, atomic) ----

	bool get_looping() const { return _looping.load(std::memory_order_relaxed); }
	void set_looping(bool p_looping);

	int64_t get_loop_start_48k() const { return _loop_start_48k.load(std::memory_order_relaxed); }
	int64_t get_loop_end_48k() const { return _loop_end_48k.load(std::memory_order_relaxed); }
	void set_loop_region(int64_t p_start_48k, int64_t p_end_48k);

	// ---- Audio-thread API (called by SiOPMChannelStream) ----

	// Number of 48 kHz frames available to read from the ring buffer.
	int ring_available() const;

	// Peek at a sample in the ring buffer at read_pos + p_offset.
	// p_channel: 0 = left/mono, 1 = right.
	double ring_read_sample(int p_offset, int p_channel) const;

	// Advance the read position by p_frames.
	void ring_advance_read(int p_frames);

	// Enqueue a refill request into the MPSC work queue (lock-free).
	// Called by the audio thread when the ring buffer drops below the low-water mark.
	void request_refill();

	// ---- Lifecycle ----

	// Activate streaming (registers with loader thread, starts filling).
	void activate();

	// Deactivate streaming (unregisters from loader, stops filling).
	void deactivate();

	// Seek to an absolute 48 kHz frame position. Flushes the ring buffer
	// and triggers a refill from the new position.
	void seek(int64_t p_position_48k);

	// Synchronous prefill: fills the ring buffer on the calling thread.
	// Used after construction or seek to ensure data is immediately available.
	void prefill_sync();

	// ---- Static cleanup ----

	// Stop the loader thread. Called during driver shutdown.
	static void shutdown_loader();

	//

	SiOPMWaveStreamData(const String &p_file_path = String(), int p_ring_capacity = 0);
	~SiOPMWaveStreamData();
};

#endif // SIOPM_WAVE_STREAM_DATA_H
