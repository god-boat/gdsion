/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_channel_stream.h"

#include "sion_enums.h"
#include "chip/siopm_channel_params.h"
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"
#include "chip/wave/siopm_wave_base.h"
#include "chip/wave/siopm_wave_stream_data.h"
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cmath>

static constexpr int kSTREAM_GAIN_MIN_DB = -36;
static constexpr int kSTREAM_GAIN_MAX_DB = 36;

// ---------------------------------------------------------------------------
// Channel params (minimal — stream channel has no envelope or LFO)
// ---------------------------------------------------------------------------

void SiOPMChannelStream::get_channel_params(const Ref<SiOPMChannelParams> &p_params) const {
	p_params->set_operator_count(1);

	for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
		p_params->set_master_volume(i, _volumes[i]);
	}
	p_params->set_instrument_gain_db(get_instrument_gain_db());
	p_params->set_pan(_pan);
}

void SiOPMChannelStream::set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation) {
	if (p_params->get_operator_count() == 0) {
		return;
	}

	if (p_with_volume) {
		for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			_volumes.write[i] = p_params->get_master_volume(i);
		}

		_has_effect_send = false;
		for (int i = 1; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			if (_volumes[i] > 0) {
				_has_effect_send = true;
				break;
			}
		}

		_pan = p_params->get_pan();
	}
	set_instrument_gain_db(p_params->get_instrument_gain_db());

	// Filter support (shared with base class).
	_filter_type = p_params->get_filter_type();
	{
		int filter_cutoff = p_params->get_filter_cutoff();
		int filter_resonance = p_params->get_filter_resonance();
		int filter_ar = p_params->get_filter_attack_rate();
		int filter_dr1 = p_params->get_filter_decay_rate1();
		int filter_dr2 = p_params->get_filter_decay_rate2();
		int filter_rr = p_params->get_filter_release_rate();
		int filter_dc1 = p_params->get_filter_decay_offset1();
		int filter_dc2 = p_params->get_filter_decay_offset2();
		int filter_sc = p_params->get_filter_sustain_offset();
		int filter_rc = p_params->get_filter_release_offset();
		set_sv_filter(filter_cutoff, filter_resonance, filter_ar, filter_dr1, filter_dr2,
			filter_rr, filter_dc1, filter_dc2, filter_sc, filter_rc);
	}
}

// ---------------------------------------------------------------------------
// Wave data binding
// ---------------------------------------------------------------------------

void SiOPMChannelStream::set_wave_data(const Ref<SiOPMWaveBase> &p_wave_data) {
	_stream_data = p_wave_data;
}

// ---------------------------------------------------------------------------
// Pitch
// ---------------------------------------------------------------------------

void SiOPMChannelStream::_recalc_pitch_step() {
	if (_warp_mode == 1 /* REPITCH */ && _clip_bpm > 0.0) {
		// REPITCH: playback rate = driver_bpm / clip_bpm.
		// User pitch (_pitch_cents) is ignored — pitch is fully determined
		// by the BPM ratio (turntable/varispeed behavior).
		double driver_bpm = _sound_chip ? _sound_chip->get_bpm() : 120.0;
		_pitch_step = driver_bpm / _clip_bpm;
	} else if (_warp_mode == 3 /* TONES */) {
		// TONES: user pitch controls grain playback rate (pitch-shifting).
		// The grain source position advances at the BPM ratio to keep timing,
		// while _pitch_step controls the speed at which individual grains read
		// the source audio, producing the pitch shift.
		_pitch_step = std::pow(2.0, (double)_pitch_cents / 1200.0);
	} else if (_warp_mode == 4 /* TEXTURE */) {
		// TEXTURE: grain playback rate is always 1:1 (no pitch shift).
		// Time-stretching is handled entirely by the granular engine.
		_pitch_step = 1.0;
	} else {
		// OFF / BEATS / COMPLEX: user pitch only.
		_pitch_step = std::pow(2.0, (double)_pitch_cents / 1200.0);
	}
}

// ---------------------------------------------------------------------------
// Effective clip length helper
// ---------------------------------------------------------------------------

int64_t SiOPMChannelStream::_effective_clip_length() const {
	if (_out_sample > 0) {
		return _out_sample - _in_sample;
	}
	if (_stream_data.is_valid()) {
		return _stream_data->get_total_frames_48k() - _in_sample;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Fade envelope
// ---------------------------------------------------------------------------

double SiOPMChannelStream::_compute_fade_envelope(double p_source_frame) const {
	double env = 1.0;

	// Fade-in ramp: only on the very first loop iteration.
	if (_fade_in_frames > 0 && _loops_completed == 0 && p_source_frame < (double)_fade_in_frames) {
		env *= p_source_frame / (double)_fade_in_frames;
	}

	// Fade-out ramp: only when NOT looping (one-shot clips or final stop).
	if (_fade_out_frames > 0 && !_looping) {
		int64_t total_length = _effective_clip_length();
		if (total_length > 0) {
			int64_t fade_out_start = total_length - _fade_out_frames;
			if (p_source_frame >= (double)fade_out_start && p_source_frame < (double)total_length) {
				double remaining = (double)total_length - p_source_frame;
				env *= remaining / (double)_fade_out_frames;
			}
		}
	}

	return env;
}

// ---------------------------------------------------------------------------
// Processing: note_on / note_off
// ---------------------------------------------------------------------------

void SiOPMChannelStream::note_on() {
	if (_stream_data.is_null() || !_stream_data->is_valid()) {
		_is_idling = true;
		return;
	}

	cancel_kill_fade();

	// Reset playback state.
	_playback_pos = 0.0;
	_source_frames_elapsed = 0.0;
	_playing = true;
	_reached_end = false;
	_is_idling = false;
	_is_note_on = true;
	_loops_completed = 0;

	// Sync trim to stream data.
	_stream_data->set_in_sample(_in_sample);
	_stream_data->set_out_sample(_out_sample);

	// Sync loop state to stream data so the loader wraps at loop boundaries.
	_stream_data->set_looping(_looping);
	_stream_data->set_loop_region(_loop_start_48k, _loop_end_48k);

	// Reset granular state for TONES/TEXTURE modes (preserves grain_size/flux params).
	{
		double gs = _warp.get_grain_size();
		double fl = _warp.get_flux();
		_warp.reset();
		_warp.set_grain_size(gs);
		_warp.set_flux(fl);
	}

	// Seek the stream data back to the clip start so the ring buffer is
	// refilled from in_sample. This handles retrigger/loop: without it the
	// ring buffer would still be positioned at the end of the previous
	// playback. seek() is lock-free (atomics + Treiber-stack enqueue) so
	// it is safe to call from the audio thread. The loader thread picks up
	// the refill request and fills the ring buffer asynchronously; buffer()
	// handles any brief underrun with silence until data arrives.
	_stream_data->seek(_in_sample);

	// Activate the stream (registers with loader if not already).
	_stream_data->activate();

	// Trigger filter EG start.
	SiOPMChannelBase::note_on();
}

void SiOPMChannelStream::note_off() {
	if (!_playing) {
		return;
	}

	_is_note_on = false;
	_playing = false;

	// Use the base class kill-fade for a click-safe stop.
	start_kill_fade();

	SiOPMChannelBase::note_off();
}

// ---------------------------------------------------------------------------
// Processing: buffer
// ---------------------------------------------------------------------------

void SiOPMChannelStream::buffer(int p_length) {
	if (_is_idling || _stream_data.is_null() || !_stream_data->is_valid() || !_playing) {
		buffer_no_process(p_length);
		return;
	}

	int channels = _stream_data->get_channel_count();

	// Determine effective clip/loop end (relative to in_sample, in source 48 kHz frames).
	int64_t effective_end = _effective_clip_length();
	if (effective_end <= 0) {
		buffer_no_process(p_length);
		return;
	}

	// Compute the effective loop end relative to _in_sample. When looping, the
	// channel wraps _source_frames_elapsed at this boundary. The ring buffer
	// already contains continuous data across the wrap (the loader handles it).
	int64_t effective_loop_end = effective_end;
	double loop_offset = 0.0;
	if (_looping) {
		int64_t le = (_loop_end_48k > 0) ? _loop_end_48k : (_out_sample > 0 ? _out_sample : (_stream_data->get_total_frames_48k()));
		int64_t ls = (_loop_start_48k > 0) ? _loop_start_48k : _in_sample;
		// Convert to relative (from _in_sample).
		effective_loop_end = le - _in_sample;
		loop_offset = (double)(ls - _in_sample);
		if (effective_loop_end <= (int64_t)loop_offset) {
			effective_loop_end = effective_end;
		}
	}

	// Preserve the start of output pipes.
	SinglyLinkedList<int>::Element *left_start = _out_pipe->get();
	SinglyLinkedList<int>::Element *left_write = left_start;
	SinglyLinkedList<int>::Element *right_start = nullptr;
	SinglyLinkedList<int>::Element *right_write = nullptr;
	if (channels == 2) {
		if (!_out_pipe2) {
			_out_pipe2 = _sound_chip->get_pipe(3, _buffer_index);
		}
		right_start = _out_pipe2->get();
		right_write = right_start;
	}

	// Branch: granular modes (TONES=3, TEXTURE=4) vs standard playback.
	bool granular_mode = (_warp_mode == 3 || _warp_mode == 4);

	if (granular_mode) {
		// Granular overlap-add engine (delegated to SiOPMWarpProcessor).
		//
		// Source advance rate: determines how fast we move through the source
		// material, independent of grain pitch. For tempo-sync when BPM info
		// is available, source advances at (driver_bpm / clip_bpm); otherwise
		// 1:1.
		double source_advance = 1.0;
		if (_clip_bpm > 0.0) {
			double driver_bpm = _sound_chip ? _sound_chip->get_bpm() : 120.0;
			source_advance = driver_bpm / _clip_bpm;
		}

		// Lambda: read from the ring buffer at an offset.
		auto ring_reader = [&](int off, int ch) -> double {
			return _stream_data->ring_read_sample(off, ch);
		};

		for (int i = 0; i < p_length; i++) {
		// End/loop check on source position.
		if (_source_frames_elapsed >= (double)effective_loop_end || _reached_end) {
			if (_looping && !_reached_end) {
				double overshoot = _source_frames_elapsed - (double)effective_loop_end;
				_source_frames_elapsed = loop_offset + overshoot;
				_warp.set_source_pos(_source_frames_elapsed);
				_loops_completed++;
				// Fall through to the read/write code below (matching
				// the sampler's loop pattern — no output sample is skipped).
			} else {
					start_kill_fade();
					_playing = false;
					for (; i < p_length; i++) {
						left_write->value = 0;
						left_write = left_write->next();
						if (channels == 2 && right_write) {
							right_write->value = 0;
							right_write = right_write->next();
						}
					}
					break;
				}
			}

			// Schedule new grains on hop boundaries.
			_warp.schedule_grain_if_needed(_warp.get_source_pos(), _pitch_step, _warp_mode);

			// Read granular output.
			int avail = _stream_data->ring_available();
			double sampleL = _warp.read_granular(ring_reader, avail, 0, channels, _pitch_step);
			double sampleR = (channels == 2)
				? _warp.read_granular(ring_reader, avail, 1, channels, _pitch_step)
				: sampleL;

			// Apply gain and fade envelope.
			double fade = _compute_fade_envelope(_source_frames_elapsed);
			double amplitude = _clip_gain * fade;
			double outL = sampleL * amplitude;
			double outR = sampleR * amplitude;

			// Convert to engine int domain and write.
			int vL = CLAMP((int)(outL * 8192.0), -8192, 8191);
			left_write->value = vL;
			left_write = left_write->next();
			if (channels == 2 && right_write) {
				int vR = CLAMP((int)(outR * 8192.0), -8192, 8191);
				right_write->value = vR;
				right_write = right_write->next();
			}

			// Advance source position at the BPM-sync rate.
			_warp.advance(source_advance);
			_source_frames_elapsed += source_advance;

			// Advance the ring buffer read pointer to keep up with the
			// granular source position. We keep a margin of ring data ahead
			// so grains can read with interpolation.
			int ring_advance = (int)_warp.get_source_pos();
			if (ring_advance > 0) {
				// Limit consumption so we don't invalidate active grains'
				// read positions. When source_advance > _pitch_step (clip
				// BPM < project BPM), the source position races ahead of
				// grain read cursors. Without this cap, adjust_positions()
				// pushes grain read_pos negative, failing the bounds check
				// in read_granular() and producing silence.
				int safe_advance = ring_advance;
				for (int s = 0; s < 2; s++) {
					const SiOPMWarpProcessor::Grain &g = _warp.get_grain(s);
					if (g.active) {
						int grain_floor = (int)g.read_pos;
						if (grain_floor < safe_advance) {
							safe_advance = grain_floor;
						}
					}
				}
				if (safe_advance < 0) {
					safe_advance = 0;
				}

				int available = _stream_data->ring_available();
				int to_consume = MIN(safe_advance, available - 2);
				if (to_consume > 0) {
					_stream_data->ring_advance_read(to_consume);
					_warp.adjust_positions(to_consume);
				}
			}
		}
	} else {
		// Standard (non-granular) playback path.
		int ring_frames_consumed = 0;

		for (int i = 0; i < p_length; i++) {
		// End/loop check — mirrors sampler's pattern: check BEFORE interpolation.
		// Overshoot preservation: loop_offset + overshoot (sampler: loop_point + (index - end_point)).
		if (_source_frames_elapsed >= (double)effective_loop_end || _reached_end) {
			if (_looping && !_reached_end) {
				// Wrap with overshoot preservation (matching sampler pattern).
				double overshoot = _source_frames_elapsed - (double)effective_loop_end;
				_source_frames_elapsed = loop_offset + overshoot;
				_loops_completed++;
				// Fall through to the read/write code below. The ring buffer
				// contains continuous data across the loop boundary (the
				// loader wrapped at loop_end and continued filling from
				// loop_start), so _playback_pos is NOT reset.
			} else {
					// One-shot end: use kill-fade for click-safe stop (adopted
					// from sampler's click guard pattern). This keeps the channel
					// alive so the SV filter can decay naturally on zero-input.
					start_kill_fade();
					_playing = false;
					for (; i < p_length; i++) {
						left_write->value = 0;
						left_write = left_write->next();
						if (channels == 2 && right_write) {
							right_write->value = 0;
							right_write = right_write->next();
						}
					}
					break;
				}
			}

			// Check ring buffer availability (need 2 frames for interpolation).
			int available = _stream_data->ring_available();
			int needed = (int)_playback_pos + 2;

			if (available < needed) {
				// Underrun — output silence for this sample.
				left_write->value = 0;
				left_write = left_write->next();
				if (channels == 2 && right_write) {
					right_write->value = 0;
					right_write = right_write->next();
				}
				_source_frames_elapsed += _pitch_step;
				_playback_pos += _pitch_step;
				continue;
			}

			// Linear interpolation.
			int base_idx = (int)_playback_pos;
			double frac = _playback_pos - base_idx;

			double sampleL = _stream_data->ring_read_sample(base_idx, 0)
				+ (_stream_data->ring_read_sample(base_idx + 1, 0)
					- _stream_data->ring_read_sample(base_idx, 0)) * frac;

			double sampleR = sampleL;
			if (channels == 2) {
				sampleR = _stream_data->ring_read_sample(base_idx, 1)
					+ (_stream_data->ring_read_sample(base_idx + 1, 1)
						- _stream_data->ring_read_sample(base_idx, 1)) * frac;
			}

			// Apply native gain and fade envelope (source-domain position for fade).
			double fade = _compute_fade_envelope(_source_frames_elapsed);
			double amplitude = _clip_gain * fade;
			double outL = sampleL * amplitude;
			double outR = sampleR * amplitude;

			// Convert to engine int domain and write.
			int vL = CLAMP((int)(outL * 8192.0), -8192, 8191);
			left_write->value = vL;
			left_write = left_write->next();
			if (channels == 2 && right_write) {
				int vR = CLAMP((int)(outR * 8192.0), -8192, 8191);
				right_write->value = vR;
				right_write = right_write->next();
			}

			// Advance playback position (pitch-aware: each output sample consumes
			// _pitch_step source frames).
			_playback_pos += _pitch_step;
			_source_frames_elapsed += _pitch_step;

			// Consume integer frames from the ring buffer.
			int new_base = (int)_playback_pos;
			if (new_base > ring_frames_consumed) {
				int to_consume = new_base - ring_frames_consumed;
				_stream_data->ring_advance_read(to_consume);
				_playback_pos -= to_consume;
				ring_frames_consumed = 0; // Reset since we adjusted _playback_pos.
			}
		}
	}

	// Request refill if ring buffer is running low.
	if (_stream_data.is_valid()) {
		int available = _stream_data->ring_available();
		// Use half the default ring capacity as threshold.
		if (available < SiOPMWaveStreamData::DEFAULT_RING_CAPACITY / 2) {
			_stream_data->request_refill();
		}
	}

	// Apply filter on output pipes.
	if (_filter_on) {
		_apply_sv_filter(left_start, p_length, _filter_variables);
		if (channels == 2 && right_start) {
			_apply_sv_filter(right_start, p_length, _filter_variables2);
		}
	}

	// Apply kill fade if pending.
	if (_kill_fade_remaining_samples > 0) {
		if (channels == 2 && right_start) {
			_apply_kill_fade_stereo(left_start, right_start, p_length);
		} else {
			_apply_kill_fade(left_start, p_length);
		}
	}

	// Write to streams.
	if (!_mute) {
		if (channels == 1) {
			_write_stream_mono(left_start, p_length);
		} else if (right_start) {
			_write_stream_stereo(left_start, right_start, p_length);
		}
	}

	// Advance pipe cursors.
	_out_pipe->set(left_write);
	if (channels == 2 && right_write) {
		_out_pipe2->set(right_write);
	}

	_buffer_index += p_length;
}

void SiOPMChannelStream::buffer_no_process(int p_length) {
	// Rotate output buffers (mirrors sampler channel pattern).
	int pipe_index = (_buffer_index + p_length) & (_sound_chip->get_buffer_length() - 1);
	if (_output_mode == OutputMode::OUTPUT_STANDARD) {
		_out_pipe = _sound_chip->get_pipe(4, pipe_index);
		_out_pipe2 = _sound_chip->get_pipe(3, pipe_index);
		_base_pipe = _sound_chip->get_zero_buffer();
	} else {
		if (_out_pipe) {
			_out_pipe->advance(p_length);
		}
		if (_out_pipe2) {
			_out_pipe2->advance(p_length);
		}
		_base_pipe = (_output_mode == OutputMode::OUTPUT_ADD ? _out_pipe : _sound_chip->get_zero_buffer());
	}

	if (_input_mode == InputMode::INPUT_PIPE && _in_pipe) {
		_in_pipe->advance(p_length);
	}
	if (_ring_pipe) {
		_ring_pipe->advance(p_length);
	}

	_buffer_index += p_length;
}

// ---------------------------------------------------------------------------
// Initialize / reset
// ---------------------------------------------------------------------------

void SiOPMChannelStream::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	SiOPMChannelBase::initialize(p_prev, p_buffer_index);
	reset();
	_out_pipe2 = _sound_chip->get_pipe(3, p_buffer_index);
	_filter_variables2[0] = 0;
	_filter_variables2[1] = 0;
	_filter_variables2[2] = 0;
}

void SiOPMChannelStream::reset() {
	_is_note_on = false;
	_is_idling = true;
	_playing = false;
	_reached_end = false;

	_stream_data = Ref<SiOPMWaveStreamData>();
	_playback_pos = 0.0;
	_source_frames_elapsed = 0.0;
	_gain_db = 0;
	_clip_gain = 1.0;
	_pitch_step = 1.0;
	_pitch_cents = 0;
	_fade_in_frames = 0;
	_fade_out_frames = 0;
	_in_sample = 0;
	_out_sample = 0;
	_warp_mode = 0;
	_clip_bpm = 0.0;
	_warp.reset();

	_looping = false;
	_loop_start_48k = 0;
	_loop_end_48k = 0;
	_loops_completed = 0;
}

// ---------------------------------------------------------------------------
// Stream writers (mirror sampler channel)
// ---------------------------------------------------------------------------

void SiOPMChannelStream::_write_stream_mono(SinglyLinkedList<int>::Element *p_output, int p_length) {
	double volume_coef = _sound_chip->get_sampler_volume() * _instrument_gain;
	int pan = CLAMP(_pan, 0, 128);

	if (_has_effect_send) {
		for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			if (_volumes[i] > 0) {
				SiOPMStream *stream = _streams[i] ? _streams[i] : _sound_chip->get_stream_slot(i);
				if (stream) {
					stream->write(p_output, _buffer_index, p_length, _volumes[i] * volume_coef, pan);
				}
			}
		}
	} else {
		SiOPMStream *stream = _streams[0] ? _streams[0] : _sound_chip->get_output_stream();
		stream->write(p_output, _buffer_index, p_length, _volumes[0] * volume_coef, pan);
	}
}

void SiOPMChannelStream::_write_stream_stereo(SinglyLinkedList<int>::Element *p_output_left, SinglyLinkedList<int>::Element *p_output_right, int p_length) {
	double volume_coef = _sound_chip->get_sampler_volume() * _instrument_gain;
	int pan = CLAMP(_pan, 0, 128);

	if (_has_effect_send) {
		for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			if (_volumes[i] > 0) {
				SiOPMStream *stream = _streams[i] ? _streams[i] : _sound_chip->get_stream_slot(i);
				if (stream) {
					stream->write_stereo(p_output_left, p_output_right, _buffer_index, p_length, _volumes[i] * volume_coef, pan);
				}
			}
		}
	} else {
		SiOPMStream *stream = _streams[0] ? _streams[0] : _sound_chip->get_output_stream();
		stream->write_stereo(p_output_left, p_output_right, _buffer_index, p_length, _volumes[0] * volume_coef, pan);
	}
}

// ---------------------------------------------------------------------------
// Real-time param setters
// ---------------------------------------------------------------------------

void SiOPMChannelStream::set_stream_gain(double p_gain_db) {
	int clamped = CLAMP((int)p_gain_db, kSTREAM_GAIN_MIN_DB, kSTREAM_GAIN_MAX_DB);
	_gain_db = clamped;
	_clip_gain = std::pow(2.0, (double)clamped / 6.0);
}

void SiOPMChannelStream::set_stream_pan(int p_pan) {
	// Match SiOPMChannelBase::set_pan convention: input is -64..+64, internal is 0..128.
	_pan = CLAMP(p_pan, -64, 64) + 64;
}

void SiOPMChannelStream::set_stream_pitch_cents(int p_cents) {
	_pitch_cents = p_cents;
	_recalc_pitch_step();
}

void SiOPMChannelStream::set_stream_fade_in(int p_frames) {
	_fade_in_frames = MAX(p_frames, 0);
}

void SiOPMChannelStream::set_stream_fade_out(int p_frames) {
	_fade_out_frames = MAX(p_frames, 0);
}

void SiOPMChannelStream::set_stream_in_sample(int64_t p_sample) {
	_in_sample = MAX(p_sample, (int64_t)0);
	if (_stream_data.is_valid()) {
		int64_t old_in = _stream_data->get_in_sample();
		_stream_data->set_in_sample(_in_sample);

		// If the new in_sample is ahead of current playback position, seek forward.
		if (_playing && _in_sample > old_in) {
			int64_t offset = _in_sample - old_in;
			if (_source_frames_elapsed < (double)offset) {
				_stream_data->seek(_in_sample);
				_playback_pos = 0.0;
				_source_frames_elapsed = 0.0;
			}
		}
	}
}

void SiOPMChannelStream::set_stream_out_sample(int64_t p_sample) {
	_out_sample = MAX(p_sample, (int64_t)0);
	if (_stream_data.is_valid()) {
		_stream_data->set_out_sample(_out_sample);

		// If current position is past the new end, seek back to the clip
		// start instead of stopping. This lets the user freely drag the
		// endpoint without killing the stream — buffer() will handle
		// natural end-of-clip / loop logic when the position reaches
		// the new boundary again.
		if (_playing && _out_sample > 0) {
			int64_t effective_len = _out_sample - _in_sample;
			if (effective_len > 0 && _source_frames_elapsed >= (double)effective_len) {
				_stream_data->seek(_in_sample);
				_playback_pos = 0.0;
				_source_frames_elapsed = 0.0;
			}
		}
	}
}

void SiOPMChannelStream::set_stream_warp_mode(int p_mode) {
	_warp_mode = p_mode;
	_recalc_pitch_step();
}

void SiOPMChannelStream::set_stream_clip_bpm(double p_bpm) {
	_clip_bpm = MAX(p_bpm, 0.0);
	_recalc_pitch_step();
}

void SiOPMChannelStream::set_stream_grain_size(double p_grain_size) {
	_warp.set_grain_size(p_grain_size);
}

void SiOPMChannelStream::set_stream_flux(double p_flux) {
	_warp.set_flux(p_flux);
}

void SiOPMChannelStream::update_lfo_for_bpm() {
	SiOPMChannelBase::update_lfo_for_bpm();
	// When in BPM-dependent modes, recalculate pitch/rate.
	// REPITCH: pitch step = driver_bpm / clip_bpm.
	// TONES/TEXTURE: granular source_advance is computed per-buffer from BPM,
	// but _recalc_pitch_step() is needed for TONES pitch step.
	if ((_warp_mode == 1 || _warp_mode == 3 || _warp_mode == 4) && _clip_bpm > 0.0) {
		_recalc_pitch_step();
	}
}

// ---------------------------------------------------------------------------
// Loop control
// ---------------------------------------------------------------------------

void SiOPMChannelStream::set_stream_looping(bool p_looping) {
	_looping = p_looping;
	if (_stream_data.is_valid()) {
		_stream_data->set_looping(p_looping);
	}
}

void SiOPMChannelStream::set_stream_loop_region(int64_t p_start_48k, int64_t p_end_48k) {
	_loop_start_48k = MAX(p_start_48k, (int64_t)0);
	_loop_end_48k = MAX(p_end_48k, (int64_t)0);
	if (_stream_data.is_valid()) {
		_stream_data->set_loop_region(_loop_start_48k, _loop_end_48k);
	}
}

// ---------------------------------------------------------------------------
// Seek
// ---------------------------------------------------------------------------

void SiOPMChannelStream::seek_to(int64_t p_position_48k) {
	if (_stream_data.is_null()) {
		return;
	}

	_stream_data->seek(p_position_48k);
	_playback_pos = 0.0;

	// Compute source frames elapsed relative to in_sample.
	int64_t relative = p_position_48k - _in_sample;
	_source_frames_elapsed = (relative > 0) ? (double)relative : 0.0;
}

// ---------------------------------------------------------------------------

String SiOPMChannelStream::_to_string() const {
	String params = "";
	params += "gain=" + itos(_gain_db) + "dB, ";
	params += "pan=" + itos(_pan - 64) + ", ";
	params += "pitch=" + itos(_pitch_cents) + "c";
	return "SiOPMChannelStream: " + params;
}

SiOPMChannelStream::SiOPMChannelStream(SiOPMSoundChip *p_chip) : SiOPMChannelBase(p_chip) {
	// Empty.
}
