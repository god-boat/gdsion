/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_channel_base.h"

#include <godot_cpp/core/class_db.hpp>
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"
#include <cstring>
#include <cmath>

#define COPY_TL_TABLE(m_target, m_source)                        \
	for (int _i = 0; _i < SiOPMRefTable::TL_TABLE_SIZE; _i++) {  \
		m_target[_i] = m_source[_i];                             \
	}

static const int kInstrumentGainDbMin = -70;
static const int kInstrumentGainDbMax = 6;
static const int kInstrumentGainDbDefault = 0;

static inline double _db_to_linear(double p_db) {
	return std::pow(10.0, p_db / 20.0);
}


static double _beat_division_to_ms(int p_division, double p_bpm) {
	if (p_bpm <= 0) {
		p_bpm = 120.0; // Default BPM
	}
	double quarter_note_ms = 60000.0 / p_bpm;
	double multipliers[6] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
	int idx = CLAMP(p_division, 0, 5);
	return quarter_note_ms * multipliers[idx];
}

// Convert milliseconds to LFO timer step.
// Based on set_lfo_cycle_time formula:
// coef = sampling_rate / (1000.0 * 255.0)
// timer_step = (LFO_TIMER_INITIAL / (ms * coef)) << sample_rate_pitch_shift
static int _ms_to_lfo_timer_step(double p_ms, int p_sampling_rate, int p_sample_rate_pitch_shift) {
	if (p_ms <= 0) {
		return 0;
	}
	double coef = p_sampling_rate / (1000.0 * 256.0);
	int timer_step = (int)(SiOPMRefTable::LFO_TIMER_INITIAL / (p_ms * coef));
	return timer_step << p_sample_rate_pitch_shift;
}

// Calculate LFO timer step based on time mode, beat division, and BPM.
// This is the main conversion function used by the engine.
static int _calculate_lfo_timer_step(int p_time_mode, int p_value, double p_bpm, int p_sampling_rate, int p_sample_rate_pitch_shift) {
	switch (p_time_mode) {
		case SiOPMChannelBase::LFO_TIME_MODE_RATE:
			// Rate mode: value is the raw timer step (current behavior)
			return p_value;

		case SiOPMChannelBase::LFO_TIME_MODE_TIME:
			// Time mode: value is period in milliseconds
			return _ms_to_lfo_timer_step((double)p_value, p_sampling_rate, p_sample_rate_pitch_shift);

		case SiOPMChannelBase::LFO_TIME_MODE_SYNCED: {
			// Synced mode: value is beat division, calculate period from BPM
			double ms = _beat_division_to_ms(p_value, p_bpm);
			return _ms_to_lfo_timer_step(ms, p_sampling_rate, p_sample_rate_pitch_shift);
		}

		case SiOPMChannelBase::LFO_TIME_MODE_DOTTED: {
			// Dotted mode: multiply period by 1.5
			double ms = _beat_division_to_ms(p_value, p_bpm) * 1.5;
			return _ms_to_lfo_timer_step(ms, p_sampling_rate, p_sample_rate_pitch_shift);
		}

		case SiOPMChannelBase::LFO_TIME_MODE_TRIPLET: {
			// Triplet mode: multiply period by 2/3
			double ms = _beat_division_to_ms(p_value, p_bpm) * (2.0 / 3.0);
			return _ms_to_lfo_timer_step(ms, p_sampling_rate, p_sample_rate_pitch_shift);
		}

		default:
			return p_value;
	}
}

int SiOPMChannelBase::get_master_volume() const {
	return _volumes[0] * 128;
}

void SiOPMChannelBase::set_master_volume(int p_value) {
	int value = CLAMP(p_value, 0, 256);
	_volumes.write[0] = value * 0.0078125; // 0.0078125 = 1/128
}

void SiOPMChannelBase::set_instrument_gain_db(int p_db) {
	int value = CLAMP(p_db, kInstrumentGainDbMin, kInstrumentGainDbMax);
	_instrument_gain_db = value;
	if (value <= kInstrumentGainDbMin) {
		_instrument_gain = 0.0;
		return;
	}
	_instrument_gain = _db_to_linear((double)value);
}

// External value is in the -64 to 64 range (from all the way to the left,
// to all the way to the right). We store it as 0-128.
// [left volume]  = cos((pan + 64) / 128 * PI * 0.5) * volume;
// [right volume] = sin((pan + 64) / 128 * PI * 0.5) * volume;

int SiOPMChannelBase::get_pan() const {
	return _pan - 64;
}

void SiOPMChannelBase::set_pan(int p_value) {
	_pan = CLAMP(p_value, -64, 64) + 64;
}

//

void SiOPMChannelBase::set_filter_type(int p_type) {
	_filter_type = (p_type < 0 || p_type > 2) ? 0 : p_type;
}

// Volume control.

void SiOPMChannelBase::set_all_stream_send_levels(Vector<int> p_levels) {
	for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
		int value = p_levels[i];
		_volumes.write[i] = value != INT32_MIN ? value * 0.0078125 : 0;
	}

	_has_effect_send = false;
	for (int i = 1; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
		if (_volumes[i] > 0) {
			_has_effect_send = true;
		}
	}
}

void SiOPMChannelBase::set_stream_buffer(int p_stream_num, SiOPMStream *p_stream) {
	_streams.write[p_stream_num] = p_stream;
}

void SiOPMChannelBase::set_stream_send(int p_stream_num, double p_volume) {
	_volumes.write[p_stream_num] = p_volume;
	if (p_stream_num == 0) {
		return;
	}

	if (p_volume > 0) {
		_has_effect_send = true;
	} else {
		_has_effect_send = false;
		for (int i = 1; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			if (_volumes[i] > 0) {
				_has_effect_send = true;
			}
		}
	}
}

double SiOPMChannelBase::get_stream_send(int p_stream_num) {
	return _volumes[p_stream_num];
}

// LFO control.

void SiOPMChannelBase::initialize_lfo(int p_waveform, Vector<int> p_custom_wave_table) {
	const int table_size = SiOPMRefTable::LFO_TABLE_SIZE;
	if (_lfo_wave_table.size() != table_size) {
		_lfo_wave_table.resize_zeroed(table_size);
	}

	int *dst = _lfo_wave_table.ptrw();

	if (p_waveform == -1 && p_custom_wave_table.size() == table_size) {
		_lfo_wave_shape = -1;
		const int *src = p_custom_wave_table.ptr();
		memcpy(dst, src, sizeof(int) * table_size);
	} else {
		_lfo_wave_shape = (p_waveform >= 0 && p_waveform < SiOPMRefTable::LFO_WAVE_MAX) ? p_waveform : SiOPMRefTable::LFO_WAVE_TRIANGLE;
		const int *src = _table->lfo_wave_tables[_lfo_wave_shape];
		memcpy(dst, src, sizeof(int) * table_size);
	}

	_lfo_timer = 1;
	_lfo_timer_step = 0;
	_lfo_timer_step_buffer = 0;
	_lfo_phase = 0;
}

void SiOPMChannelBase::set_lfo_cycle_time(double p_ms) {
	_lfo_timer = 0;
	// Coefficient = sampling_rate / (1000 * 255)
	double coef = _table->sampling_rate / (1000.0 * 255.0);
	_lfo_timer_step = ((int)(SiOPMRefTable::LFO_TIMER_INITIAL/(p_ms * coef))) << _table->sample_rate_pitch_shift;
	_lfo_timer_step_buffer = _lfo_timer_step;
}

double SiOPMChannelBase::_get_lfo_bpm() const {
	return _sound_chip ? _sound_chip->get_bpm() : 120.0;
}

void SiOPMChannelBase::set_lfo_frequency_step(int p_value) {
	// In synced modes, p_value is beat division; in Rate mode, it's raw timer step.
	if (_lfo_time_mode >= LFO_TIME_MODE_SYNCED) {
		_lfo_beat_division = p_value;
	}
	double bpm = _get_lfo_bpm();
	int timer_step = _calculate_lfo_timer_step(
		_lfo_time_mode,
		p_value,
		bpm,
		_table->sampling_rate,
		_table->sample_rate_pitch_shift
	);
	_lfo_timer = (timer_step > 0 ? 1 : 0);
	_lfo_timer_step = timer_step;
	_lfo_timer_step_buffer = timer_step;
}

void SiOPMChannelBase::set_lfo_time_mode(int p_mode) {
	_lfo_time_mode = p_mode;
	// Recalculate timer step if in a BPM-synced mode.
	if (_lfo_time_mode >= LFO_TIME_MODE_SYNCED) {
		double bpm = _get_lfo_bpm();
		int timer_step = _calculate_lfo_timer_step(
			_lfo_time_mode,
			_lfo_beat_division,
			bpm,
			_table->sampling_rate,
			_table->sample_rate_pitch_shift
		);
		_lfo_timer = (timer_step > 0 ? 1 : 0);
		_lfo_timer_step = timer_step;
		_lfo_timer_step_buffer = timer_step;
	}
}

void SiOPMChannelBase::update_lfo_for_bpm() {
	// Only recalculate if in a BPM-synced mode.
	if (_lfo_time_mode >= LFO_TIME_MODE_SYNCED) {
		double bpm = _get_lfo_bpm();
		int timer_step = _calculate_lfo_timer_step(
			_lfo_time_mode,
			_lfo_beat_division,
			bpm,
			_table->sampling_rate,
			_table->sample_rate_pitch_shift
		);
		_lfo_timer = (timer_step > 0 ? 1 : 0);
		_lfo_timer_step = timer_step;
		_lfo_timer_step_buffer = timer_step;
	}
}

// Filter control.

void SiOPMChannelBase::set_sv_filter(int p_cutoff, int p_resonance, int p_attack_rate, int p_decay_rate1, int p_decay_rate2, int p_release_rate, int p_decay_cutoff1, int p_decay_cutoff2, int p_sustain_cutoff, int p_release_cutoff) {
	_filter_eg_cutoff[EG_ATTACK]  = CLAMP(p_cutoff, 0, 128);
	_filter_eg_cutoff[EG_DECAY1]  = CLAMP(p_decay_cutoff1, 0, 128);
	_filter_eg_cutoff[EG_DECAY2]  = CLAMP(p_decay_cutoff2, 0, 128);
	_filter_eg_cutoff[EG_SUSTAIN] = CLAMP(p_sustain_cutoff, 0, 128);
	_filter_eg_cutoff[EG_RELEASE] = 0;
	_filter_eg_cutoff[EG_OFF]     = CLAMP(p_release_cutoff, 0, 128);

	_filter_eg_time[EG_ATTACK]  = _table->filter_eg_rate[p_attack_rate & 63];
	_filter_eg_time[EG_DECAY1]  = _table->filter_eg_rate[p_decay_rate1 & 63];
	_filter_eg_time[EG_DECAY2]  = _table->filter_eg_rate[p_decay_rate2 & 63];
	_filter_eg_time[EG_SUSTAIN] = INT32_MAX;
	_filter_eg_time[EG_RELEASE] = _table->filter_eg_rate[p_release_rate & 63];
	_filter_eg_time[EG_OFF]     = INT32_MAX;

	_resonance = (1 << (9 - CLAMP(p_resonance, 0, 9))) * 0.001953125; // 0.001953125 = 1/512
	_filter_on = (p_cutoff < 128 || p_resonance > 0 || p_attack_rate > 0 || p_release_rate > 0);
}

void SiOPMChannelBase::offset_filter(int p_offset) {
	_cutoff_offset = p_offset - 128;
}

// Connection control.

void SiOPMChannelBase::set_input(int p_level, int p_pipe_index) {
	if (p_level > 0) {
		_in_pipe = _sound_chip->get_pipe(p_pipe_index & 3, _buffer_index);
		_input_mode = InputMode::INPUT_PIPE;
		_input_level = p_level + 10;
	} else {
		_in_pipe = _sound_chip->get_zero_buffer();
		_input_mode = InputMode::INPUT_ZERO;
		_input_level = 0;
	}
}

void SiOPMChannelBase::set_ring_modulation(int p_level, int p_pipe_index) {
	_ringmod_level = p_level * 4.0 / (1 << SiOPMRefTable::LOG_VOLUME_BITS);
	_ring_pipe = p_level > 0 ? _sound_chip->get_pipe(p_pipe_index & 3, _buffer_index) : nullptr;
}

void SiOPMChannelBase::set_output(OutputMode p_output_mode, int p_pipe_index) {
	int pipe_index = p_pipe_index & 3;
	if (p_output_mode == OutputMode::OUTPUT_STANDARD) {
		pipe_index = 4;   // pipe[4] is used.
	}

	_output_mode = p_output_mode;
	_out_pipe = _sound_chip->get_pipe(pipe_index, _buffer_index);
	_base_pipe = (_output_mode == OutputMode::OUTPUT_ADD ? _out_pipe : _sound_chip->get_zero_buffer());
}

void SiOPMChannelBase::set_volume_tables(int (&p_velocity_table)[SiOPMRefTable::TL_TABLE_SIZE], int (&p_expression_table)[SiOPMRefTable::TL_TABLE_SIZE]) {
	COPY_TL_TABLE(_velocity_table, p_velocity_table);
	COPY_TL_TABLE(_expression_table, p_expression_table);
}

// Processing.

void SiOPMChannelBase::_reset_sv_filter_state() {
	_cutoff_frequency = _filter_eg_cutoff[EG_ATTACK];
}

bool SiOPMChannelBase::_try_shift_sv_filter_state(int p_state) {
	if (_filter_eg_time[p_state] == 0) {
		return false;
	}

	_filter_eg_state = p_state;
	_filter_eg_step  = _filter_eg_time[p_state];
	_filter_eg_next  = _filter_eg_cutoff[p_state + 1];
	_filter_eg_cutoff_inc = (_cutoff_frequency < _filter_eg_next) ? 1 : -1;
	return (_cutoff_frequency != _filter_eg_next);
}

void SiOPMChannelBase::_shift_sv_filter_state(int p_state) {
	int state = p_state;

	switch (state) {
		case EG_ATTACK: {
			if (_try_shift_sv_filter_state(state)) {
				break;
			}
			state++;
		}
			[[fallthrough]];
		case EG_DECAY1: {
			if (_try_shift_sv_filter_state(state)) {
				break;
			}
			state++;
		}
			[[fallthrough]];
		case EG_DECAY2: {
			if (_try_shift_sv_filter_state(state)) {
				break;
			}
			state++;
		}
			[[fallthrough]];
		case EG_SUSTAIN: {
			// Catch all.
			_filter_eg_state = EG_SUSTAIN;
			_filter_eg_step  = INT32_MAX;
			_filter_eg_next  = _cutoff_frequency + 1;
			_filter_eg_cutoff_inc = 0;
		} break;

		case EG_RELEASE: {
			if (_try_shift_sv_filter_state(state)) {
				break;
			}
			state++;
		}
			[[fallthrough]];
		case EG_OFF: {
			// Catch all.
			_filter_eg_state = EG_OFF;
			_filter_eg_step  = INT32_MAX;
			_filter_eg_next  = _cutoff_frequency + 1;
			_filter_eg_cutoff_inc = 0;
		} break;
	}

	_filter_eg_residue = _filter_eg_step;
}

void SiOPMChannelBase::note_on() {
	// If this channel was in the middle of a click-safe hard stop fade (e.g. due to
	// voice stealing), cancel it. Otherwise the fade would attenuate or even reset
	// the channel while the new note is starting, causing missing attacks/dropped notes.
	cancel_kill_fade();

	_lfo_phase = 0; // Reset.
	if (_filter_on) {
		_reset_sv_filter_state();
		_shift_sv_filter_state(EG_ATTACK);
	}
	_is_note_on = true;
}

void SiOPMChannelBase::note_off() {
	if (_filter_on) {
		_shift_sv_filter_state(EG_RELEASE);
	}
	_is_note_on = false;
}

void SiOPMChannelBase::cancel_kill_fade() {
	_kill_fade_total_samples = 0;
	_kill_fade_remaining_samples = 0;
}

void SiOPMChannelBase::_no_process(int p_length) {
	// Rotate the output buffer.
	if (_output_mode == OutputMode::OUTPUT_STANDARD) {
		int pipe_index = (_buffer_index + p_length) & (_sound_chip->get_buffer_length() - 1);
		_out_pipe = _sound_chip->get_pipe(4, pipe_index);
	} else {
		_out_pipe->advance(p_length);
		_base_pipe = (_output_mode == OutputMode::OUTPUT_ADD ? _out_pipe : _sound_chip->get_zero_buffer());
	}

	// Rotate the input buffer when connected by @i.
	if (_input_mode == InputMode::INPUT_PIPE) {
		_in_pipe->advance(p_length);
	}

	// Rotate the ring buffer.
	if (_ring_pipe) {
		_ring_pipe->advance(p_length);
	}

}

void SiOPMChannelBase::reset_channel_buffer_status() {
	// Reset the per-buffer state so the channel can start writing
	// into the output pipes from the beginning of the next audio frame.
	_buffer_index = 0;

	// Reset read/write cursors for all pipes that the channel may use.
	if (_in_pipe) {
		_in_pipe->front();
	}
	if (_ring_pipe) {
		_ring_pipe->front();
	}
	if (_base_pipe) {
		_base_pipe->front();
	}
	if (_out_pipe) {
		_out_pipe->front();
	}
}

void SiOPMChannelBase::_apply_ring_modulation(SinglyLinkedList<int>::Element *p_buffer_start, int p_length) {
	SinglyLinkedList<int>::Element *target = p_buffer_start;

	for (int i = 0; i < p_length; i++) {
		target->value *= _ring_pipe->get()->value * _ringmod_level;
		target = target->next();
		_ring_pipe->next();
	}
}

void SiOPMChannelBase::_apply_sv_filter(SinglyLinkedList<int>::Element *p_buffer_start, int p_length, double (&r_variables)[3]) {
	int cutoff = CLAMP(_cutoff_frequency + _cutoff_offset, 0, 128);
	double cutoff_value = _table->filter_cutoff_table[cutoff];
	double feedback_value = _resonance; // * _table->filter_feedback_table[out]; // This is commented out in original code.

	// Previous setting.
	int step = _filter_eg_residue;

	SinglyLinkedList<int>::Element *target = p_buffer_start;
	int length = p_length;
	while (length >= step) {
		// Process.
		for (int i = 0; i < step; i++) {
			r_variables[2] = (double)target->value - r_variables[0] - r_variables[1] * feedback_value;
			r_variables[1] += r_variables[2] * cutoff_value;
			r_variables[0] += r_variables[1] * cutoff_value;

			target->value = (int)r_variables[_filter_type];
			target = target->next();
		}
		length -= step;

		// Change cutoff and shift state.

		_cutoff_frequency += _filter_eg_cutoff_inc;
		cutoff = CLAMP(_cutoff_frequency + _cutoff_offset, 0, 128);
		cutoff_value = _table->filter_cutoff_table[cutoff];
		feedback_value = _resonance; // * _table->filter_feedback_table[out]; // This is commented out in original code.

		if (_cutoff_frequency == _filter_eg_next) {
			_shift_sv_filter_state(_filter_eg_state + 1);
		}

		step = _filter_eg_step;
	}

	// Process the remainder.
	for (int i = 0; i < length; i++) {
		r_variables[2] = (double)target->value - r_variables[0] - r_variables[1] * feedback_value;
		r_variables[1] += r_variables[2] * cutoff_value;
		r_variables[0] += r_variables[1] * cutoff_value;

		target->value = (int)r_variables[_filter_type];
		target = target->next();
	}

	// Next setting.
	_filter_eg_residue = _filter_eg_step - length;
}

void SiOPMChannelBase::start_kill_fade(int p_samples) {
	// If we can't compute a sensible fade, fall back to immediate reset.
	if (p_samples == 0) {
		reset();
		_kill_fade_total_samples = 0;
		_kill_fade_remaining_samples = 0;
		return;
	}

	int samples = p_samples;
	if (samples < 0) {
		// Default: a very short fade (in samples) to avoid clicks.
		// Use the engine sampling rate when available.
		const int sr = (_table ? _table->sampling_rate : 0);
		// 2ms is typically enough to remove hard discontinuities without sounding like a fade-out.
		samples = (sr > 0) ? (int)(sr * 0.002) : 96;
	}
	// Clamp to a sane range (>=1).
	if (samples < 1) {
		samples = 1;
	}

	_kill_fade_total_samples = samples;
	_kill_fade_remaining_samples = samples;
	// Ensure we process at least until the fade completes.
	_is_idling = false;
}

void SiOPMChannelBase::_apply_kill_fade(SinglyLinkedList<int>::Element *p_buffer_start, int p_length) {
	if (_kill_fade_remaining_samples <= 0 || _kill_fade_total_samples <= 0) {
		return;
	}

	SinglyLinkedList<int>::Element *target = p_buffer_start;
	for (int i = 0; i < p_length; i++) {
		double gain = 0.0;
		if (_kill_fade_remaining_samples > 0) {
			if (_kill_fade_total_samples <= 1) {
				// Single-sample fade: set output to 0 immediately (no time for a ramp).
				gain = 0.0;
			} else {
				// Make the last sample exactly 0: remaining==1 -> gain==0, remaining==total -> gain==1.
				gain = (double)(_kill_fade_remaining_samples - 1) / (double)(_kill_fade_total_samples - 1);
				gain = CLAMP(gain, 0.0, 1.0);
			}
			_kill_fade_remaining_samples -= 1;
		}

		target->value = (int)((double)target->value * gain);
		target = target->next();
	}

	if (_kill_fade_remaining_samples <= 0) {
		_kill_fade_remaining_samples = 0;
		_kill_fade_total_samples = 0;
		// Fade complete: now it's safe to reset DSP state without clicking.
		reset();
	}
}

void SiOPMChannelBase::_apply_kill_fade_stereo(SinglyLinkedList<int>::Element *p_left_start, SinglyLinkedList<int>::Element *p_right_start, int p_length) {
	if (_kill_fade_remaining_samples <= 0 || _kill_fade_total_samples <= 0) {
		return;
	}

	SinglyLinkedList<int>::Element *left = p_left_start;
	SinglyLinkedList<int>::Element *right = p_right_start;
	for (int i = 0; i < p_length; i++) {
		double gain = 0.0;
		if (_kill_fade_remaining_samples > 0) {
			if (_kill_fade_total_samples <= 1) {
				gain = 0.0;
			} else {
				gain = (double)(_kill_fade_remaining_samples - 1) / (double)(_kill_fade_total_samples - 1);
				gain = CLAMP(gain, 0.0, 1.0);
			}
			_kill_fade_remaining_samples -= 1;
		}

		left->value = (int)((double)left->value * gain);
		right->value = (int)((double)right->value * gain);
		left = left->next();
		right = right->next();
	}

	if (_kill_fade_remaining_samples <= 0) {
		_kill_fade_remaining_samples = 0;
		_kill_fade_total_samples = 0;
		reset();
	}
}

void SiOPMChannelBase::buffer(int p_length) {
	if (_is_idling) {
		buffer_no_process(p_length);
		return;
	}

	// Preserve the start of the output pipe.
	SinglyLinkedList<int>::Element *mono_out = _out_pipe->get();

	// Update the output pipe for the provided length.
	// Note: _process_function is always initialized and updated atomically,
	// so no validity check needed. Checking validity from the audio thread
	// can cause crashes due to Callable internal state access during updates.
	_process_function.call(p_length);

	if (_ring_pipe) {
		_apply_ring_modulation(mono_out, p_length);
	}
	if (_filter_on) {
		_apply_sv_filter(mono_out, p_length, _filter_variables);
	}
	if (_kill_fade_remaining_samples > 0) {
		_apply_kill_fade(mono_out, p_length);
	}

	if (_output_mode == OutputMode::OUTPUT_STANDARD && !_mute) {
		// TODO: this seems bad
		// make track faders/pan work after the effect stream by preventing pre‑application in the channel write.
		const bool is_redirected_main_stream = (_streams[0] != nullptr && _streams[0] != _sound_chip->get_output_stream());
		if (_has_effect_send) {
			for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
				if (_volumes[i] > 0) {
					SiOPMStream *stream = _streams[i] ? _streams[i] : _sound_chip->get_stream_slot(i);
					if (stream) {
						const double volume = (i == 0 && is_redirected_main_stream) ? _instrument_gain : (_volumes[i] * _instrument_gain);
						const int pan = (i == 0 && is_redirected_main_stream) ? 64 : _pan;
						stream->write(mono_out, _buffer_index, p_length, volume, pan);
					}
				}
			}
		} else {
			SiOPMStream *stream = _streams[0] ? _streams[0] : _sound_chip->get_output_stream();
			const double volume = is_redirected_main_stream ? _instrument_gain : (_volumes[0] * _instrument_gain);
			const int pan = is_redirected_main_stream ? 64 : _pan;
			stream->write(mono_out, _buffer_index, p_length, volume, pan);
		}
	}

	_buffer_index += p_length;
}

void SiOPMChannelBase::buffer_no_process(int p_length) {
	_no_process(p_length);
	_buffer_index += p_length;
}

//

void SiOPMChannelBase::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	// Volume.

	if (p_prev && p_prev != this) {
		for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			_volumes.write[i] = p_prev->_volumes[i];
			_streams.write[i] = p_prev->_streams[i];
		}

		_instrument_gain = p_prev->_instrument_gain;
		_instrument_gain_db = p_prev->_instrument_gain_db;
		_pan = p_prev->_pan;
		_has_effect_send = p_prev->_has_effect_send;
		_mute = p_prev->_mute;
		COPY_TL_TABLE(_velocity_table, p_prev->_velocity_table);
		COPY_TL_TABLE(_expression_table, p_prev->_expression_table);
	} else if (!p_prev) {
		_volumes.write[0] = 0.5;
		_streams.write[0] = nullptr;
		for (int i = 1; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			_volumes.write[i] = 0;
			_streams.write[i] = nullptr;
		}

		set_instrument_gain_db(kInstrumentGainDbDefault);
		_pan = 64;
		_has_effect_send = false;
		_mute = false;
		COPY_TL_TABLE(_velocity_table, _table->eg_total_level_tables[SiOPMRefTable::VM_LINEAR]);
		COPY_TL_TABLE(_expression_table, _table->eg_total_level_tables[SiOPMRefTable::VM_LINEAR]);
	}

	// Buffer index.
	_is_note_on = false;
	_is_idling = true;
	_buffer_index = p_buffer_index;

	// LFO.
	_lfo_time_mode = LFO_TIME_MODE_RATE;
	_lfo_beat_division = LFO_BEAT_1_4;
	initialize_lfo(SiOPMRefTable::LFO_WAVE_TRIANGLE);
	set_lfo_cycle_time(333);
	set_frequency_ratio(100);

	// Connection.
	set_input(0, 0);
	set_ring_modulation(0, 0);
	set_output(OutputMode::OUTPUT_STANDARD, 0);

	// LP filter.
	_filter_variables[0] = 0;
	_filter_variables[1] = 0;
	_filter_variables[2] = 0;
	_cutoff_offset = 0;
	_filter_type = FILTER_LP;
	set_sv_filter();
	_shift_sv_filter_state(EG_OFF);

}

void SiOPMChannelBase::reset() {
	_is_note_on = false;
	_is_idling = true;
	cancel_kill_fade();
}

String SiOPMChannelBase::_to_string() const {
	String params = "";

	params += "feedback=" + itos(_input_level - 6) + ", ";
	params += "vol=" + rtos(_volumes[0]) + ", ";
	params += "inst_gain_db=" + itos(_instrument_gain_db) + ", ";
	params += "pan=" + itos(_pan - 64) + "";

	return "SiOPMChannelBase: " + params;
}

void SiOPMChannelBase::_bind_methods() {
	// To be used as callables.
	ClassDB::bind_method(D_METHOD("_no_process", "length"), &SiOPMChannelBase::_no_process);
	ClassDB::bind_method(D_METHOD("set_sv_filter", "cutoff", "resonance", "attack_rate", "decay_rate1", "decay_rate2", "release_rate", "decay_cutoff1", "decay_cutoff2", "sustain_cutoff", "release_cutoff"), &SiOPMChannelBase::set_sv_filter, DEFVAL(128), DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(128), DEFVAL(128), DEFVAL(128), DEFVAL(128));
	ClassDB::bind_method(D_METHOD("activate_filter", "active"), &SiOPMChannelBase::activate_filter);
	ClassDB::bind_method(D_METHOD("offset_filter", "offset"), &SiOPMChannelBase::offset_filter);
	ClassDB::bind_method(D_METHOD("set_filter_type", "type"), &SiOPMChannelBase::set_filter_type);
	ClassDB::bind_method(D_METHOD("is_filter_active"), &SiOPMChannelBase::is_filter_active);
    ClassDB::bind_method(D_METHOD("set_filter_cutoff_now", "cutoff"), &SiOPMChannelBase::set_filter_cutoff_now);
    ClassDB::bind_method(D_METHOD("set_filter_resonance_now", "resonance"), &SiOPMChannelBase::set_filter_resonance_now);
	// Volume getters/setters
	ClassDB::bind_method(D_METHOD("get_master_volume"), &SiOPMChannelBase::get_master_volume);
	ClassDB::bind_method(D_METHOD("set_master_volume", "value"), &SiOPMChannelBase::set_master_volume);
}

SiOPMChannelBase::SiOPMChannelBase(SiOPMSoundChip *p_chip) {
	_table = SiOPMRefTable::get_instance();
	_sound_chip = p_chip;
	_process_function = Callable(this, "_no_process");

	_streams.clear();
	_streams.resize_zeroed(SiOPMSoundChip::STREAM_SEND_SIZE);
	_volumes.clear();
	_volumes.resize_zeroed(SiOPMSoundChip::STREAM_SEND_SIZE);
	set_instrument_gain_db(kInstrumentGainDbDefault);
}

SiOPMChannelBase::~SiOPMChannelBase() {
}

#undef COPY_TL_TABLE
