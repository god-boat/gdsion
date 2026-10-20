/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_SIGNAL_TAP_H
#define SION_SIGNAL_TAP_H

#include <godot_cpp/core/memory.hpp>
#include <atomic>
#include <cstdint>

using namespace godot;

// The newest mono frames at one point of a stream, for a display. The render
// owner writes them only while a reader holds the lease, and every read renews
// it, so a tap nobody reads stops capturing within LEASE_FRAMES.
struct SignalTap {
	static constexpr int CAPACITY = 8192; // Frames. A power of two.
	static constexpr int LEASE_FRAMES = 1 << 15;

	std::atomic<int> position{ -1 };
	std::atomic<int> lease_frames{ 0 };
	std::atomic<uint32_t> written{ 0 };
	std::atomic<float> frames[CAPACITY];

	SignalTap() { clear(); }

	void clear() {
		for (std::atomic<float> &frame : frames) {
			frame.store(0.0f, std::memory_order_relaxed);
		}
	}

	// Render owner. Downmixes interleaved stereo.
	void capture(const double *p_interleaved, int p_frame_count) {
		const uint32_t start = written.load(std::memory_order_relaxed);
		for (int i = 0; i < p_frame_count; i++) {
			const double mono = 0.5 * (p_interleaved[2 * i] + p_interleaved[2 * i + 1]);
			frames[(start + i) & (CAPACITY - 1)].store((float)mono, std::memory_order_relaxed);
		}
		written.store(start + p_frame_count, std::memory_order_release);
		lease_frames.fetch_sub(p_frame_count, std::memory_order_relaxed);
	}

	// Main thread. The newest p_frame_count frames, oldest first.
	void read(float *r_frames, int p_frame_count) const {
		const uint32_t end = written.load(std::memory_order_acquire);
		for (int i = 0; i < p_frame_count; i++) {
			r_frames[i] = frames[(end - p_frame_count + i) & (CAPACITY - 1)].load(std::memory_order_relaxed);
		}
	}
};

// The taps of one stream, at positions along its effect chain: 0 is the
// stream's input and k follows its k-th effect. A tap belongs to a position,
// not to an effect, so a chain edit needs no bookkeeping. The main thread
// leases taps and allocates them on first use; the render owner captures every
// leased position.
class SignalTaps {
	static constexpr int SLOTS = 4;

	// Filled in order, so the first empty slot ends the search.
	std::atomic<SignalTap *> _slots[SLOTS] = {};

public:
	// Render owner.
	void capture(int p_position, const double *p_interleaved, int p_frame_count) {
		for (std::atomic<SignalTap *> &slot : _slots) {
			SignalTap *tap = slot.load(std::memory_order_acquire);
			if (tap == nullptr) {
				return;
			}
			if (tap->lease_frames.load(std::memory_order_acquire) > 0 && tap->position.load(std::memory_order_relaxed) == p_position) {
				tap->capture(p_interleaved, p_frame_count);
			}
		}
	}

	// Main thread. Renews the tap at p_position, or takes over one whose lease
	// ran out. Null when every tap is leased at another position. A tap that
	// stopped capturing starts again from silence, so a read never mixes in
	// frames from before the gap.
	SignalTap *lease(int p_position) {
		SignalTap *idle = nullptr;
		for (std::atomic<SignalTap *> &slot : _slots) {
			SignalTap *tap = slot.load(std::memory_order_relaxed);
			if (tap == nullptr) {
				if (idle == nullptr) {
					idle = memnew(SignalTap);
					slot.store(idle, std::memory_order_release);
				}
				break;
			}
			if (tap->position.load(std::memory_order_relaxed) == p_position) {
				idle = tap;
				break;
			}
			if (idle == nullptr && tap->lease_frames.load(std::memory_order_relaxed) <= 0) {
				idle = tap;
			}
		}
		if (idle == nullptr) {
			return nullptr;
		}
		if (idle->lease_frames.load(std::memory_order_relaxed) <= 0) {
			idle->clear();
		}
		idle->position.store(p_position, std::memory_order_relaxed);
		// Releases the position with the lease the render owner acquires.
		idle->lease_frames.store(SignalTap::LEASE_FRAMES, std::memory_order_release);
		return idle;
	}

	// Main thread, while the render owner does not run this stream.
	void release_all() {
		for (std::atomic<SignalTap *> &slot : _slots) {
			SignalTap *tap = slot.load(std::memory_order_relaxed);
			if (tap == nullptr) {
				return;
			}
			tap->lease_frames.store(0, std::memory_order_relaxed);
			tap->position.store(-1, std::memory_order_relaxed);
		}
	}

	~SignalTaps() {
		for (std::atomic<SignalTap *> &slot : _slots) {
			SignalTap *tap = slot.load(std::memory_order_relaxed);
			if (tap != nullptr) {
				memdelete(tap);
			}
		}
	}
};

#endif // SION_SIGNAL_TAP_H
