/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_channel_sampler.h"

#include "sion_enums.h"
#include "chip/siopm_channel_params.h"
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"
#include "chip/wave/siopm_wave_base.h"
#include "chip/wave/siopm_wave_sampler_data.h"
#include "chip/wave/siopm_wave_sampler_table.h"
#include <cmath>

void SiOPMChannelSampler::get_channel_params(const Ref<SiOPMChannelParams> &p_params) const {
	for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
		p_params->set_master_volume(i, _volumes[i]);
	}
	p_params->set_pan(_pan);
}

void SiOPMChannelSampler::set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation) {
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
}

void SiOPMChannelSampler::set_wave_data(const Ref<SiOPMWaveBase> &p_wave_data) {
	_sampler_table = p_wave_data;
	_sample_data = p_wave_data;
}

void SiOPMChannelSampler::set_types(int p_pg_type, SiONPitchTableType p_pt_type) {
	_bank_number = p_pg_type & 3;
}

int SiOPMChannelSampler::get_pitch() const {
	return (_wave_number << 6) + _fine_pitch;
}

void SiOPMChannelSampler::set_pitch(int p_value) {
	_wave_number = p_value >> 6;
	_fine_pitch = p_value & 0x3F; // lower 6 bits

	// Calculate pitch ratio relative to middle C (note 60).
	int note_number = _wave_number;
	double delta_semitones = (double)(note_number - 60) + (_fine_pitch / 64.0); // 64 fine steps per semitone
	_pitch_step = std::pow(2.0, delta_semitones / 12.0);
}

void SiOPMChannelSampler::set_phase(int p_value) {
	_sample_start_phase = p_value;
}

// Volume control.

void SiOPMChannelSampler::offset_volume(int p_expression, int p_velocity) {
	_expression = p_expression * p_velocity * 0.00006103515625; // 1/16384
}

// Processing.

void SiOPMChannelSampler::note_on() {
	if (_wave_number < 0) {
		return;
	}

	if (_sampler_table.is_valid()) {
		_sample_data = _sampler_table->get_sample(_wave_number & 127);
	}
	if (_sample_data.is_valid() && _sample_start_phase != 255) {
		_sample_index = _sample_data->get_initial_sample_index(_sample_start_phase * 0.00390625); // 1/256
		_sample_index_fp = (double)_sample_index;
		_sample_pan = CLAMP(_pan + _sample_data->get_pan(), 0, 128);
		// If the sample is marked as fixed_pitch (slice mode), disable pitch shifting.
		if (_sample_data->is_fixed_pitch()) {
			// Apply global pitch offset (in semitones) instead of note-based transposition.
			double offset_semitones = _sample_data->get_pitch_offset();
			_pitch_step = std::pow(2.0, offset_semitones / 12.0);
		}
	}

	_is_idling = (_sample_data == nullptr);
	_is_note_on = !_is_idling;

	// Reset envelope.
	_is_releasing = false;
	_envelope_level = 1.0;
}

void SiOPMChannelSampler::note_off() {
	if (_sample_data.is_null() || _sample_data->is_ignoring_note_off()) {
		return;
	}

	_is_note_on = false;

	// Start release phase instead of immediate stop to avoid clicks.
	_is_releasing = true;
	_release_samples_left = RELEASE_SAMPLES;
}

void SiOPMChannelSampler::buffer(int p_length) {
	if (_is_idling || _sample_data == nullptr || _sample_data->get_length() <= 0 || _mute) {
		buffer_no_process(p_length);
		return;
	}

	Vector<double> wave_data = _sample_data->get_wave_data();
	int channels = _sample_data->get_channel_count();
	int end_point = _sample_data->get_end_point();
	int loop_point = _sample_data->get_loop_point();

	// Temporary buffer that will hold resampled data for this block.
	Vector<double> temp_buffer;
	temp_buffer.resize_zeroed(p_length * channels);

	int samples_written = 0;
	while (samples_written < p_length) {
		if (_sample_index_fp >= end_point) {
			if (loop_point >= 0) {
				// Loop back.
				_sample_index_fp = loop_point + (_sample_index_fp - end_point);
			} else {
				// End reached, finish playing.
				_is_idling = true;
				break;
			}
		}

		int remaining = p_length - samples_written;

		for (int i = 0; i < remaining; i++) {
			int base_index = (int)_sample_index_fp;
			double frac = _sample_index_fp - base_index;

			if (base_index >= end_point) {
				// This would be handled in outer loop.
				break;
			}

			for (int ch = 0; ch < channels; ch++) {
				double s1 = wave_data[(base_index * channels) + ch];
				double s2;
				if (base_index + 1 < end_point) {
					s2 = wave_data[((base_index + 1) * channels) + ch];
				} else {
					s2 = s1;
				}

				double sample_value = s1 + (s2 - s1) * frac;
				sample_value *= _envelope_level;

				temp_buffer.write[(samples_written * channels) + ch] = sample_value;
			}

			// Advance sample position.
			_sample_index_fp += _pitch_step;

			// Update envelope during release.
			if (_is_releasing) {
				if (_release_samples_left > 0) {
					_release_samples_left--;
					_envelope_level = (double)_release_samples_left / RELEASE_SAMPLES;
				} else {
					_envelope_level = 0.0;
					_is_releasing = false;
					_is_idling = true;
					// We can break early since envelope is silent.
					break;
				}
			}

			samples_written++;
			if (samples_written >= p_length) {
				break;
			}
		}
	}

	if (samples_written > 0) {
		// Write to stream(s).
		if (_has_effect_send) {
			for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
				if (_volumes[i] > 0) {
					SiOPMStream *stream = _streams[i] ? _streams[i] : _sound_chip->get_stream_slot(i);
					if (stream) {
						double volume = _volumes[i] * _expression * _sound_chip->get_sampler_volume();
						stream->write_from_vector(&temp_buffer, 0, _buffer_index, samples_written, volume, _sample_pan, channels);
					}
				}
			}
		} else {
			SiOPMStream *stream = _streams[0] ? _streams[0] : _sound_chip->get_output_stream();
			double volume = _volumes[0] * _expression * _sound_chip->get_sampler_volume();
			stream->write_from_vector(&temp_buffer, 0, _buffer_index, samples_written, volume, _sample_pan, channels);
		}
	}

	_buffer_index += p_length;
}

void SiOPMChannelSampler::buffer_no_process(int p_length) {
	_buffer_index += p_length;
}

//

void SiOPMChannelSampler::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	SiOPMChannelBase::initialize(p_prev, p_buffer_index);
	reset();
}

void SiOPMChannelSampler::reset() {
	_is_note_on = false;
	_is_idling = true;

	_bank_number = 0;
	_wave_number = -1;
	_expression = 1;

	_sampler_table = _table->sampler_tables[0];
	_sample_data = Ref<SiOPMWaveSamplerData>();

	_sample_start_phase = 0;
	_sample_index = 0;
	_sample_pan = 0;

	_fine_pitch = 0;
	_pitch_step = 1.0;
	_sample_index_fp = 0.0;
	_is_releasing = false;
	_release_samples_left = 0;
	_envelope_level = 1.0;
}

String SiOPMChannelSampler::_to_string() const {
	String params = "";

	params += "vol=" + rtos(_volumes[0] * _expression) + ", ";
	params += "pan=" + itos(_pan - 64) + "";

	return "SiOPMChannelSampler: " + params;
}

SiOPMChannelSampler::SiOPMChannelSampler(SiOPMSoundChip *p_chip) : SiOPMChannelBase(p_chip) {
	// Empty.
}
