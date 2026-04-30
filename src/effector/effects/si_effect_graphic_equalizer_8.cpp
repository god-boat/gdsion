/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_graphic_equalizer_8.h"

const double SiEffectGraphicEqualizer8::DENORMAL_THRESHOLD = 1e-15;

const double SiEffectGraphicEqualizer8::DEFAULT_FREQS[NUM_BANDS] = {
	32.0, 64.0, 125.0, 250.0, 500.0, 1000.0, 4000.0, 8000.0
};

// --- Internal helpers ---

void SiEffectGraphicEqualizer8::_recompute_band(int p_band) {
	Band &b = _bands[p_band];
	if (!b.enabled) {
		b.target = BiquadCoeffs{ 1.0, 0.0, 0.0, 0.0, 0.0 };
	} else {
		b.target = compute_biquad_coefficients(b.type, b.freq_hz, b.q, b.gain_db, _get_sampling_rate());
	}
	if (!_initialized) {
		b.current = b.target;
		b.dirty = false;
	} else {
		b.dirty = true;
	}
}

void SiEffectGraphicEqualizer8::_apply_band_params(int p_band, int p_type, bool p_enabled, double p_freq_hz, double p_gain_db, double p_q) {
	Band &b = _bands[p_band];
	b.type = CLAMP(p_type, 0, (int)(FILTER_TYPE_MAX - 1));
	b.enabled = p_enabled;
	b.freq_hz = MAX(p_freq_hz, 10.0);
	b.gain_db = p_gain_db;
	b.q = MAX(p_q, 0.01);
	_recompute_band(p_band);
}

void SiEffectGraphicEqualizer8::_snap_all() {
	for (int i = 0; i < NUM_BANDS; i++) {
		Band &b = _bands[i];
		b.current = b.target;
		b.dirty = false;
		b.step = BiquadCoeffs{ 0, 0, 0, 0, 0 };
	}
	_output_gain = _target_output_gain;
	_output_gain_dirty = false;
	_output_gain_step = 0.0;
}

double SiEffectGraphicEqualizer8::_process_biquad(BandChannelState *p_state, const BiquadCoeffs &p_coeffs, double p_input) const {
	double output = p_coeffs.b0 * p_input + p_coeffs.b1 * p_state->in1 + p_coeffs.b2 * p_state->in2
			- p_coeffs.a1 * p_state->out1 - p_coeffs.a2 * p_state->out2;

	p_state->in2 = p_state->in1;
	p_state->in1 = p_input;
	p_state->out2 = p_state->out1;
	p_state->out1 = output;

	return output;
}

// --- Public API ---

void SiEffectGraphicEqualizer8::set_band_params(int p_band, int p_type, bool p_enabled, double p_freq_hz, double p_gain_db, double p_q) {
	ERR_FAIL_INDEX(p_band, NUM_BANDS);
	_apply_band_params(p_band, p_type, p_enabled, p_freq_hz, p_gain_db, p_q);
}

void SiEffectGraphicEqualizer8::set_output_gain_db(double p_gain_db) {
	_target_output_gain = Math::pow(10.0, CLAMP(p_gain_db, -60.0, 24.0) / 20.0);
	if (!_initialized) {
		_output_gain = _target_output_gain;
		_output_gain_dirty = false;
	} else {
		_output_gain_dirty = true;
	}
}

// --- SiEffectBase overrides ---

int SiEffectGraphicEqualizer8::prepare_process() {
	for (int i = 0; i < NUM_BANDS; i++) {
		_bands[i].left.clear();
		_bands[i].right.clear();
	}
	return 2;
}

int SiEffectGraphicEqualizer8::process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	int start_index = p_start_index << 1;
	int length = p_length << 1;

	if (length <= 0) {
		return p_channels;
	}

	int block_samples = p_length;

	// Prepare interpolation steps for dirty bands.
	double inv_block = 1.0 / (double)block_samples;
	for (int b = 0; b < NUM_BANDS; b++) {
		Band &band = _bands[b];
		if (band.dirty) {
			band.step.b0 = (band.target.b0 - band.current.b0) * inv_block;
			band.step.b1 = (band.target.b1 - band.current.b1) * inv_block;
			band.step.b2 = (band.target.b2 - band.current.b2) * inv_block;
			band.step.a1 = (band.target.a1 - band.current.a1) * inv_block;
			band.step.a2 = (band.target.a2 - band.current.a2) * inv_block;
		}
	}

	if (_output_gain_dirty) {
		_output_gain_step = (_target_output_gain - _output_gain) * inv_block;
	}

	// Process samples.
	bool stereo = (p_channels == 2);

	for (int i = start_index; i < start_index + length; i += 2) {
		double left = (*r_buffer)[i];
		double right = stereo ? (*r_buffer)[i + 1] : left;

		for (int b = 0; b < NUM_BANDS; b++) {
			Band &band = _bands[b];
			if (!band.enabled) {
				continue;
			}

			left = _process_biquad(&band.left, band.current, left);
			right = _process_biquad(&band.right, band.current, right);

			if (band.dirty) {
				band.current.b0 += band.step.b0;
				band.current.b1 += band.step.b1;
				band.current.b2 += band.step.b2;
				band.current.a1 += band.step.a1;
				band.current.a2 += band.step.a2;
			}
		}

		double gain = _output_gain;
		if (_output_gain_dirty) {
			_output_gain += _output_gain_step;
			gain = _output_gain;
		}

		r_buffer->write[i] = left * gain;
		r_buffer->write[i + 1] = right * gain;
	}

	// Snap all interpolating bands to target and flush denormals.
	for (int b = 0; b < NUM_BANDS; b++) {
		Band &band = _bands[b];
		if (band.dirty) {
			band.current = band.target;
			band.dirty = false;
			band.step = BiquadCoeffs{ 0, 0, 0, 0, 0 };
		}
		band.left.flush_denormals();
		band.right.flush_denormals();
	}

	if (_output_gain_dirty) {
		_output_gain = _target_output_gain;
		_output_gain_dirty = false;
		_output_gain_step = 0.0;
	}

	return 2;
}

void SiEffectGraphicEqualizer8::set_by_mml(Vector<double> p_args) {
	// Arg layout:
	//   [0]: output_gain_db
	//   For band i (0-7), base = 1 + i * 5:
	//     [base+0]: filter type
	//     [base+1]: enabled (1.0 = on, 0.0 = off)
	//     [base+2]: frequency_hz
	//     [base+3]: gain_db
	//     [base+4]: q

	double output_gain_db = _get_mml_arg(p_args, 0, 0.0);
	set_output_gain_db(output_gain_db);

	for (int i = 0; i < NUM_BANDS; i++) {
		int base = 1 + i * PARAMS_PER_BAND;
		int type = (int)_get_mml_arg(p_args, base + 0, (double)FILTER_PEAK);
		bool enabled = _get_mml_arg(p_args, base + 1, 1.0) >= 0.5;
		double freq = _get_mml_arg(p_args, base + 2, DEFAULT_FREQS[i]);
		double gain = _get_mml_arg(p_args, base + 3, 0.0);
		double q = _get_mml_arg(p_args, base + 4, 1.0);
		_apply_band_params(i, type, enabled, freq, gain, q);
	}

	_initialized = true;
}

void SiEffectGraphicEqualizer8::reset() {
	_initialized = false;
	_output_gain = 1.0;
	_target_output_gain = 1.0;
	_output_gain_step = 0.0;
	_output_gain_dirty = false;

	for (int i = 0; i < NUM_BANDS; i++) {
		Band &b = _bands[i];
		b.type = FILTER_PEAK;
		b.enabled = true;
		b.freq_hz = DEFAULT_FREQS[i];
		b.gain_db = 0.0;
		b.q = 1.0;
		b.current = BiquadCoeffs{ 1.0, 0.0, 0.0, 0.0, 0.0 };
		b.target = b.current;
		b.clear_state();
	}
}

void SiEffectGraphicEqualizer8::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_band_params", "band", "type", "enabled", "freq_hz", "gain_db", "q"),
			&SiEffectGraphicEqualizer8::set_band_params);
	ClassDB::bind_method(D_METHOD("set_output_gain_db", "gain_db"), &SiEffectGraphicEqualizer8::set_output_gain_db);

	BIND_ENUM_CONSTANT(FILTER_PEAK);
	BIND_ENUM_CONSTANT(FILTER_LOW_PASS);
	BIND_ENUM_CONSTANT(FILTER_HIGH_PASS);
	BIND_ENUM_CONSTANT(FILTER_BAND_PASS);
	BIND_ENUM_CONSTANT(FILTER_NOTCH);
	BIND_ENUM_CONSTANT(FILTER_LOW_SHELF);
	BIND_ENUM_CONSTANT(FILTER_HIGH_SHELF);
	BIND_ENUM_CONSTANT(FILTER_ALL_PASS);
}

SiEffectGraphicEqualizer8::SiEffectGraphicEqualizer8() :
		SiEffectBase() {
	reset();
}
