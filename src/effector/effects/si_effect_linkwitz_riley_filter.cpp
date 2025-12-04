/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "si_effect_linkwitz_riley_filter.h"

const double SiEffectLinkwitzRileyFilter::PI = 3.14159265358979323846;
const double SiEffectLinkwitzRileyFilter::SQRT2 = 1.41421356237309504880;

void SiEffectLinkwitzRileyFilter::set_params(double p_cutoff_frequency, int p_output_mode) {
	_cutoff = CLAMP(p_cutoff_frequency, 20.0, 20000.0);
	_output_mode = (p_output_mode == 0 || p_output_mode == 1) ? p_output_mode : 0;
	_compute_coefficients(_cutoff);
}

void SiEffectLinkwitzRileyFilter::set_cutoff_frequency(double p_value) {
	_cutoff = CLAMP(p_value, 20.0, 20000.0);
	_compute_coefficients(_cutoff);
}

void SiEffectLinkwitzRileyFilter::set_output_mode(int p_value) {
	_output_mode = (p_value == 0 || p_value == 1) ? p_value : 0;
}

void SiEffectLinkwitzRileyFilter::_compute_coefficients(double p_frequency) {
	double warp = 1.0 / Math::tan(PI * p_frequency / SAMPLE_RATE);
	double warp2 = warp * warp;
	double mult = 1.0 / (1.0 + SQRT2 * warp + warp2);

	_low_in_0 = mult;
	_low_in_1 = 2.0 * mult;
	_low_in_2 = mult;
	_low_out_1 = -2.0 * (1.0 - warp2) * mult;
	_low_out_2 = -(1.0 - SQRT2 * warp + warp2) * mult;

	_high_in_0 = warp2 * mult;
	_high_in_1 = -2.0 * _high_in_0;
	_high_in_2 = _high_in_0;
	_high_out_1 = _low_out_1;
	_high_out_2 = _low_out_2;
}

int SiEffectLinkwitzRileyFilter::prepare_process() {
	return 2;
}

int SiEffectLinkwitzRileyFilter::process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	int start_index = p_start_index << 1;
	int length = p_length << 1;

	_compute_coefficients(_cutoff);

	if (p_channels == 1) {
		// Mono processing - same signal in both channels
		for (int i = start_index; i < (start_index + length); i += 2) {
			double audio = (*r_buffer)[i];

			// Process low band - first stage
			double low_in01 = audio * _low_in_0 + _past_in_1a_low_left * _low_in_1;
			double low_in = low_in01 + _past_in_2a_low_left * _low_in_2;
			double low_in_out1 = low_in + _past_out_1a_low_left * _low_out_1;
			double low = low_in_out1 + _past_out_2a_low_left * _low_out_2;

			_past_in_2a_low_left = _past_in_1a_low_left;
			_past_in_1a_low_left = audio;
			_past_out_2a_low_left = _past_out_1a_low_left;
			_past_out_1a_low_left = low;

			// Process low band - second stage
			double low_in01_b = low * _low_in_0 + _past_in_1b_low_left * _low_in_1;
			double low_in_b = low_in01_b + _past_in_2b_low_left * _low_in_2;
			double low_in_out1_b = low_in_b + _past_out_1b_low_left * _low_out_1;
			double low_final = low_in_out1_b + _past_out_2b_low_left * _low_out_2;

			_past_in_2b_low_left = _past_in_1b_low_left;
			_past_in_1b_low_left = low;
			_past_out_2b_low_left = _past_out_1b_low_left;
			_past_out_1b_low_left = low_final;

			// Process high band - first stage
			double high_in01 = audio * _high_in_0 + _past_in_1a_high_left * _high_in_1;
			double high_in = high_in01 + _past_in_2a_high_left * _high_in_2;
			double high_in_out1 = high_in + _past_out_1a_high_left * _high_out_1;
			double high = high_in_out1 + _past_out_2a_high_left * _high_out_2;

			_past_in_2a_high_left = _past_in_1a_high_left;
			_past_in_1a_high_left = audio;
			_past_out_2a_high_left = _past_out_1a_high_left;
			_past_out_1a_high_left = high;

			// Process high band - second stage
			double high_in01_b = high * _high_in_0 + _past_in_1b_high_left * _high_in_1;
			double high_in_b = high_in01_b + _past_in_2b_high_left * _high_in_2;
			double high_in_out1_b = high_in_b + _past_out_1b_high_left * _high_out_1;
			double high_final = high_in_out1_b + _past_out_2b_high_left * _high_out_2;

			_past_in_2b_high_left = _past_in_1b_high_left;
			_past_in_1b_high_left = high;
			_past_out_2b_high_left = _past_out_1b_high_left;
			_past_out_1b_high_left = high_final;

			// Output based on mode
			double output_value = (_output_mode == 0) ? low_final : high_final;
			r_buffer->write[i] = output_value;
			r_buffer->write[i + 1] = output_value;
		}
	} else {
		// Stereo processing - separate left and right channels
		for (int i = start_index; i < (start_index + length); i += 2) {
			double audio_left = (*r_buffer)[i];
			double audio_right = (*r_buffer)[i + 1];

			// Process low band for left channel - first stage
			double low_in01_left = audio_left * _low_in_0 + _past_in_1a_low_left * _low_in_1;
			double low_in_left = low_in01_left + _past_in_2a_low_left * _low_in_2;
			double low_in_out1_left = low_in_left + _past_out_1a_low_left * _low_out_1;
			double low_left = low_in_out1_left + _past_out_2a_low_left * _low_out_2;

			_past_in_2a_low_left = _past_in_1a_low_left;
			_past_in_1a_low_left = audio_left;
			_past_out_2a_low_left = _past_out_1a_low_left;
			_past_out_1a_low_left = low_left;

			// Process low band for left channel - second stage
			double low_in01_b_left = low_left * _low_in_0 + _past_in_1b_low_left * _low_in_1;
			double low_in_b_left = low_in01_b_left + _past_in_2b_low_left * _low_in_2;
			double low_in_out1_b_left = low_in_b_left + _past_out_1b_low_left * _low_out_1;
			double low_final_left = low_in_out1_b_left + _past_out_2b_low_left * _low_out_2;

			_past_in_2b_low_left = _past_in_1b_low_left;
			_past_in_1b_low_left = low_left;
			_past_out_2b_low_left = _past_out_1b_low_left;
			_past_out_1b_low_left = low_final_left;

			// Process low band for right channel - first stage
			double low_in01_right = audio_right * _low_in_0 + _past_in_1a_low_right * _low_in_1;
			double low_in_right = low_in01_right + _past_in_2a_low_right * _low_in_2;
			double low_in_out1_right = low_in_right + _past_out_1a_low_right * _low_out_1;
			double low_right = low_in_out1_right + _past_out_2a_low_right * _low_out_2;

			_past_in_2a_low_right = _past_in_1a_low_right;
			_past_in_1a_low_right = audio_right;
			_past_out_2a_low_right = _past_out_1a_low_right;
			_past_out_1a_low_right = low_right;

			// Process low band for right channel - second stage
			double low_in01_b_right = low_right * _low_in_0 + _past_in_1b_low_right * _low_in_1;
			double low_in_b_right = low_in01_b_right + _past_in_2b_low_right * _low_in_2;
			double low_in_out1_b_right = low_in_b_right + _past_out_1b_low_right * _low_out_1;
			double low_final_right = low_in_out1_b_right + _past_out_2b_low_right * _low_out_2;

			_past_in_2b_low_right = _past_in_1b_low_right;
			_past_in_1b_low_right = low_right;
			_past_out_2b_low_right = _past_out_1b_low_right;
			_past_out_1b_low_right = low_final_right;

			// Process high band for left channel - first stage
			double high_in01_left = audio_left * _high_in_0 + _past_in_1a_high_left * _high_in_1;
			double high_in_left = high_in01_left + _past_in_2a_high_left * _high_in_2;
			double high_in_out1_left = high_in_left + _past_out_1a_high_left * _high_out_1;
			double high_left = high_in_out1_left + _past_out_2a_high_left * _high_out_2;

			_past_in_2a_high_left = _past_in_1a_high_left;
			_past_in_1a_high_left = audio_left;
			_past_out_2a_high_left = _past_out_1a_high_left;
			_past_out_1a_high_left = high_left;

			// Process high band for left channel - second stage
			double high_in01_b_left = high_left * _high_in_0 + _past_in_1b_high_left * _high_in_1;
			double high_in_b_left = high_in01_b_left + _past_in_2b_high_left * _high_in_2;
			double high_in_out1_b_left = high_in_b_left + _past_out_1b_high_left * _high_out_1;
			double high_final_left = high_in_out1_b_left + _past_out_2b_high_left * _high_out_2;

			_past_in_2b_high_left = _past_in_1b_high_left;
			_past_in_1b_high_left = high_left;
			_past_out_2b_high_left = _past_out_1b_high_left;
			_past_out_1b_high_left = high_final_left;

			// Process high band for right channel - first stage
			double high_in01_right = audio_right * _high_in_0 + _past_in_1a_high_right * _high_in_1;
			double high_in_right = high_in01_right + _past_in_2a_high_right * _high_in_2;
			double high_in_out1_right = high_in_right + _past_out_1a_high_right * _high_out_1;
			double high_right = high_in_out1_right + _past_out_2a_high_right * _high_out_2;

			_past_in_2a_high_right = _past_in_1a_high_right;
			_past_in_1a_high_right = audio_right;
			_past_out_2a_high_right = _past_out_1a_high_right;
			_past_out_1a_high_right = high_right;

			// Process high band for right channel - second stage
			double high_in01_b_right = high_right * _high_in_0 + _past_in_1b_high_right * _high_in_1;
			double high_in_b_right = high_in01_b_right + _past_in_2b_high_right * _high_in_2;
			double high_in_out1_b_right = high_in_b_right + _past_out_1b_high_right * _high_out_1;
			double high_final_right = high_in_out1_b_right + _past_out_2b_high_right * _high_out_2;

			_past_in_2b_high_right = _past_in_1b_high_right;
			_past_in_1b_high_right = high_right;
			_past_out_2b_high_right = _past_out_1b_high_right;
			_past_out_1b_high_right = high_final_right;

			// Output based on mode
			if (_output_mode == 0) {
				r_buffer->write[i] = low_final_left;
				r_buffer->write[i + 1] = low_final_right;
			} else {
				r_buffer->write[i] = high_final_left;
				r_buffer->write[i + 1] = high_final_right;
			}
		}
	}

	return p_channels;
}

void SiEffectLinkwitzRileyFilter::set_by_mml(Vector<double> p_args) {
	double cutoff = _get_mml_arg(p_args, 0, 1000.0);
	int output_mode = (int)_get_mml_arg(p_args, 1, 0.0);

	cutoff = CLAMP(cutoff, 20.0, 20000.0);
	output_mode = (output_mode == 0 || output_mode == 1) ? output_mode : 0;

	set_params(cutoff, output_mode);
}

void SiEffectLinkwitzRileyFilter::reset() {
	// Reset all past values
	_past_in_1a_low_left = 0.0;
	_past_in_2a_low_left = 0.0;
	_past_out_1a_low_left = 0.0;
	_past_out_2a_low_left = 0.0;
	_past_in_1b_low_left = 0.0;
	_past_in_2b_low_left = 0.0;
	_past_out_1b_low_left = 0.0;
	_past_out_2b_low_left = 0.0;

	_past_in_1a_high_left = 0.0;
	_past_in_2a_high_left = 0.0;
	_past_out_1a_high_left = 0.0;
	_past_out_2a_high_left = 0.0;
	_past_in_1b_high_left = 0.0;
	_past_in_2b_high_left = 0.0;
	_past_out_1b_high_left = 0.0;
	_past_out_2b_high_left = 0.0;

	_past_in_1a_low_right = 0.0;
	_past_in_2a_low_right = 0.0;
	_past_out_1a_low_right = 0.0;
	_past_out_2a_low_right = 0.0;
	_past_in_1b_low_right = 0.0;
	_past_in_2b_low_right = 0.0;
	_past_out_1b_low_right = 0.0;
	_past_out_2b_low_right = 0.0;

	_past_in_1a_high_right = 0.0;
	_past_in_2a_high_right = 0.0;
	_past_out_1a_high_right = 0.0;
	_past_out_2a_high_right = 0.0;
	_past_in_1b_high_right = 0.0;
	_past_in_2b_high_right = 0.0;
	_past_out_1b_high_right = 0.0;
	_past_out_2b_high_right = 0.0;

	set_params(_cutoff, _output_mode);
}

void SiEffectLinkwitzRileyFilter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_params", "cutoff_frequency", "output_mode"), &SiEffectLinkwitzRileyFilter::set_params, DEFVAL(1000.0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_cutoff_frequency", "value"), &SiEffectLinkwitzRileyFilter::set_cutoff_frequency);
	ClassDB::bind_method(D_METHOD("get_cutoff_frequency"), &SiEffectLinkwitzRileyFilter::get_cutoff_frequency);
	ClassDB::bind_method(D_METHOD("set_output_mode", "value"), &SiEffectLinkwitzRileyFilter::set_output_mode);
	ClassDB::bind_method(D_METHOD("get_output_mode"), &SiEffectLinkwitzRileyFilter::get_output_mode);
}

SiEffectLinkwitzRileyFilter::SiEffectLinkwitzRileyFilter(double p_cutoff_frequency, int p_output_mode) :
		SiEffectBase() {
	set_params(p_cutoff_frequency, p_output_mode);
}
