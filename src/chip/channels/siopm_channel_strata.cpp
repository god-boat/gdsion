#include "siopm_channel_strata.h"

#include <cmath>
#include <cstring>
#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"

void SiOPMChannelStrata::_recompute_pitch_correction(double p_sample_rate) {
	if (p_sample_rate <= 0) {
		_pitch_correction = 0;
		return;
	}
	// Braids assumes 96 kHz internally. To make a given MIDI note number sound
	// the same at our host rate, raise the pitch fed to Braids by
	//   log2(REFERENCE_SAMPLE_RATE / host_sr) * 12 semitones * 128 units/semi.
	double semitones = std::log2(REFERENCE_SAMPLE_RATE / p_sample_rate) * 12.0;
	_pitch_correction = (int)std::round(semitones * 128.0);
}

void SiOPMChannelStrata::_set_strata_pitch() {
	// SiON pitch is 64 units / semitone -> Braids is 128 units / semitone.
	int braids_pitch = (_current_pitch << 1) + _pitch_correction;
	// Clamp to a safe range under both analog (kHighestNote=128*128) and
	// digital (kHighestNote=140*128) ceilings.
	if (braids_pitch < 0) {
		braids_pitch = 0;
	} else if (braids_pitch > 16383) {
		braids_pitch = 16383;
	}
	_osc.set_pitch((int16_t)braids_pitch);
}

void SiOPMChannelStrata::set_strata_params(int p_shape, int p_timbre, int p_color) {
	int s = p_shape;
	if (s < 0) {
		s = 0;
	} else if (s >= (int)braids::MACRO_OSC_SHAPE_LAST) {
		s = (int)braids::MACRO_OSC_SHAPE_LAST - 1;
	}
	_shape = s;
	_timbre = CLAMP(p_timbre, 0, 32767);
	_color = CLAMP(p_color, 0, 32767);

	_osc.set_shape((braids::MacroOscillatorShape)_shape);
	_osc.set_parameters((int16_t)_timbre, (int16_t)_color);
}

void SiOPMChannelStrata::offset_volume(int p_expression, int p_velocity) {
	_expression = (double)p_expression * 0.0078125;
}

void SiOPMChannelStrata::note_on() {
	_is_note_on = true;
	_is_idling = false;
	_osc.Strike();
}

void SiOPMChannelStrata::note_off() {
	_is_note_on = false;
}

void SiOPMChannelStrata::reset_channel_buffer_status() {
	_buffer_index = 0;
	// Most Braids shapes are self-sustaining, so we don't try to detect idle
	// energy here. Caller-side gating (via the SiON amp envelope or expression)
	// is responsible for silencing the channel when the note is released.
	_is_idling = !_is_note_on;
}

void SiOPMChannelStrata::buffer(int p_length) {
	if (_is_idling) {
		buffer_no_process(p_length);
		return;
	}
	if (p_length <= 0 || p_length > 2048) {
		buffer_no_process(p_length);
		return;
	}

	memset(_scratch_left, 0, sizeof(double) * p_length);
	memset(_scratch_right, 0, sizeof(double) * p_length);

	_set_strata_pitch();
	_osc.set_shape((braids::MacroOscillatorShape)_shape);
	_osc.set_parameters((int16_t)_timbre, (int16_t)_color);

	const double inv_int16 = 1.0 / 32768.0;

	int written = 0;
	while (written < p_length) {
		int chunk = p_length - written;
		if (chunk > BRAIDS_BLOCK_SIZE) {
			chunk = BRAIDS_BLOCK_SIZE;
		}
		// Braids drum render functions decrement size by 2 per iteration; an odd
		// size causes unsigned underflow in size_t, producing a runaway write.
		// Round up to the next even number for Render, but only read `chunk`
		// samples from the output.
		int render_size = (chunk + 1) & ~1;
		memset(_sync_buffer, 0, sizeof(_sync_buffer));
		_osc.Render(_sync_buffer, _render_buffer, (size_t)render_size);

		for (int i = 0; i < chunk; i++) {
			double s = (double)_render_buffer[i] * inv_int16;
			_scratch_left[written + i] = s;
			_scratch_right[written + i] = s;
		}
		written += chunk;
	}

	if (_output_mode == OutputMode::OUTPUT_STANDARD && !_mute) {
		SiOPMStream *stream = _streams[0] ? _streams[0] : _sound_chip->get_output_stream();
		if (stream) {
			const double volume_coef = _expression * _instrument_gain;
			const bool is_redirected = (_streams[0] != nullptr && _streams[0] != _sound_chip->get_output_stream());

			Vector<double> *buf_ptr = stream->get_buffer_ptr();
			double *buf = buf_ptr->ptrw();
			int buf_size = buf_ptr->size();

			double vol = is_redirected ? volume_coef : (_volumes[0] * volume_coef);
			int offset = _buffer_index * 2;

			for (int i = 0; i < p_length; i++) {
				int idx = offset + i * 2;
				if (idx + 1 < buf_size) {
					buf[idx] += _scratch_left[i] * vol;
					buf[idx + 1] += _scratch_right[i] * vol;
				}
			}

			if (_has_effect_send) {
				for (int send = 1; send < SiOPMSoundChip::STREAM_SEND_SIZE; send++) {
					if (_volumes[send] > 0) {
						SiOPMStream *send_stream = _streams[send] ? _streams[send] : _sound_chip->get_stream_slot(send);
						if (send_stream) {
							double send_vol = _volumes[send] * volume_coef;
							Vector<double> *send_buf_ptr = send_stream->get_buffer_ptr();
							double *send_buf = send_buf_ptr->ptrw();
							int send_buf_size = send_buf_ptr->size();
							int send_offset = _buffer_index * 2;
							for (int i = 0; i < p_length; i++) {
								int sidx = send_offset + i * 2;
								if (sidx + 1 < send_buf_size) {
									send_buf[sidx] += _scratch_left[i] * send_vol;
									send_buf[sidx + 1] += _scratch_right[i] * send_vol;
								}
							}
						}
					}
				}
			}
		}
	}

	_buffer_index += p_length;
}

void SiOPMChannelStrata::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	SiOPMChannelBase::initialize(p_prev, p_buffer_index);

	double sample_rate = _table ? (double)_table->sampling_rate : REFERENCE_SAMPLE_RATE;
	_recompute_pitch_correction(sample_rate);

	_shape = (int)braids::MACRO_OSC_SHAPE_CSAW;
	_timbre = 0;
	_color = 0;
	_current_pitch = 0;
	_expression = 1.0;

	_osc.Init();
	_osc.set_shape((braids::MacroOscillatorShape)_shape);
	_osc.set_parameters((int16_t)_timbre, (int16_t)_color);

	_is_idling = true;
	_is_note_on = false;
}

void SiOPMChannelStrata::reset() {
	_osc.Init();
	_osc.set_shape((braids::MacroOscillatorShape)_shape);
	_osc.set_parameters((int16_t)_timbre, (int16_t)_color);

	SiOPMChannelBase::reset();
}

String SiOPMChannelStrata::_to_string() const {
	String params;
	params += "shape=" + itos(_shape) + ", ";
	params += "timbre=" + itos(_timbre) + ", ";
	params += "color=" + itos(_color) + ", ";
	params += "vol=" + rtos(_volumes[0]) + ", ";
	params += "pan=" + itos(_pan - 64);

	return "SiOPMChannelStrata: " + params;
}

SiOPMChannelStrata::SiOPMChannelStrata(SiOPMSoundChip *p_chip) : SiOPMChannelBase(p_chip) {
	_osc.Init();
}
