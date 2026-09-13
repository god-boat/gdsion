/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef POOLY_TIMING_CLIENT_H
#define POOLY_TIMING_CLIENT_H

#include <cstdint>

// Ties the engine's rendered-frame clock to host time. Host time is
// std::chrono::steady_clock nanoseconds; every extension in the process reads
// the same clock, so the value crosses the extension boundary unchanged.
struct PoolyFrameClockSnapshot {
	// Rendered frame count at the start of the anchoring render call.
	int64_t frame_position = 0;
	// Host time at which that render call began.
	int64_t host_time_ns = 0;
	int sample_rate = 0;
};

// Backend-neutral timing interface, the counterpart of PoolyRenderClient.
// Sibling extensions (pooly_midi_io) read the engine's audio-frame clock
// through it without knowing about the engine.
class PoolyTimingClient {
public:
	virtual ~PoolyTimingClient() = default;

	// Fills r_snapshot with the latest anchor. Returns false until the engine
	// has rendered once. After a clock reset the anchor stays stale until the
	// next render call; consumers detect the reset when frame_position moves
	// backwards. Safe to call from any thread.
	virtual bool get_frame_clock(PoolyFrameClockSnapshot &r_snapshot) const = 0;
};

#endif // POOLY_TIMING_CLIENT_H
