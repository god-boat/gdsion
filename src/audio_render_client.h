/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef AUDIO_RENDER_CLIENT_H
#define AUDIO_RENDER_CLIENT_H

// Backend-neutral render interface. Any audio backend (Godot fallback,
// WASAPI, Oboe, etc.) can pull rendered audio through this interface
// without knowing about the SiON engine internals.
//
// Named PoolyRenderClient (not IAudioRenderClient) to avoid collision
// with the Windows COM IAudioRenderClient from audioclient.h.
class PoolyRenderClient {
public:
	virtual ~PoolyRenderClient() = default;

	// Renders interleaved float audio into the provided output buffer.
	// Returns the number of frames actually written.
	// p_output: destination buffer, must hold at least p_frames * p_channels floats
	// p_frames: number of frames requested
	// p_channels: number of interleaved channels (currently only 2 supported)
	virtual int render_interleaved(float *p_output, int p_frames, int p_channels) = 0;
};

#endif // AUDIO_RENDER_CLIENT_H
