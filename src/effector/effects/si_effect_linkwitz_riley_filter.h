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

#ifndef SI_EFFECT_LINKWITZ_RILEY_FILTER_H
#define SI_EFFECT_LINKWITZ_RILEY_FILTER_H

#include "effector/si_effect_base.h"
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

class SiEffectLinkwitzRileyFilter : public SiEffectBase {
	GDCLASS(SiEffectLinkwitzRileyFilter, SiEffectBase)

	static const int SAMPLE_RATE = 48000;
	static const double PI;
	static const double SQRT2;

	double _cutoff = 1000.0;

	// Coefficients.
	double _low_in_0 = 0.0;
	double _low_in_1 = 0.0;
	double _low_in_2 = 0.0;
	double _low_out_1 = 0.0;
	double _low_out_2 = 0.0;
	double _high_in_0 = 0.0;
	double _high_in_1 = 0.0;
	double _high_in_2 = 0.0;
	double _high_out_1 = 0.0;
	double _high_out_2 = 0.0;

	// Past input and output values for left channel (low and high bands).
	double _past_in_1a_low_left = 0.0;
	double _past_in_2a_low_left = 0.0;
	double _past_out_1a_low_left = 0.0;
	double _past_out_2a_low_left = 0.0;
	double _past_in_1b_low_left = 0.0;
	double _past_in_2b_low_left = 0.0;
	double _past_out_1b_low_left = 0.0;
	double _past_out_2b_low_left = 0.0;

	double _past_in_1a_high_left = 0.0;
	double _past_in_2a_high_left = 0.0;
	double _past_out_1a_high_left = 0.0;
	double _past_out_2a_high_left = 0.0;
	double _past_in_1b_high_left = 0.0;
	double _past_in_2b_high_left = 0.0;
	double _past_out_1b_high_left = 0.0;
	double _past_out_2b_high_left = 0.0;

	// Past input and output values for right channel (low and high bands).
	double _past_in_1a_low_right = 0.0;
	double _past_in_2a_low_right = 0.0;
	double _past_out_1a_low_right = 0.0;
	double _past_out_2a_low_right = 0.0;
	double _past_in_1b_low_right = 0.0;
	double _past_in_2b_low_right = 0.0;
	double _past_out_1b_low_right = 0.0;
	double _past_out_2b_low_right = 0.0;

	double _past_in_1a_high_right = 0.0;
	double _past_in_2a_high_right = 0.0;
	double _past_out_1a_high_right = 0.0;
	double _past_out_2a_high_right = 0.0;
	double _past_in_1b_high_right = 0.0;
	double _past_in_2b_high_right = 0.0;
	double _past_out_1b_high_right = 0.0;
	double _past_out_2b_high_right = 0.0;

	// Output mode: 0 = low band, 1 = high band
	int _output_mode = 0;

	void _compute_coefficients(double p_frequency);

protected:
	static void _bind_methods();

public:
	void set_params(double p_cutoff_frequency, int p_output_mode = 0);

	double get_cutoff_frequency() const { return _cutoff; }
	void set_cutoff_frequency(double p_value);

	int get_output_mode() const { return _output_mode; }
	void set_output_mode(int p_value);

	//

	virtual int prepare_process() override;
	virtual int process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual void reset() override;

	SiEffectLinkwitzRileyFilter(double p_cutoff_frequency = 1000.0, int p_output_mode = 0);
	~SiEffectLinkwitzRileyFilter() {}
};

#endif // SI_EFFECT_LINKWITZ_RILEY_FILTER_H
