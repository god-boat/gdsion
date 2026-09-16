#include "si_effect_mb_compressor.h"

using namespace godot;

// -----------------------------------------------------------------------------
// BandCompressor
// -----------------------------------------------------------------------------

void SiEffectMultibandCompressor::BandCompressor::reset() {
	_envelope = 0.0;
	_input_mean_squared = 0.0;
	_output_mean_squared = 0.0;
	_output_mult = 1.0;

	_meter_input_db.store(-120.0f, std::memory_order_relaxed);
	_meter_output_db.store(-120.0f, std::memory_order_relaxed);
	_meter_downward_db.store(0.0f, std::memory_order_relaxed);
	_meter_upward_db.store(0.0f, std::memory_order_relaxed);
}

void SiEffectMultibandCompressor::BandCompressor::process(double *p_audio, int p_length, const BandSettings &p_settings, double p_sample_rate) {
	if (p_length <= 0 || p_sample_rate <= 0.0) {
		return;
	}

	// One pair of time constants for the whole band. Both the upward and the
	// downward section run off them, because the band owns its ballistics.
	double attack_ms = MAX(_base_attack_ms * std::exp(p_settings.attack * TIME_EXP_SPAN - TIME_EXP_BIAS), MIN_ENVELOPE_MS);
	double release_ms = MAX(_base_release_ms * std::exp(p_settings.release * TIME_EXP_SPAN - TIME_EXP_BIAS), MIN_ENVELOPE_MS);

	double attack_coeff = _compute_coeff(attack_ms, p_sample_rate);
	double release_coeff = _compute_coeff(release_ms, p_sample_rate);
	double meter_coeff = _compute_coeff(RMS_METER_MS, p_sample_rate);

	double upper_threshold_db = p_settings.upper_threshold_db;
	double lower_threshold_db = p_settings.lower_threshold_db;
	double upper_amount = p_settings.upper_amount;
	double lower_amount = p_settings.lower_amount;
	double knee_db = p_settings.knee_db;

	// De-zipper the makeup gain across the block.
	double target_output_mult = _db_to_linear(p_settings.output_gain_db);
	double delta_output_mult = (target_output_mult - _output_mult) / p_length;

	// Dry/wet is blended here, against this band's own pre-compression signal,
	// rather than against the effect's input. The crossover network hands every
	// band the phase of the splitters it passed through, so blending a band
	// against the unfiltered input would comb-filter the crossovers: at 50% the
	// wet sum is exactly antiphase there and nulls outright.
	double mix = p_settings.mix_start;
	double mix_delta = p_settings.mix_delta;
	// Exact comparisons on purpose: a fully wet block is the shipped default,
	// and this skips a blend whose result would only round back to the wet
	// sample anyway.
	const bool full_wet = (mix >= 1.0 && mix_delta == 0.0);

	double envelope = _envelope;
	double input_mean_squared = _input_mean_squared;
	double output_mean_squared = _output_mean_squared;
	double downward_db = 0.0;
	double upward_db = 0.0;

	for (int i = 0; i < p_length; ++i) {
		int frame = i << 1;
		double dry_left = p_audio[frame];
		double dry_right = p_audio[frame + 1];

		// Stereo-linked mean square. The 0.5 is what keeps a mono signal from
		// reading 3 dB hot against thresholds that are quoted per channel.
		double energy = 0.5 * (dry_left * dry_left + dry_right * dry_right);

		double coeff = (energy > envelope) ? attack_coeff : release_coeff;
		envelope = coeff * envelope + (1.0 - coeff) * energy;
		if (envelope < EPSILON) {
			envelope = 0.0; // Keeps a decaying tail out of denormal range.
		}
		double envelope_db = _mean_square_to_db(envelope);

		// Downward above the upper threshold, upward below the lower one, both
		// off the same envelope. A negative lower amount expands downward
		// instead of compressing upward.
		downward_db = -upper_amount * _soft_excess_db(envelope_db - upper_threshold_db, knee_db);
		upward_db = lower_amount * _soft_excess_db(lower_threshold_db - envelope_db, knee_db);
		upward_db = CLAMP(upward_db, -MAX_UPWARD_GAIN_DB, MAX_UPWARD_GAIN_DB);

		_output_mult += delta_output_mult;
		mix += mix_delta;

		double gain = _db_to_linear(MAX(downward_db + upward_db, MIN_TOTAL_GAIN_DB)) * _output_mult;
		double wet_left = dry_left * gain;
		double wet_right = dry_right * gain;
		double out_left = full_wet ? wet_left : dry_left + (wet_left - dry_left) * mix;
		double out_right = full_wet ? wet_right : dry_right + (wet_right - dry_right) * mix;
		p_audio[frame] = out_left;
		p_audio[frame + 1] = out_right;

		// Metering rides along in the same pass instead of costing the band two
		// extra sweeps over the buffer. Output is metered post-blend, which is
		// what the band actually contributes to the sum.
		input_mean_squared = meter_coeff * input_mean_squared + (1.0 - meter_coeff) * energy;
		output_mean_squared = meter_coeff * output_mean_squared + (1.0 - meter_coeff) * 0.5 * (out_left * out_left + out_right * out_right);
	}

	if (input_mean_squared < EPSILON) {
		input_mean_squared = 0.0;
	}
	if (output_mean_squared < EPSILON) {
		output_mean_squared = 0.0;
	}

	_envelope = envelope;
	_input_mean_squared = input_mean_squared;
	_output_mean_squared = output_mean_squared;
	_output_mult = target_output_mult;

	// Reduction is published positive, boost separately, so a UI can show both
	// halves of the band the way OTT-style meters do.
	_meter_input_db.store((float)_mean_square_to_db(input_mean_squared), std::memory_order_relaxed);
	_meter_output_db.store((float)_mean_square_to_db(output_mean_squared), std::memory_order_relaxed);
	_meter_downward_db.store((float)(-downward_db), std::memory_order_relaxed);
	_meter_upward_db.store((float)upward_db, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// Parameters
// -----------------------------------------------------------------------------

void SiEffectMultibandCompressor::set_params(int p_enabled_bands,
		double p_low_upper_threshold, double p_mid_upper_threshold, double p_high_upper_threshold,
		double p_low_lower_threshold, double p_mid_lower_threshold, double p_high_lower_threshold,
		double p_low_upper_amount, double p_mid_upper_amount, double p_high_upper_amount,
		double p_low_lower_amount, double p_mid_lower_amount, double p_high_lower_amount,
		double p_low_output_gain, double p_mid_output_gain, double p_high_output_gain,
		double p_attack, double p_release, double p_mix,
		double p_lm_frequency, double p_mh_frequency, double p_knee_db) {
	_params.enabled_bands.store((float)CLAMP(p_enabled_bands, (int)BAND_MULTIBAND, (int)BAND_FULL_RANGE), std::memory_order_relaxed);

	_params.bands[BAND_LOW_INDEX].upper_threshold.store((float)CLAMP(p_low_upper_threshold, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed);
	_params.bands[BAND_MID_INDEX].upper_threshold.store((float)CLAMP(p_mid_upper_threshold, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed);
	_params.bands[BAND_HIGH_INDEX].upper_threshold.store((float)CLAMP(p_high_upper_threshold, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed);

	_params.bands[BAND_LOW_INDEX].lower_threshold.store((float)CLAMP(p_low_lower_threshold, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed);
	_params.bands[BAND_MID_INDEX].lower_threshold.store((float)CLAMP(p_mid_lower_threshold, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed);
	_params.bands[BAND_HIGH_INDEX].lower_threshold.store((float)CLAMP(p_high_lower_threshold, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed);

	_params.bands[BAND_LOW_INDEX].upper_amount.store((float)CLAMP(p_low_upper_amount, 0.0, 1.0), std::memory_order_relaxed);
	_params.bands[BAND_MID_INDEX].upper_amount.store((float)CLAMP(p_mid_upper_amount, 0.0, 1.0), std::memory_order_relaxed);
	_params.bands[BAND_HIGH_INDEX].upper_amount.store((float)CLAMP(p_high_upper_amount, 0.0, 1.0), std::memory_order_relaxed);

	_params.bands[BAND_LOW_INDEX].lower_amount.store((float)CLAMP(p_low_lower_amount, -1.0, 1.0), std::memory_order_relaxed);
	_params.bands[BAND_MID_INDEX].lower_amount.store((float)CLAMP(p_mid_lower_amount, -1.0, 1.0), std::memory_order_relaxed);
	_params.bands[BAND_HIGH_INDEX].lower_amount.store((float)CLAMP(p_high_lower_amount, -1.0, 1.0), std::memory_order_relaxed);

	_params.bands[BAND_LOW_INDEX].output_gain.store((float)CLAMP(p_low_output_gain, MIN_GAIN, MAX_GAIN), std::memory_order_relaxed);
	_params.bands[BAND_MID_INDEX].output_gain.store((float)CLAMP(p_mid_output_gain, MIN_GAIN, MAX_GAIN), std::memory_order_relaxed);
	_params.bands[BAND_HIGH_INDEX].output_gain.store((float)CLAMP(p_high_output_gain, MIN_GAIN, MAX_GAIN), std::memory_order_relaxed);

	_params.attack.store((float)CLAMP(p_attack, 0.0, 1.0), std::memory_order_relaxed);
	_params.release.store((float)CLAMP(p_release, 0.0, 1.0), std::memory_order_relaxed);
	_params.mix.store((float)CLAMP(p_mix, 0.0, 1.0), std::memory_order_relaxed);
	_params.lm_frequency.store((float)CLAMP(p_lm_frequency, MIN_FREQUENCY, MAX_FREQUENCY), std::memory_order_relaxed);
	_params.mh_frequency.store((float)CLAMP(p_mh_frequency, MIN_FREQUENCY, MAX_FREQUENCY), std::memory_order_relaxed);
	_params.knee_db.store((float)CLAMP(p_knee_db, 0.0, MAX_KNEE), std::memory_order_relaxed);
}

bool SiEffectMultibandCompressor::set_arg(int p_arg_index, double p_value) {
	switch (p_arg_index) {
		case 0: _params.enabled_bands.store((float)CLAMP((int)Math::round(p_value), (int)BAND_MULTIBAND, (int)BAND_FULL_RANGE), std::memory_order_relaxed); return true;

		case 1: _params.bands[BAND_LOW_INDEX].upper_threshold.store((float)CLAMP(p_value, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed); return true;
		case 2: _params.bands[BAND_MID_INDEX].upper_threshold.store((float)CLAMP(p_value, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed); return true;
		case 3: _params.bands[BAND_HIGH_INDEX].upper_threshold.store((float)CLAMP(p_value, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed); return true;

		case 4: _params.bands[BAND_LOW_INDEX].lower_threshold.store((float)CLAMP(p_value, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed); return true;
		case 5: _params.bands[BAND_MID_INDEX].lower_threshold.store((float)CLAMP(p_value, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed); return true;
		case 6: _params.bands[BAND_HIGH_INDEX].lower_threshold.store((float)CLAMP(p_value, MIN_THRESHOLD, MAX_THRESHOLD), std::memory_order_relaxed); return true;

		case 7: _params.bands[BAND_LOW_INDEX].upper_amount.store((float)CLAMP(p_value, 0.0, 1.0), std::memory_order_relaxed); return true;
		case 8: _params.bands[BAND_MID_INDEX].upper_amount.store((float)CLAMP(p_value, 0.0, 1.0), std::memory_order_relaxed); return true;
		case 9: _params.bands[BAND_HIGH_INDEX].upper_amount.store((float)CLAMP(p_value, 0.0, 1.0), std::memory_order_relaxed); return true;

		case 10: _params.bands[BAND_LOW_INDEX].lower_amount.store((float)CLAMP(p_value, -1.0, 1.0), std::memory_order_relaxed); return true;
		case 11: _params.bands[BAND_MID_INDEX].lower_amount.store((float)CLAMP(p_value, -1.0, 1.0), std::memory_order_relaxed); return true;
		case 12: _params.bands[BAND_HIGH_INDEX].lower_amount.store((float)CLAMP(p_value, -1.0, 1.0), std::memory_order_relaxed); return true;

		case 13: _params.bands[BAND_LOW_INDEX].output_gain.store((float)CLAMP(p_value, MIN_GAIN, MAX_GAIN), std::memory_order_relaxed); return true;
		case 14: _params.bands[BAND_MID_INDEX].output_gain.store((float)CLAMP(p_value, MIN_GAIN, MAX_GAIN), std::memory_order_relaxed); return true;
		case 15: _params.bands[BAND_HIGH_INDEX].output_gain.store((float)CLAMP(p_value, MIN_GAIN, MAX_GAIN), std::memory_order_relaxed); return true;

		case 16: _params.attack.store((float)CLAMP(p_value, 0.0, 1.0), std::memory_order_relaxed); return true;
		case 17: _params.release.store((float)CLAMP(p_value, 0.0, 1.0), std::memory_order_relaxed); return true;
		case 18: _params.mix.store((float)CLAMP(p_value, 0.0, 1.0), std::memory_order_relaxed); return true;
		case 19: _params.lm_frequency.store((float)CLAMP(p_value, MIN_FREQUENCY, MAX_FREQUENCY), std::memory_order_relaxed); return true;
		case 20: _params.mh_frequency.store((float)CLAMP(p_value, MIN_FREQUENCY, MAX_FREQUENCY), std::memory_order_relaxed); return true;
		case 21: _params.knee_db.store((float)CLAMP(p_value, 0.0, MAX_KNEE), std::memory_order_relaxed); return true;

		default: return false;
	}
}

void SiEffectMultibandCompressor::set_by_mml(Vector<double> p_args) {
	set_params((int)_get_mml_arg(p_args, 0, (double)BAND_MULTIBAND),
			_get_mml_arg(p_args, 1, DEFAULT_LOW_UPPER_THRESHOLD),
			_get_mml_arg(p_args, 2, DEFAULT_MID_UPPER_THRESHOLD),
			_get_mml_arg(p_args, 3, DEFAULT_HIGH_UPPER_THRESHOLD),
			_get_mml_arg(p_args, 4, DEFAULT_LOW_LOWER_THRESHOLD),
			_get_mml_arg(p_args, 5, DEFAULT_MID_LOWER_THRESHOLD),
			_get_mml_arg(p_args, 6, DEFAULT_HIGH_LOWER_THRESHOLD),
			_get_mml_arg(p_args, 7, DEFAULT_LOW_UPPER_AMOUNT),
			_get_mml_arg(p_args, 8, DEFAULT_MID_UPPER_AMOUNT),
			_get_mml_arg(p_args, 9, DEFAULT_HIGH_UPPER_AMOUNT),
			_get_mml_arg(p_args, 10, DEFAULT_LOW_LOWER_AMOUNT),
			_get_mml_arg(p_args, 11, DEFAULT_MID_LOWER_AMOUNT),
			_get_mml_arg(p_args, 12, DEFAULT_HIGH_LOWER_AMOUNT),
			_get_mml_arg(p_args, 13, DEFAULT_LOW_OUTPUT_GAIN),
			_get_mml_arg(p_args, 14, DEFAULT_MID_OUTPUT_GAIN),
			_get_mml_arg(p_args, 15, DEFAULT_HIGH_OUTPUT_GAIN),
			_get_mml_arg(p_args, 16, DEFAULT_ATTACK),
			_get_mml_arg(p_args, 17, DEFAULT_RELEASE),
			_get_mml_arg(p_args, 18, DEFAULT_MIX),
			_get_mml_arg(p_args, 19, DEFAULT_LM_FREQUENCY),
			_get_mml_arg(p_args, 20, DEFAULT_MH_FREQUENCY),
			_get_mml_arg(p_args, 21, DEFAULT_KNEE));
}

// Loads every band out of the atomic store in one pass, before any DSP runs.
// Snapshotting a band immediately before processing it would hand low, mid and
// high three different parameter epochs whenever the main thread wrote between
// them.
void SiEffectMultibandCompressor::_snapshot_bands(const BlockSettings &p_block, BandSettings r_bands[BAND_COUNT]) const {
	for (int band = 0; band < BAND_COUNT; ++band) {
		const BandParams &src = _params.bands[band];
		BandSettings &out = r_bands[band];

		out.upper_threshold_db = src.upper_threshold.load(std::memory_order_relaxed);
		out.lower_threshold_db = src.lower_threshold.load(std::memory_order_relaxed);
		// The two sections share one envelope, so an inverted pair would boost
		// and cut the same signal at once. Pin the below threshold under the
		// above one.
		//
		// Ordering is all this enforces. With a soft knee the two sections
		// still overlap wherever the gap between the thresholds is narrower
		// than the knee, and at equal thresholds both are already acting on
		// knee/8 dB of excess (0.75 dB at the default 6 dB knee). That overlap
		// is intended: it is what lets the pair be dragged together into one
		// continuous upward-to-downward transfer curve instead of meeting at a
		// corner.
		out.lower_threshold_db = MIN(out.lower_threshold_db, out.upper_threshold_db);
		out.upper_amount = src.upper_amount.load(std::memory_order_relaxed);
		out.lower_amount = src.lower_amount.load(std::memory_order_relaxed);
		out.output_gain_db = src.output_gain.load(std::memory_order_relaxed);
		out.knee_db = p_block.knee_db;
		out.attack = p_block.attack;
		out.release = p_block.release;
		out.mix_start = p_block.mix_start;
		out.mix_delta = p_block.mix_delta;
	}
}

// -----------------------------------------------------------------------------
// Band routing
// -----------------------------------------------------------------------------

void SiEffectMultibandCompressor::_process_multiband(int p_channels, double *p_audio, int p_length, double p_sample_rate, const BandSettings *p_bands) {
	if (_lm_filter.is_null() || _mh_filter.is_null() || _low_compensation_filter.is_null()) {
		return;
	}

	int interleaved = p_length << 1;
	double *low = _low_buffer.ptrw();
	double *mid = _mid_buffer.ptrw();
	double *high = _high_buffer.ptrw();
	double *scratch = _scratch_buffer.ptrw();

	// Split at the low/mid crossover, then split everything above it again.
	_lm_filter->process_split(p_channels, p_audio, low, scratch, p_length);
	_mh_filter->process_split(p_channels, scratch, mid, high, p_length);

	// The mid and high bands have picked up the phase of the mid/high crossover
	// and the low band has not, so the three would not sum flat. Run the low
	// band through a matched splitter and recombine both halves: that is the
	// same allpass, applied to the band that would otherwise skip it. The high
	// half lands back in low in place, which process_split allows because it
	// reads each frame before writing either output.
	_low_compensation_filter->process_split(p_channels, low, scratch, low, p_length);
	for (int i = 0; i < interleaved; ++i) {
		low[i] += scratch[i];
	}

	_low_compressor.process(low, p_length, p_bands[BAND_LOW_INDEX], p_sample_rate);
	_mid_compressor.process(mid, p_length, p_bands[BAND_MID_INDEX], p_sample_rate);
	_high_compressor.process(high, p_length, p_bands[BAND_HIGH_INDEX], p_sample_rate);

	for (int i = 0; i < interleaved; ++i) {
		p_audio[i] = low[i] + mid[i] + high[i];
	}
}

void SiEffectMultibandCompressor::_process_low_band(int p_channels, double *p_audio, int p_length, double p_sample_rate, const BandSettings &p_low) {
	if (_lm_filter.is_null()) {
		return;
	}

	int interleaved = p_length << 1;
	double *low = _low_buffer.ptrw();
	double *rest = _scratch_buffer.ptrw();

	// Compress the low band only. Everything above the split passes through
	// untouched and is summed back, so this narrows what is compressed rather
	// than what is heard. Dry/wet applies to the low band alone, inside the
	// compressor, where it blends against a phase-matched dry.
	_lm_filter->process_split(p_channels, p_audio, low, rest, p_length);
	_low_compressor.process(low, p_length, p_low, p_sample_rate);

	for (int i = 0; i < interleaved; ++i) {
		p_audio[i] = low[i] + rest[i];
	}
}

void SiEffectMultibandCompressor::_process_high_band(int p_channels, double *p_audio, int p_length, double p_sample_rate, const BandSettings &p_high) {
	if (_mh_filter.is_null()) {
		return;
	}

	int interleaved = p_length << 1;
	double *rest = _scratch_buffer.ptrw();
	double *high = _high_buffer.ptrw();

	// Same deal in the other direction: everything below the split is summed
	// back in untouched.
	_mh_filter->process_split(p_channels, p_audio, rest, high, p_length);
	_high_compressor.process(high, p_length, p_high, p_sample_rate);

	for (int i = 0; i < interleaved; ++i) {
		p_audio[i] = rest[i] + high[i];
	}
}

void SiEffectMultibandCompressor::_process_full_range(double *p_audio, int p_length, double p_sample_rate, const BandSettings &p_mid) {
	// No crossover at all; the mid band settings drive the whole signal, and
	// dry/wet blends against the untouched input, so mix 0 is a true bypass.
	_mid_compressor.process(p_audio, p_length, p_mid, p_sample_rate);
}

// -----------------------------------------------------------------------------
// Processing
// -----------------------------------------------------------------------------

int SiEffectMultibandCompressor::prepare_process() {
	_reset_dsp_state();
	_active_mode = -1;
	// Forces a filter retune on the next block, which is also how a changed
	// sampling rate reaches the crossovers.
	_active_sample_rate = 0.0;

	return 2;
}

int SiEffectMultibandCompressor::process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	if (p_length <= 0) {
		return p_channels;
	}

	double sample_rate = _get_sampling_rate();
	if (sample_rate <= 0.0) {
		return p_channels;
	}

	if (_reset_state_requested.exchange(false, std::memory_order_acq_rel)) {
		_reset_dsp_state();
	}

	// Working buffers are sized for the driver maximum in the constructor, so
	// this never fires. Passing the block through untouched beats resizing four
	// vectors on the audio thread if it somehow ever does.
	if (p_length > MAX_BLOCK_FRAMES) {
		return p_channels;
	}

	// Snapshot the atomic parameter store once per block. Nothing below this
	// point reads shared state, so the audio thread never takes a lock.
	int mode = CLAMP((int)Math::round((double)_params.enabled_bands.load(std::memory_order_relaxed)), (int)BAND_MULTIBAND, (int)BAND_FULL_RANGE);
	BlockSettings block;
	block.attack = CLAMP((double)_params.attack.load(std::memory_order_relaxed), 0.0, 1.0);
	block.release = CLAMP((double)_params.release.load(std::memory_order_relaxed), 0.0, 1.0);
	block.knee_db = CLAMP((double)_params.knee_db.load(std::memory_order_relaxed), 0.0, MAX_KNEE);
	double target_mix = CLAMP((double)_params.mix.load(std::memory_order_relaxed), 0.0, 1.0);

	// The splits are independent controls, so order them here rather than
	// trusting the UI: an inverted pair would hand the mid band a negative
	// width and fill it with garbage.
	double lm_frequency = CLAMP((double)_params.lm_frequency.load(std::memory_order_relaxed), MIN_FREQUENCY, MAX_FREQUENCY / MIN_CROSSOVER_RATIO);
	double mh_frequency = CLAMP((double)_params.mh_frequency.load(std::memory_order_relaxed), lm_frequency * MIN_CROSSOVER_RATIO, MAX_FREQUENCY);

	if (lm_frequency != _active_lm_frequency || mh_frequency != _active_mh_frequency || sample_rate != _active_sample_rate) {
		_retune_filters(lm_frequency, mh_frequency);
		_active_lm_frequency = lm_frequency;
		_active_mh_frequency = mh_frequency;
		_active_sample_rate = sample_rate;
	}

	if (mode != _active_mode) {
		_reset_dsp_state();
		_active_mode = mode;
	}

	// A reset leaves nothing to glide from, and a pooled instance would
	// otherwise spend its first block ramping down from whatever mix the
	// previous patch left behind - at 4096 frames and 44.1 kHz, 93 ms of the
	// wrong blend.
	if (!_mix_primed) {
		_mix_current = target_mix;
		_mix_primed = true;
	}
	block.mix_start = _mix_current;
	block.mix_delta = (target_mix - _mix_current) / p_length;
	_mix_current = target_mix;

	// Every band is loaded here, before any of them is processed, so all three
	// run the same parameter epoch and the same dry/wet ramp.
	BandSettings bands[BAND_COUNT];
	_snapshot_bands(block, bands);

	double *audio = r_buffer->ptrw() + (p_start_index << 1);

	switch (mode) {
		case BAND_LOW:
			_process_low_band(p_channels, audio, p_length, sample_rate, bands[BAND_LOW_INDEX]);
			break;
		case BAND_HIGH:
			_process_high_band(p_channels, audio, p_length, sample_rate, bands[BAND_HIGH_INDEX]);
			break;
		case BAND_FULL_RANGE:
			_process_full_range(audio, p_length, sample_rate, bands[BAND_MID_INDEX]);
			break;
		default:
			_process_multiband(p_channels, audio, p_length, sample_rate, bands);
			break;
	}

	return p_channels;
}

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

void SiEffectMultibandCompressor::_reset_dsp_state() {
	_low_compressor.reset();
	_mid_compressor.reset();
	_high_compressor.reset();

	// Not a value: the next block adopts whatever mix is then in force, with no
	// ramp. See process().
	_mix_primed = false;

	// The filters keep their tuning across a reset; only the history clears.
	if (_lm_filter.is_valid()) {
		_lm_filter->reset();
	}
	if (_mh_filter.is_valid()) {
		_mh_filter->reset();
	}
	if (_low_compensation_filter.is_valid()) {
		_low_compensation_filter->reset();
	}
}

void SiEffectMultibandCompressor::_retune_filters(double p_lm_frequency, double p_mh_frequency) {
	if (_lm_filter.is_valid()) {
		_lm_filter->refresh_sampling_rate();
		_lm_filter->set_params(p_lm_frequency, 0);
	}
	if (_mh_filter.is_valid()) {
		_mh_filter->refresh_sampling_rate();
		_mh_filter->set_params(p_mh_frequency, 1);
	}
	if (_low_compensation_filter.is_valid()) {
		_low_compensation_filter->refresh_sampling_rate();
		_low_compensation_filter->set_params(p_mh_frequency, 0);
	}
}

void SiEffectMultibandCompressor::reset() {
	set_params();
	_reset_state_requested.store(true, std::memory_order_release);
}

// -----------------------------------------------------------------------------
// Metering
// -----------------------------------------------------------------------------

const SiEffectMultibandCompressor::BandCompressor *SiEffectMultibandCompressor::_get_band_compressor(int p_band) const {
	switch (p_band) {
		case BAND_LOW_INDEX: return &_low_compressor;
		case BAND_MID_INDEX: return &_mid_compressor;
		case BAND_HIGH_INDEX: return &_high_compressor;
		default: return nullptr;
	}
}

double SiEffectMultibandCompressor::get_band_input_db(int p_band) const {
	const BandCompressor *band = _get_band_compressor(p_band);
	return band ? band->get_input_db() : -120.0;
}

double SiEffectMultibandCompressor::get_band_output_db(int p_band) const {
	const BandCompressor *band = _get_band_compressor(p_band);
	return band ? band->get_output_db() : -120.0;
}

double SiEffectMultibandCompressor::get_band_downward_gain_reduction_db(int p_band) const {
	const BandCompressor *band = _get_band_compressor(p_band);
	return band ? band->get_downward_gain_reduction_db() : 0.0;
}

double SiEffectMultibandCompressor::get_band_upward_gain_db(int p_band) const {
	const BandCompressor *band = _get_band_compressor(p_band);
	return band ? band->get_upward_gain_db() : 0.0;
}

// -----------------------------------------------------------------------------
// Bindings
// -----------------------------------------------------------------------------

void SiEffectMultibandCompressor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_params", "enabled_bands",
								  "low_upper_threshold", "mid_upper_threshold", "high_upper_threshold",
								  "low_lower_threshold", "mid_lower_threshold", "high_lower_threshold",
								  "low_upper_amount", "mid_upper_amount", "high_upper_amount",
								  "low_lower_amount", "mid_lower_amount", "high_lower_amount",
								  "low_output_gain", "mid_output_gain", "high_output_gain",
								  "attack", "release", "mix", "lm_frequency", "mh_frequency", "knee_db"),
			&SiEffectMultibandCompressor::set_params,
			DEFVAL(BAND_MULTIBAND),
			DEFVAL(DEFAULT_LOW_UPPER_THRESHOLD), DEFVAL(DEFAULT_MID_UPPER_THRESHOLD), DEFVAL(DEFAULT_HIGH_UPPER_THRESHOLD),
			DEFVAL(DEFAULT_LOW_LOWER_THRESHOLD), DEFVAL(DEFAULT_MID_LOWER_THRESHOLD), DEFVAL(DEFAULT_HIGH_LOWER_THRESHOLD),
			DEFVAL(DEFAULT_LOW_UPPER_AMOUNT), DEFVAL(DEFAULT_MID_UPPER_AMOUNT), DEFVAL(DEFAULT_HIGH_UPPER_AMOUNT),
			DEFVAL(DEFAULT_LOW_LOWER_AMOUNT), DEFVAL(DEFAULT_MID_LOWER_AMOUNT), DEFVAL(DEFAULT_HIGH_LOWER_AMOUNT),
			DEFVAL(DEFAULT_LOW_OUTPUT_GAIN), DEFVAL(DEFAULT_MID_OUTPUT_GAIN), DEFVAL(DEFAULT_HIGH_OUTPUT_GAIN),
			DEFVAL(DEFAULT_ATTACK), DEFVAL(DEFAULT_RELEASE), DEFVAL(DEFAULT_MIX),
			DEFVAL(DEFAULT_LM_FREQUENCY), DEFVAL(DEFAULT_MH_FREQUENCY), DEFVAL(DEFAULT_KNEE));

	ClassDB::bind_method(D_METHOD("get_band_input_db", "band"), &SiEffectMultibandCompressor::get_band_input_db);
	ClassDB::bind_method(D_METHOD("get_band_output_db", "band"), &SiEffectMultibandCompressor::get_band_output_db);
	ClassDB::bind_method(D_METHOD("get_band_downward_gain_reduction_db", "band"), &SiEffectMultibandCompressor::get_band_downward_gain_reduction_db);
	ClassDB::bind_method(D_METHOD("get_band_upward_gain_db", "band"), &SiEffectMultibandCompressor::get_band_upward_gain_db);
}

SiEffectMultibandCompressor::SiEffectMultibandCompressor() :
		SiEffectBase(),
		_low_compressor(LOW_ATTACK_MS, LOW_RELEASE_MS),
		_mid_compressor(MID_ATTACK_MS, MID_RELEASE_MS),
		_high_compressor(HIGH_ATTACK_MS, HIGH_RELEASE_MS) {
	_lm_filter.instantiate();
	_mh_filter.instantiate();
	_low_compensation_filter.instantiate();

	// Sized up front, for the largest block the driver will ever hand us, so no
	// block allocates on the audio thread.
	const int max_samples = MAX_BLOCK_FRAMES << 1;
	_low_buffer.resize(max_samples);
	_mid_buffer.resize(max_samples);
	_high_buffer.resize(max_samples);
	_scratch_buffer.resize(max_samples);

	set_params();
}
