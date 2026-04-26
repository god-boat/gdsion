#include "siopm_channel_guitar6.h"

#include <cmath>
#include <cstring>
#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"

// --- NoiseGen is fully inline in the header ---

// --- Resonator ---

void SiOPMChannelGuitar6::Resonator::init(double p_freq, double p_q, double p_gain, double p_sample_rate) {
	double w0 = 2.0 * Math_PI * p_freq / p_sample_rate;
	double cos_w0 = cos(w0);
	double sin_w0 = sin(w0);
	double alpha = sin_w0 / (2.0 * p_q);

	double b0_raw = alpha;
	double a0 = 1.0 + alpha;
	double a1_raw = -2.0 * cos_w0;
	double a2_raw = 1.0 - alpha;

	b0 = b0_raw / a0;
	a1 = a1_raw / a0;
	a2 = a2_raw / a0;
	gain = p_gain;

	x1l = x2l = y1l = y2l = 0;
	x1r = x2r = y1r = y2r = 0;
}

void SiOPMChannelGuitar6::Resonator::process(double *p_left, double *p_right, int p_num_samples) {
	for (int i = 0; i < p_num_samples; i++) {
		double xl = p_left[i];
		double yl = b0 * xl - b0 * x2l - a1 * y1l - a2 * y2l;
		x2l = x1l;
		x1l = xl;
		y2l = y1l;
		y1l = yl;
		p_left[i] += yl * gain;

		double xr = p_right[i];
		double yr = b0 * xr - b0 * x2r - a1 * y1r - a2 * y2r;
		x2r = x1r;
		x1r = xr;
		y2r = y1r;
		y1r = yr;
		p_right[i] += yr * gain;
	}
}

// --- GuitarString ---

void SiOPMChannelGuitar6::GuitarString::init(int p_index, double p_sample_rate) {
	semitone = STRING_SEMITONES[p_index];
	pre_pan = (p_index - 2.5) * 0.4;

	double target_bandwidth = REFERENCE_SAMPLE_RATE / 2.0;
	noise_lp_coef = MIN(1.0, 2.0 * sin(Math_PI * target_bandwidth / p_sample_rate));

	double ratio = p_sample_rate / REFERENCE_SAMPLE_RATE;
	noise_gain_compensation = ratio > 1.0 ? pow(ratio, 0.25) : 1.0;

	char_noise_step = REFERENCE_SAMPLE_RATE / p_sample_rate;

	string_noise_gen.seed = 4095;
	damp_noise_gen.seed = 65535;

	reset_state();
}

void SiOPMChannelGuitar6::GuitarString::add_pluck(int p_tab_index, double p_velocity, int p_target_sample) {
	if (pending_pluck_count >= MAX_PENDING_PLUCKS) {
		return;
	}

	PluckEvent evt;
	evt.tab_index = p_tab_index;
	evt.velocity = p_velocity;
	evt.target_sample = p_target_sample;

	int pos = pending_pluck_count;
	while (pos > 0 && pending_plucks[pos - 1].target_sample > p_target_sample) {
		pending_plucks[pos] = pending_plucks[pos - 1];
		pos--;
	}
	pending_plucks[pos] = evt;
	pending_pluck_count++;
}

void SiOPMChannelGuitar6::GuitarString::process(double *p_left, double *p_right, int p_block_start, int p_num_samples,
		const double *p_char_noise, int p_char_noise_len,
		double p_character_variation, double p_string_damp, double p_string_damp_variation,
		double p_plug_damp_param, double p_plug_damp_variation, double p_string_tension,
		double p_stereo_spread, double p_sample_rate) {
	int sample_index = 0;

	while (sample_index < p_num_samples) {
		while (pending_pluck_count > 0) {
			PluckEvent &pluck = pending_plucks[0];
			int pluck_offset = pluck.target_sample - p_block_start;

			if (pluck_offset <= sample_index) {
				execute_pluck(pluck, p_char_noise, p_char_noise_len,
						p_character_variation, p_string_damp, p_string_damp_variation,
						p_plug_damp_param, p_plug_damp_variation, p_string_tension,
						p_stereo_spread, p_sample_rate);
				for (int j = 0; j < pending_pluck_count - 1; j++) {
					pending_plucks[j] = pending_plucks[j + 1];
				}
				pending_pluck_count--;
			} else if (pluck_offset < p_num_samples) {
				int to_process = pluck_offset - sample_index;
				process_add(p_left, p_right, sample_index, to_process, p_char_noise, p_char_noise_len);
				sample_index += to_process;
			} else {
				break;
			}
		}

		if (sample_index < p_num_samples) {
			process_add(p_left, p_right, sample_index, p_num_samples - sample_index, p_char_noise, p_char_noise_len);
			sample_index = p_num_samples;
		}
	}
}

void SiOPMChannelGuitar6::GuitarString::execute_pluck(const PluckEvent &p_pluck,
		const double *p_char_noise, int p_char_noise_len,
		double p_character_variation, double p_string_damp, double p_string_damp_variation,
		double p_plug_damp_param, double p_plug_damp_variation, double p_string_tension,
		double p_stereo_spread, double p_sample_rate) {
	int note = semitone + p_pluck.tab_index;
	double freq = 27.5 * pow(2.0, (double)note / 12.0);
	period_n = (int)ceil(p_sample_rate / freq);
	if (period_n > MAX_DELAY_SAMPLES) {
		period_n = MAX_DELAY_SAMPLES;
	}
	if (period_n < 2) {
		period_n = 2;
	}

	feed_noise = true;
	velocity = p_pluck.velocity * 0.25;
	write_index = -1;
	char_noise_pos = 0;
	noise_lp0 = noise_lp1 = 0;

	double normalized_tone = (double)(note - 19) / 44.0;
	if (normalized_tone < 0.0) {
		normalized_tone = 0.0;
	}
	if (normalized_tone > 1.0) {
		normalized_tone = 1.0;
	}

	double sr_scale = REFERENCE_SAMPLE_RATE / p_sample_rate;
	double adjusted_scale = sr_scale + (1.0 - sr_scale) * 0.1;

	double base_dc = p_string_damp + pow(normalized_tone, 0.5) * (1.0 - p_string_damp) * 0.5 +
			(1.0 - p_string_damp) * damp_noise_gen.norm() * p_string_damp_variation;
	dc = 1.0 - pow(1.0 - base_dc, adjusted_scale);

	double min_damp = 0.1;
	double max_damp = 0.9;
	double v0 = p_plug_damp_param - (p_plug_damp_param - min_damp) * p_plug_damp_variation;
	double v1 = p_plug_damp_param + (max_damp - p_plug_damp_param) * p_plug_damp_variation;
	double base_plug_damp = v0 + damp_noise_gen.norm() * (v1 - v0);
	plug_damp = 1.0 - pow(1.0 - base_plug_damp, adjusted_scale);

	char_variation = p_character_variation;
	read_offset = p_string_tension * (period_n - 1);

	double pan = p_stereo_spread * pre_pan;
	gain_l = (1.0 - pan) * 0.5;
	gain_r = (pan + 1.0) * 0.5;
}

void SiOPMChannelGuitar6::GuitarString::process_add(double *p_left, double *p_right, int p_start, int p_count,
		const double *p_char_noise, int p_char_noise_len) {
	if (period_n == -1) {
		return;
	}

	for (int i = 0; i < p_count; i++) {
		write_index++;
		if (write_index >= period_n) {
			write_index = 0;
			feed_noise = false;
		}

		if (feed_noise) {
			int char_idx = (int)floor(char_noise_pos);
			double char_frac = char_noise_pos - (double)char_idx;
			int idx0 = char_idx % p_char_noise_len;
			int idx1 = (char_idx + 1) % p_char_noise_len;
			double char_sample = p_char_noise[idx0] + (p_char_noise[idx1] - p_char_noise[idx0]) * char_frac;
			char_noise_pos += char_noise_step;

			double raw_noise = (char_sample * (1.0 - char_variation) +
					string_noise_gen.bipolar() * char_variation) * velocity * noise_gain_compensation;

			noise_lp0 += (raw_noise - noise_lp0) * noise_lp_coef;
			noise_lp1 += (noise_lp0 - noise_lp1) * noise_lp_coef;
			af += (noise_lp1 - af) * plug_damp;
		} else {
			double r_num = write_index + read_offset;
			if (r_num < 0) {
				r_num += period_n;
			}
			int r_int0 = (int)floor(r_num);
			int r_int1 = r_int0 + 1;
			double r_alp = r_num - (double)r_int0;

			if (r_int0 >= period_n) {
				r_int0 -= period_n;
			}
			if (r_int1 >= period_n) {
				r_int1 -= period_n;
			}

			af = delay[r_int0] * (1.0 - r_alp) + delay[r_int1] * r_alp;
		}

		dr = 0;
		dr += (af - df) * dc;
		df += dr;
		delay[write_index] = df;

		int idx = p_start + i;
		p_left[idx] += df * gain_l;
		p_right[idx] += df * gain_r;
	}
}

void SiOPMChannelGuitar6::GuitarString::reset_state() {
	memset(delay, 0, sizeof(delay));
	write_index = -1;
	period_n = -1;
	af = df = dr = 0;
	noise_lp0 = noise_lp1 = 0;
	char_noise_pos = 0;
	pending_pluck_count = 0;
}

// --- SiOPMChannelGuitar6 ---

void SiOPMChannelGuitar6::_randomize_character_noise(uint32_t p_seed) {
	_char_noise_gen.seed = p_seed;
	for (int i = 0; i < CHAR_NOISE_LENGTH; i++) {
		_char_noise[i] = _char_noise_gen.bipolar();
	}
}

void SiOPMChannelGuitar6::_init_body_resonators(double p_sample_rate) {
	_body_resonators[0].init(98.0, 8.0, 4.0, p_sample_rate);
	_body_resonators[1].init(204.0, 10.0, 3.0, p_sample_rate);
	_body_resonators[2].init(290.0, 8.0, 2.0, p_sample_rate);
}

void SiOPMChannelGuitar6::set_guitar6_params(double p_character_seed, double p_character_variation,
		double p_string_damp, double p_string_damp_variation,
		double p_plug_damp, double p_plug_damp_variation,
		double p_string_tension, double p_stereo_spread, bool p_body_bypass) {
	if (p_character_seed != _character_seed) {
		_character_seed = p_character_seed;
		_randomize_character_noise((uint32_t)p_character_seed);
	}
	_character_variation = CLAMP(p_character_variation, 0.0, 1.0);
	_string_damp = CLAMP(p_string_damp, 0.0, 1.0);
	_string_damp_variation = CLAMP(p_string_damp_variation, 0.0, 1.0);
	_plug_damp = CLAMP(p_plug_damp, 0.0, 1.0);
	_plug_damp_variation = CLAMP(p_plug_damp_variation, 0.0, 1.0);
	_string_tension = CLAMP(p_string_tension, 0.0, 1.0);
	_stereo_spread = CLAMP(p_stereo_spread, 0.0, 1.0);
	_body_bypass = p_body_bypass;
}

void SiOPMChannelGuitar6::pluck_string(int p_string_index, int p_tab_index, double p_velocity, int p_target_sample) {
	if (p_string_index < 0 || p_string_index >= NUM_STRINGS) {
		return;
	}
	_strings[p_string_index].add_pluck(p_tab_index, p_velocity, p_target_sample);
	_is_idling = false;
}

void SiOPMChannelGuitar6::offset_volume(int p_expression, int p_velocity) {
	_expression = (double)p_expression * 0.0078125;
}

void SiOPMChannelGuitar6::note_on() {
	_is_note_on = true;
	_is_idling = false;

	int tab_index = 0;
	double vel = 1.0;

	int midi_note = _current_pitch >> 6;
	int note = midi_note - 21; // Convert MIDI note number to semitone offset from A0 (27.5 Hz).
	if (midi_note >= 0 && midi_note < 128) {
		int base_semitone = STRING_SEMITONES[0];
		tab_index = note - base_semitone;
		if (tab_index < 0) {
			tab_index = 0;
		}
	}

	int best_string = 0;
	int best_fret = tab_index;
	for (int s = NUM_STRINGS - 1; s >= 0; s--) {
		int fret = note - STRING_SEMITONES[s];
		if (fret >= 0 && fret <= 24) {
			best_string = s;
			best_fret = fret;
			break;
		}
	}

	pluck_string(best_string, best_fret, vel, _buffer_index);
}

void SiOPMChannelGuitar6::note_off() {
	_is_note_on = false;
}

void SiOPMChannelGuitar6::reset_channel_buffer_status() {
	_buffer_index = 0;
	_is_idling = true;

	for (int s = 0; s < NUM_STRINGS; s++) {
		if (_strings[s].period_n != -1) {
			bool has_energy = false;
			if (fabs(_strings[s].df) > 0.0001) {
				has_energy = true;
			}
			if (!has_energy) {
				for (int j = 0; j < _strings[s].period_n && !has_energy; j++) {
					if (fabs(_strings[s].delay[j]) > 0.0001) {
						has_energy = true;
					}
				}
			}
			if (has_energy) {
				_is_idling = false;
				return;
			}
		}
		if (_strings[s].pending_pluck_count > 0) {
			_is_idling = false;
			return;
		}
	}
}

void SiOPMChannelGuitar6::_no_process_guitar(int p_length) {
	// Advance buffer index without processing.
}

void SiOPMChannelGuitar6::buffer(int p_length) {
	if (_is_idling) {
		buffer_no_process(p_length);
		return;
	}

	if (p_length <= 0 || p_length > 2048) {
		buffer_no_process(p_length);
		return;
	}

	double sample_rate = _table ? (double)_table->sampling_rate : REFERENCE_SAMPLE_RATE;

	memset(_scratch_left, 0, sizeof(double) * p_length);
	memset(_scratch_right, 0, sizeof(double) * p_length);

	for (int s = 0; s < NUM_STRINGS; s++) {
		_strings[s].process(_scratch_left, _scratch_right, _buffer_index, p_length,
				_char_noise, CHAR_NOISE_LENGTH,
				_character_variation, _string_damp, _string_damp_variation,
				_plug_damp, _plug_damp_variation, _string_tension,
				_stereo_spread, sample_rate);
	}

	if (!_body_bypass) {
		for (int r = 0; r < 3; r++) {
			_body_resonators[r].process(_scratch_left, _scratch_right, p_length);
		}
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

void SiOPMChannelGuitar6::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	SiOPMChannelBase::initialize(p_prev, p_buffer_index);

	double sample_rate = _table ? (double)_table->sampling_rate : REFERENCE_SAMPLE_RATE;

	_character_seed = 65535.0;
	_character_variation = 0.5;
	_string_damp = 0.5;
	_string_damp_variation = 0.25;
	_plug_damp = 0.5;
	_plug_damp_variation = 0.25;
	_string_tension = 0.0;
	_stereo_spread = 0.2;
	_body_bypass = false;
	_expression = 1.0;
	_current_pitch = 0;

	_randomize_character_noise(65535);
	_init_body_resonators(sample_rate);

	for (int s = 0; s < NUM_STRINGS; s++) {
		_strings[s].init(s, sample_rate);
	}

	_is_idling = true;
}

void SiOPMChannelGuitar6::reset() {
	for (int s = 0; s < NUM_STRINGS; s++) {
		_strings[s].reset_state();
	}

	SiOPMChannelBase::reset();
}

String SiOPMChannelGuitar6::_to_string() const {
	String params;
	params += "char_seed=" + rtos(_character_seed) + ", ";
	params += "damp=" + rtos(_string_damp) + ", ";
	params += "tension=" + rtos(_string_tension) + ", ";
	params += "vol=" + rtos(_volumes[0]) + ", ";
	params += "pan=" + itos(_pan - 64);

	return "SiOPMChannelGuitar6: " + params;
}

SiOPMChannelGuitar6::SiOPMChannelGuitar6(SiOPMSoundChip *p_chip) : SiOPMChannelBase(p_chip) {
	_randomize_character_noise(65535);
}
