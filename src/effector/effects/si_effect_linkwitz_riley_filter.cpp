/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_linkwitz_riley_filter.h"

void SiEffectLinkwitzRileyFilter::set_params(double p_cutoff_frequency, int p_output_mode) {
	_cutoff = CLAMP(p_cutoff_frequency, 20.0, 20000.0);
	_output_mode = (p_output_mode == 0 || p_output_mode == 1) ? p_output_mode : 0;
	_coeffs.set_cutoff(_cutoff, _get_sampling_rate());
}

int SiEffectLinkwitzRileyFilter::prepare_process() {
	return 2;
}

int SiEffectLinkwitzRileyFilter::process(const ProcessContext &p_context, int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	int length = p_length << 1;
	double *buffer = r_buffer->ptrw() + (p_start_index << 1);
	const bool output_low = _output_mode == 0;
	double low = 0.0;
	double high = 0.0;

	for (int i = 0; i < length; i += 2) {
		_left.split(_coeffs, buffer[i], low, high);
		buffer[i] = output_low ? low : high;

		// A mono buffer carries the same signal in both channels.
		if (p_channels == 1) {
			buffer[i + 1] = buffer[i];
		} else {
			_right.split(_coeffs, buffer[i + 1], low, high);
			buffer[i + 1] = output_low ? low : high;
		}
	}

	return p_channels;
}

void SiEffectLinkwitzRileyFilter::set_by_mml(Vector<double> p_args) {
	set_params(_get_mml_arg(p_args, 0, 1000.0), (int)_get_mml_arg(p_args, 1, 0.0));
}

void SiEffectLinkwitzRileyFilter::reset() {
	_left = {};
	_right = {};
	// A pooled instance is reset after its sampling rate is refreshed.
	set_params(_cutoff, _output_mode);
}

void SiEffectLinkwitzRileyFilter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_params", "cutoff_frequency", "output_mode"), &SiEffectLinkwitzRileyFilter::set_params, DEFVAL(1000.0), DEFVAL(0));
}

SiEffectLinkwitzRileyFilter::SiEffectLinkwitzRileyFilter() :
		SiEffectBase() {
	set_params(_cutoff, _output_mode);
}
