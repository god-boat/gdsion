/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_OFFLINE_RENDERER_H
#define SION_OFFLINE_RENDERER_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>

using namespace godot;

class SiONDriver;
class SiOPMSoundChip;
class SiEffector;
class SiMMLSequencer;

// Block-by-block offline audio renderer for SiONDriver.
//
// Designed for export pipelines that schedule notes dynamically (e.g. arrangement-based
// playback) rather than from pre-compiled MML sequences. The caller drives the render
// loop from GDScript, processing the arrangement step-by-step and firing notes via the
// driver's mailbox API between render blocks:
//
//   var renderer = SiONOfflineRenderer.new()
//   renderer.begin(driver)      # driver already has instruments/effects configured
//   while not done:
//       # check step boundaries, fire notes via driver.mailbox_key_on() etc.
//       var block = renderer.render_block()
//       output.append_array(block)
//   renderer.finish()
//
// Each render_block() call processes exactly one internal buffer (buffer_length frames)
// and returns interleaved stereo float32 samples. The driver's mailbox is drained at
// the start of each block, so notes queued via mailbox_key_on() etc. take effect
// immediately on the next block boundary — zero latency, unlike the live capture path
// which has a deferred-signal frame delay.
//
// IMPORTANT: The driver must already be in streaming mode with all instruments, effects,
// and BPM configured before calling begin(). The renderer does NOT modify any driver
// state — it simply calls the same render pipeline that generate_audio() uses, but
// synchronously on the calling thread instead of the audio thread.
class SiONOfflineRenderer : public RefCounted {
	GDCLASS(SiONOfflineRenderer, RefCounted)

	SiONDriver *_driver = nullptr;
	SiOPMSoundChip *_sound_chip = nullptr;
	SiEffector *_effector = nullptr;
	SiMMLSequencer *_sequencer = nullptr;

	bool _active = false;
	int _buffer_length = 0;  // frames per block (from driver)
	int64_t _total_frames_rendered = 0;

	bool _cache_driver_internals();

protected:
	static void _bind_methods();

public:
	// Prepares for offline rendering using an already-configured driver.
	// The driver should be in streaming mode (stream() called) with instruments,
	// effects, and BPM already set up. This does NOT modify driver state — it simply
	// caches internal pointers for direct render pipeline access.
	bool begin(SiONDriver *p_driver);

	// Processes one internal buffer block and returns the audio.
	// Returns interleaved stereo float32 samples (buffer_length * 2 elements).
	// The driver's mailbox is drained at the start, so any notes queued since the
	// last call will take effect on this block's boundary.
	PackedFloat32Array render_block();

	// Convenience: renders multiple blocks in a tight loop.
	// Returns interleaved stereo float32 (p_block_count * buffer_length * 2 elements).
	PackedFloat32Array render_blocks(int p_block_count);

	// Finishes offline rendering. Releases internal references.
	// The driver remains in its current state for normal use.
	void finish();

	// Returns true if the renderer is active (between begin/finish).
	bool is_active() const { return _active; }

	// Returns the number of frames per render block.
	int get_block_size_frames() const { return _buffer_length; }

	// Returns the number of samples per render block (frames * 2 for stereo).
	int get_block_size_samples() const { return _buffer_length * 2; }

	// Returns total frames rendered since begin() was called.
	int64_t get_total_frames_rendered() const { return _total_frames_rendered; }

	// Returns total time rendered in seconds (based on driver sample rate).
	double get_total_time_rendered() const;

	// Returns the driver's sample rate.
	double get_sample_rate() const;

	SiONOfflineRenderer() {}
	~SiONOfflineRenderer() {}
};

#endif // SION_OFFLINE_RENDERER_H
