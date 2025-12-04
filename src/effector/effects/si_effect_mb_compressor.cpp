/* Copyright 2013-2019 Matt Tytel
 *           2021 Yegor Suslin
 *           2025 Refactor
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

 #include "si_effect_mb_compressor.h"
 #include "chip/siopm_ref_table.h"
 #include <mutex>
 
 using namespace godot;
 
 // Helper functions
 static inline double db_to_magnitude(double p_db) {
	 return Math::pow(10.0, p_db / 20.0);
 }
 
 static inline double lerp(double p_a, double p_b, double p_t) {
	 return p_a + (p_b - p_a) * p_t;
 }
 
 // -----------------------------------------------------------------------------
 // Compressor Implementation
 // -----------------------------------------------------------------------------
 
 SiEffectMultibandCompressor::Compressor::Compressor(double p_base_attack_ms_first, double p_base_release_ms_first,
													  double p_base_attack_ms_second, double p_base_release_ms_second) :
		 _base_attack_ms_first(p_base_attack_ms_first),
		 _base_release_ms_first(p_base_release_ms_first),
		 _base_attack_ms_second(p_base_attack_ms_second),
		 _base_release_ms_second(p_base_release_ms_second),
		 _input_mean_squared(0.0),
		 _output_mean_squared(0.0),
		 _high_enveloped_mean_squared(0.0),
		 _low_enveloped_mean_squared(0.0),
		 _output_mult(1.0),
		 _mix(1.0) {
 }
 
 void SiEffectMultibandCompressor::Compressor::reset() {
	 _input_mean_squared = 0.0;
	 _output_mean_squared = 0.0;
	 _high_enveloped_mean_squared = 0.0;
	 _low_enveloped_mean_squared = 0.0;
	 _output_mult = 1.0;
	 _mix = 1.0;
 }
 
 // Optimized RMS calculation for metering only (post-process)
 double SiEffectMultibandCompressor::Compressor::_compute_mean_squared(const double *p_audio_left, const double *p_audio_right, int p_length, double p_mean_squared, int p_sample_rate) {
	 int rms_samples = (int)(RMS_TIME * p_sample_rate);
	 if (rms_samples <= 0) {
		 rms_samples = 1;
	 }
	 double rms_adjusted = rms_samples - 1.0;
	 double input_scale = 1.0 / rms_samples;
 
	 for (int i = 0; i < p_length; ++i) {
		 double sample_left = p_audio_left[i];
		 double sample_right = p_audio_right[i];
		 double sample_squared = sample_left * sample_left + sample_right * sample_right;
		 // Simple IIR RMS follower
		 p_mean_squared = (p_mean_squared * rms_adjusted + sample_squared) * input_scale;
	 }
	 return p_mean_squared;
 }
 
 void SiEffectMultibandCompressor::Compressor::process_band(double *p_audio_left, double *p_audio_right, int p_length,
															double p_upper_threshold_db, double p_lower_threshold_db,
															double p_upper_ratio, double p_lower_ratio,
															double p_output_gain_db, double p_attack, double p_release,
															double p_mix, int p_sample_rate) {
	 SiOPMRefTable *ref_table = SiOPMRefTable::get_instance();
	 if (!ref_table || ref_table->sampling_rate <= 0) {
		 return;
	 }
 
	 // 1. Calculate Envelope Coefficients
	 double samples_per_ms = ref_table->sampling_rate / 1000.0;
	 double attack_mult_first = _base_attack_ms_first * samples_per_ms;
	 double release_mult_first = _base_release_ms_first * samples_per_ms;
	 double attack_mult_second = _base_attack_ms_second * samples_per_ms;
	 double release_mult_second = _base_release_ms_second * samples_per_ms;
 
	 double attack_exponent = CLAMP(p_attack, 0.0, 1.0) * 8.0 - 4.0;
	 double release_exponent = CLAMP(p_release, 0.0, 1.0) * 8.0 - 4.0;
 
	 // Pre-calculate sample counts for attack/release curves
	 double envelope_attack_samples_first = MAX(Math::exp(attack_exponent) * attack_mult_first, MIN_SAMPLE_ENVELOPE);
	 double envelope_release_samples_first = MAX(Math::exp(release_exponent) * release_mult_first, MIN_SAMPLE_ENVELOPE);
	 double envelope_attack_samples_second = MAX(Math::exp(attack_exponent) * attack_mult_second, MIN_SAMPLE_ENVELOPE);
	 double envelope_release_samples_second = MAX(Math::exp(release_exponent) * release_mult_second, MIN_SAMPLE_ENVELOPE);
 
	 double attack_scale_first = 1.0 / (envelope_attack_samples_first + 1.0);
	 double release_scale_first = 1.0 / (envelope_release_samples_first + 1.0);
	 double attack_scale_second = 1.0 / (envelope_attack_samples_second + 1.0);
	 double release_scale_second = 1.0 / (envelope_release_samples_second + 1.0);
 
	 // 2. Prepare Thresholds and Ratios
	 double upper_threshold = CLAMP(p_upper_threshold_db, MIN_THRESHOLD, MAX_THRESHOLD);
	 upper_threshold = db_to_magnitude(upper_threshold);
	 upper_threshold *= upper_threshold; // Work in squared domain
 
	 double lower_threshold = CLAMP(p_lower_threshold_db, MIN_THRESHOLD, MAX_THRESHOLD);
	 lower_threshold = db_to_magnitude(lower_threshold);
	 lower_threshold *= lower_threshold; // Work in squared domain
 
	 double upper_ratio = CLAMP(p_upper_ratio, 0.0, 1.0) * 0.5;
	 double lower_ratio = CLAMP(p_lower_ratio, -1.0, 1.0) * 0.5;
 
	 // Load State
	 double low_enveloped_mean_squared = _low_enveloped_mean_squared;
	 double high_enveloped_mean_squared = _high_enveloped_mean_squared;
 
	 // 3. Prepare Gain Smoothing (De-zippering)
	 double target_output_mult = db_to_magnitude(CLAMP(p_output_gain_db, MIN_GAIN, MAX_GAIN));
	 double target_mix = CLAMP(p_mix, 0.0, 1.0);
	 double delta_output_mult = (target_output_mult - _output_mult) / p_length;
	 double delta_mix = (target_mix - _mix) / p_length;
 
	 // 4. Update Input Metering
	 _input_mean_squared = _compute_mean_squared(p_audio_left, p_audio_right, p_length, _input_mean_squared, p_sample_rate);
 
	 // 5. Process Audio Loop (Compression + Mix + Output Gain)
	 for (int i = 0; i < p_length; ++i) {
		 double dry_left = p_audio_left[i];
		 double dry_right = p_audio_right[i];
		 double sample_squared = dry_left * dry_left + dry_right * dry_right;
 
		 // --- High Threshold (Downward Compression) ---
		 bool high_attack = sample_squared > high_enveloped_mean_squared;
		 double high_samples = high_attack ? envelope_attack_samples_first : envelope_release_samples_first;
		 double high_scale = high_attack ? attack_scale_first : release_scale_first;
 
		 high_enveloped_mean_squared = (sample_squared + high_enveloped_mean_squared * high_samples) * high_scale;
		 high_enveloped_mean_squared = MAX(high_enveloped_mean_squared, upper_threshold);
		 // Safety check to prevent division by zero
		 if (high_enveloped_mean_squared < 1e-10) {
			 high_enveloped_mean_squared = 1e-10;
		 }

		 double upper_mag_delta = upper_threshold / high_enveloped_mean_squared;
		 double upper_mult = Math::pow(upper_mag_delta, upper_ratio);
 
		 // --- Low Threshold (Upward Compression) ---
		 bool low_attack = sample_squared > low_enveloped_mean_squared;
		 double low_samples = low_attack ? envelope_attack_samples_second : envelope_release_samples_second;
		 double low_scale = low_attack ? attack_scale_second : release_scale_second;
 
		 low_enveloped_mean_squared = (sample_squared + low_enveloped_mean_squared * low_samples) * low_scale;
		 low_enveloped_mean_squared = MIN(low_enveloped_mean_squared, lower_threshold);
		 // Safety check to prevent division by zero
		 if (low_enveloped_mean_squared < 1e-10) {
			 low_enveloped_mean_squared = 1e-10;
		 }

		 double lower_mag_delta = lower_threshold / low_enveloped_mean_squared;
		 double lower_mult = Math::pow(lower_mag_delta, lower_ratio);
 
		 // --- Apply Compression Gain ---
		 double gain_compression = CLAMP(upper_mult * lower_mult, 0.0, MAX_EXPAND_MULT);
		 
		 // --- Apply Makeup Gain & Mix ---
		 _output_mult += delta_output_mult;
		 _mix += delta_mix;
 
		 // Apply gain to create wet signal
		 double wet_left = dry_left * gain_compression * _output_mult;
		 double wet_right = dry_right * gain_compression * _output_mult;
 
		 // Mix dry and wet
		 p_audio_left[i] = lerp(dry_left, wet_left, _mix);
		 p_audio_right[i] = lerp(dry_right, wet_right, _mix);
	 }
 
	 // Save State
	 _low_enveloped_mean_squared = low_enveloped_mean_squared;
	 _high_enveloped_mean_squared = high_enveloped_mean_squared;
	 _output_mult = target_output_mult;
	 _mix = target_mix;
 
	 // 6. Update Output Metering
	 _output_mean_squared = _compute_mean_squared(p_audio_left, p_audio_right, p_length, _output_mean_squared, p_sample_rate);
 }
 
 // -----------------------------------------------------------------------------
 // MultibandCompressor Implementation
 // -----------------------------------------------------------------------------
 
 SiEffectMultibandCompressor::SiEffectMultibandCompressor(int p_enabled_bands,
														  double p_low_upper_threshold, double p_band_upper_threshold, double p_high_upper_threshold,
														  double p_low_lower_threshold, double p_band_lower_threshold, double p_high_lower_threshold,
														  double p_low_upper_ratio, double p_band_upper_ratio, double p_high_upper_ratio,
														  double p_low_lower_ratio, double p_band_lower_ratio, double p_high_lower_ratio,
														  double p_low_output_gain, double p_band_output_gain, double p_high_output_gain,
														  double p_attack, double p_release, double p_mix,
														  double p_lm_frequency, double p_mh_frequency) :
		 SiEffectBase(),
		 _low_band_compressor(LOW_ATTACK_MS, LOW_RELEASE_MS, BAND_ATTACK_MS, BAND_RELEASE_MS),
		 _band_high_compressor(BAND_ATTACK_MS, BAND_RELEASE_MS, HIGH_ATTACK_MS, HIGH_RELEASE_MS) {
	 
	 // Instantiate separate filters to avoid state corruption during crossover splitting
	 _lm_low_filter.instantiate();
	 _lm_high_filter.instantiate();
	 _mh_low_filter.instantiate();
	 _mh_high_filter.instantiate();

	 // Initialize filters with default frequencies (will be updated by set_params)
	 // This ensures filters are valid before first use
	 double lm_freq = CLAMP(p_lm_frequency, 20.0, 20000.0);
	 double mh_freq = CLAMP(p_mh_frequency, 20.0, 20000.0);
	 
	 if (_lm_low_filter.is_valid()) _lm_low_filter->set_params(lm_freq, 0);
	 if (_lm_high_filter.is_valid()) _lm_high_filter->set_params(lm_freq, 1);
	 if (_mh_low_filter.is_valid()) _mh_low_filter->set_params(mh_freq, 0);
	 if (_mh_high_filter.is_valid()) _mh_high_filter->set_params(mh_freq, 1);

	 set_params(p_enabled_bands,
				p_low_upper_threshold, p_band_upper_threshold, p_high_upper_threshold,
				p_low_lower_threshold, p_band_lower_threshold, p_high_lower_threshold,
				p_low_upper_ratio, p_band_upper_ratio, p_high_upper_ratio,
				p_low_lower_ratio, p_band_lower_ratio, p_high_lower_ratio,
				p_low_output_gain, p_band_output_gain, p_high_output_gain,
				p_attack, p_release, p_mix,
				p_lm_frequency, p_mh_frequency);
 }
 
 void SiEffectMultibandCompressor::set_params(int p_enabled_bands,
											   double p_low_upper_threshold, double p_band_upper_threshold, double p_high_upper_threshold,
											   double p_low_lower_threshold, double p_band_lower_threshold, double p_high_lower_threshold,
											   double p_low_upper_ratio, double p_band_upper_ratio, double p_high_upper_ratio,
											   double p_low_lower_ratio, double p_band_lower_ratio, double p_high_lower_ratio,
											   double p_low_output_gain, double p_band_output_gain, double p_high_output_gain,
											   double p_attack, double p_release, double p_mix,
											   double p_lm_frequency, double p_mh_frequency) {
	 std::lock_guard<std::mutex> guard(_state_mutex);

	 _enabled_bands = CLAMP(p_enabled_bands, BAND_MULTIBAND, BAND_SINGLE);
	 _low_upper_threshold = CLAMP(p_low_upper_threshold, MIN_THRESHOLD, MAX_THRESHOLD);
	 _band_upper_threshold = CLAMP(p_band_upper_threshold, MIN_THRESHOLD, MAX_THRESHOLD);
	 _high_upper_threshold = CLAMP(p_high_upper_threshold, MIN_THRESHOLD, MAX_THRESHOLD);
	 _low_lower_threshold = CLAMP(p_low_lower_threshold, MIN_THRESHOLD, MAX_THRESHOLD);
	 _band_lower_threshold = CLAMP(p_band_lower_threshold, MIN_THRESHOLD, MAX_THRESHOLD);
	 _high_lower_threshold = CLAMP(p_high_lower_threshold, MIN_THRESHOLD, MAX_THRESHOLD);
	 _low_upper_ratio = CLAMP(p_low_upper_ratio, 0.0, 1.0);
	 _band_upper_ratio = CLAMP(p_band_upper_ratio, 0.0, 1.0);
	 _high_upper_ratio = CLAMP(p_high_upper_ratio, 0.0, 1.0);
	 _low_lower_ratio = CLAMP(p_low_lower_ratio, -1.0, 1.0);
	 _band_lower_ratio = CLAMP(p_band_lower_ratio, -1.0, 1.0);
	 _high_lower_ratio = CLAMP(p_high_lower_ratio, -1.0, 1.0);
	 _low_output_gain = CLAMP(p_low_output_gain, MIN_GAIN, MAX_GAIN);
	 _band_output_gain = CLAMP(p_band_output_gain, MIN_GAIN, MAX_GAIN);
	 _high_output_gain = CLAMP(p_high_output_gain, MIN_GAIN, MAX_GAIN);
	 _attack = CLAMP(p_attack, 0.0, 1.0);
	 _release = CLAMP(p_release, 0.0, 1.0);
	 _mix = CLAMP(p_mix, 0.0, 1.0);
	 _lm_frequency = CLAMP(p_lm_frequency, 20.0, 20000.0);
	 _mh_frequency = CLAMP(p_mh_frequency, 20.0, 20000.0);
 
	 // Update params for all filter instances
	 if (_lm_low_filter.is_valid()) _lm_low_filter->set_params(_lm_frequency, 0); // Low
	 if (_lm_high_filter.is_valid()) _lm_high_filter->set_params(_lm_frequency, 1); // High
	 
	 if (_mh_low_filter.is_valid()) _mh_low_filter->set_params(_mh_frequency, 0); // Low (Mid)
	 if (_mh_high_filter.is_valid()) _mh_high_filter->set_params(_mh_frequency, 1); // High
 }
 
 int SiEffectMultibandCompressor::prepare_process() {
	 return 2; // Stereo output
 }
 
 // Ensure working buffers are large enough (prevents allocation on audio thread)
 void SiEffectMultibandCompressor::_ensure_buffer_size(int p_frames) {
	 int required_size = p_frames * 2; // Stereo
	 if (_temp_buffer.size() < required_size) _temp_buffer.resize(required_size);
	 if (_low_buffer.size() < required_size) _low_buffer.resize(required_size);
	 if (_band_buffer.size() < required_size) _band_buffer.resize(required_size);
	 if (_high_buffer.size() < required_size) _high_buffer.resize(required_size);
	 
	 if (_left_vec.size() < p_frames) _left_vec.resize(p_frames);
	 if (_right_vec.size() < p_frames) _right_vec.resize(p_frames);
 }
 
 void SiEffectMultibandCompressor::_process_multiband(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	 SiOPMRefTable *ref_table = SiOPMRefTable::get_instance();
	 if (!ref_table || ref_table->sampling_rate <= 0) return;
 
	 _ensure_buffer_size(p_length);
 
	 int start_idx = p_start_index << 1;
	 int length = p_length << 1;
 
	 // Copy input to temp buffer
	 const double *input_ptr = r_buffer->ptr() + start_idx;
	 double *temp_ptr = _temp_buffer.ptrw();
	 for (int i = 0; i < length; ++i) {
		 temp_ptr[i] = input_ptr[i];
	 }
 
	 // --- Crossover Network ---
	 // Filters should already have correct parameters set via set_params()
	 // No need to call set_params during audio processing
	 
	 // 1. Split Input at LM Frequency
	 // Filter A: Input -> Low Band (written to _low_buffer)
	 double *low_ptr = _low_buffer.ptrw();
	 for(int i=0; i<length; ++i) low_ptr[i] = temp_ptr[i];
	 if (_lm_low_filter.is_valid()) {
		 _lm_low_filter->process(p_channels, &_low_buffer, 0, p_length);
	 }

	 // Filter B: Input -> High intermediate (written to _band_buffer temporarily)
	 double *band_ptr = _band_buffer.ptrw();
	 for(int i=0; i<length; ++i) band_ptr[i] = temp_ptr[i];
	 if (_lm_high_filter.is_valid()) {
		 _lm_high_filter->process(p_channels, &_band_buffer, 0, p_length);
	 }

	 // 2. Split High Intermediate at MH Frequency
	 // Filter C: High Int -> Mid Band (stays in _band_buffer)
	 // We need to save High Int for Filter D first
	 double *high_ptr = _high_buffer.ptrw();
	 for(int i=0; i<length; ++i) high_ptr[i] = band_ptr[i];

	 if (_mh_low_filter.is_valid()) {
		 _mh_low_filter->process(p_channels, &_band_buffer, 0, p_length); // _band_buffer now contains Mid
	 }

	 // Filter D: High Int -> High Band
	 if (_mh_high_filter.is_valid()) {
		 _mh_high_filter->process(p_channels, &_high_buffer, 0, p_length); // _high_buffer now contains High
	 }
 
	 // --- Band Processing ---
	 
	 // Combine Low + Mid for the "Low Band Compressor" (Intentional 2-band structure)
	 for (int i = 0; i < length; ++i) {
		 low_ptr[i] += band_ptr[i];
	 }
 
	 // Prepare vectors for compressor (Interleaved -> Planar)
	 double *l_vec = _left_vec.ptrw();
	 double *r_vec = _right_vec.ptrw();
 
	 // Process Low Band (Low + Mid)
	 for (int i = 0; i < p_length; ++i) {
		 l_vec[i] = low_ptr[i * 2];
		 r_vec[i] = low_ptr[i * 2 + 1];
	 }
	 
	 _low_band_compressor.process_band(l_vec, r_vec, p_length,
									   _band_upper_threshold, _band_lower_threshold,
									   _band_upper_ratio, _band_lower_ratio,
									   _band_output_gain, _attack, _release, _mix, ref_table->sampling_rate);
 
	 for (int i = 0; i < p_length; ++i) {
		 low_ptr[i * 2] = l_vec[i];
		 low_ptr[i * 2 + 1] = r_vec[i];
	 }
 
	 // Process High Band
	 for (int i = 0; i < p_length; ++i) {
		 l_vec[i] = high_ptr[i * 2];
		 r_vec[i] = high_ptr[i * 2 + 1];
	 }
 
	 _band_high_compressor.process_band(l_vec, r_vec, p_length,
										_high_upper_threshold, _high_lower_threshold,
										_high_upper_ratio, _high_lower_ratio,
										_high_output_gain, _attack, _release, _mix, ref_table->sampling_rate);
 
	 for (int i = 0; i < p_length; ++i) {
		 high_ptr[i * 2] = l_vec[i];
		 high_ptr[i * 2 + 1] = r_vec[i];
	 }
 
	 // Sum bands back to output buffer
	 double *out_ptr = r_buffer->ptrw() + start_idx;
	 for (int i = 0; i < length; ++i) {
		 out_ptr[i] = low_ptr[i] + high_ptr[i];
	 }
 }
 
 void SiEffectMultibandCompressor::_process_low_band(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	 SiOPMRefTable *ref_table = SiOPMRefTable::get_instance();
	 if (!ref_table || ref_table->sampling_rate <= 0) return;
 
	 _ensure_buffer_size(p_length);
	 int start_idx = p_start_index << 1;
	 int length = p_length << 1;
 
	 // Copy input to temp
	 const double *in_ptr = r_buffer->ptr() + start_idx;
	 double *temp_ptr = _temp_buffer.ptrw();
	 for (int i = 0; i < length; ++i) temp_ptr[i] = in_ptr[i];

	 // Filter LPF at LM (filter should already have correct params)
	 if (_lm_low_filter.is_valid()) {
		 _lm_low_filter->process(p_channels, &_temp_buffer, 0, p_length);
	 }
 
	 // Planar Split
	 double *l_vec = _left_vec.ptrw();
	 double *r_vec = _right_vec.ptrw();
	 for (int i = 0; i < p_length; ++i) {
		 l_vec[i] = temp_ptr[i * 2];
		 r_vec[i] = temp_ptr[i * 2 + 1];
	 }
 
	 _low_band_compressor.process_band(l_vec, r_vec, p_length,
									   _low_upper_threshold, _low_lower_threshold,
									   _low_upper_ratio, _low_lower_ratio,
									   _low_output_gain, _attack, _release, _mix, ref_table->sampling_rate);
 
	 // Interleave back
	 double *out_ptr = r_buffer->ptrw() + start_idx;
	 for (int i = 0; i < p_length; ++i) {
		 out_ptr[i * 2] = l_vec[i];
		 out_ptr[i * 2 + 1] = r_vec[i];
	 }
 }
 
 void SiEffectMultibandCompressor::_process_high_band(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	 SiOPMRefTable *ref_table = SiOPMRefTable::get_instance();
	 if (!ref_table || ref_table->sampling_rate <= 0) return;
 
	 _ensure_buffer_size(p_length);
	 int start_idx = p_start_index << 1;
	 int length = p_length << 1;
 
	 // Copy input to temp
	 const double *in_ptr = r_buffer->ptr() + start_idx;
	 double *temp_ptr = _temp_buffer.ptrw();
	 for (int i = 0; i < length; ++i) temp_ptr[i] = in_ptr[i];

	 // Filter HPF at MH (filter should already have correct params)
	 if (_mh_high_filter.is_valid()) {
		 _mh_high_filter->process(p_channels, &_temp_buffer, 0, p_length);
	 }
 
	 double *l_vec = _left_vec.ptrw();
	 double *r_vec = _right_vec.ptrw();
	 for (int i = 0; i < p_length; ++i) {
		 l_vec[i] = temp_ptr[i * 2];
		 r_vec[i] = temp_ptr[i * 2 + 1];
	 }
 
	 _band_high_compressor.process_band(l_vec, r_vec, p_length,
										 _high_upper_threshold, _high_lower_threshold,
										 _high_upper_ratio, _high_lower_ratio,
										 _high_output_gain, _attack, _release, _mix, ref_table->sampling_rate);
 
	 double *out_ptr = r_buffer->ptrw() + start_idx;
	 for (int i = 0; i < p_length; ++i) {
		 out_ptr[i * 2] = l_vec[i];
		 out_ptr[i * 2 + 1] = r_vec[i];
	 }
 }
 
 void SiEffectMultibandCompressor::_process_single_band(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	 SiOPMRefTable *ref_table = SiOPMRefTable::get_instance();
	 if (!ref_table || ref_table->sampling_rate <= 0) return;
	 
	 _ensure_buffer_size(p_length);
	 int start_idx = p_start_index << 1;
 
	 double *audio_in = r_buffer->ptrw() + start_idx;
	 double *l_vec = _left_vec.ptrw();
	 double *r_vec = _right_vec.ptrw();
 
	 for (int i = 0; i < p_length; ++i) {
		 l_vec[i] = audio_in[i * 2];
		 r_vec[i] = audio_in[i * 2 + 1];
	 }
 
	 _band_high_compressor.process_band(l_vec, r_vec, p_length,
										 _band_upper_threshold, _band_lower_threshold,
										 _band_upper_ratio, _band_lower_ratio,
										 _band_output_gain, _attack, _release, _mix, ref_table->sampling_rate);
 
	 for (int i = 0; i < p_length; ++i) {
		 audio_in[i * 2] = l_vec[i];
		 audio_in[i * 2 + 1] = r_vec[i];
	 }
 }
 
 int SiEffectMultibandCompressor::process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	 std::lock_guard<std::mutex> guard(_state_mutex);
 
	 bool low_enabled = _enabled_bands == BAND_MULTIBAND || _enabled_bands == BAND_LOW;
	 bool high_enabled = _enabled_bands == BAND_MULTIBAND || _enabled_bands == BAND_HIGH;
 
	 if (low_enabled != _was_low_enabled || high_enabled != _was_high_enabled) {
		 _low_band_compressor.reset();
		 _band_high_compressor.reset();
		 
		 if (_lm_low_filter.is_valid()) _lm_low_filter->reset();
		 if (_lm_high_filter.is_valid()) _lm_high_filter->reset();
		 if (_mh_low_filter.is_valid()) _mh_low_filter->reset();
		 if (_mh_high_filter.is_valid()) _mh_high_filter->reset();
 
		 _was_low_enabled = low_enabled;
		 _was_high_enabled = high_enabled;
	 }
 
	 if (low_enabled && high_enabled) {
		 _process_multiband(p_channels, r_buffer, p_start_index, p_length);
	 } else if (low_enabled) {
		 _process_low_band(p_channels, r_buffer, p_start_index, p_length);
	 } else if (high_enabled) {
		 _process_high_band(p_channels, r_buffer, p_start_index, p_length);
	 } else {
		 _process_single_band(p_channels, r_buffer, p_start_index, p_length);
	 }
 
	 return p_channels;
 }
 
 void SiEffectMultibandCompressor::set_by_mml(Vector<double> p_args) {
	 int enabled_bands = (int)_get_mml_arg(p_args, 0, BAND_MULTIBAND);
	 double low_upper_threshold = _get_mml_arg(p_args, 1, -12.0);
	 double band_upper_threshold = _get_mml_arg(p_args, 2, -12.0);
	 double high_upper_threshold = _get_mml_arg(p_args, 3, -12.0);
	 double low_lower_threshold = _get_mml_arg(p_args, 4, -35.0);
	 double band_lower_threshold = _get_mml_arg(p_args, 5, -35.0);
	 double high_lower_threshold = _get_mml_arg(p_args, 6, -35.0);
	 double low_upper_ratio = _get_mml_arg(p_args, 7, 0.85);
	 double band_upper_ratio = _get_mml_arg(p_args, 8, 0.85);
	 double high_upper_ratio = _get_mml_arg(p_args, 9, 0.85);
	 double low_lower_ratio = _get_mml_arg(p_args, 10, 0.7);
	 double band_lower_ratio = _get_mml_arg(p_args, 11, 0.7);
	 double high_lower_ratio = _get_mml_arg(p_args, 12, 0.7);
	 double low_output_gain = _get_mml_arg(p_args, 13, 5.0);
	 double band_output_gain = _get_mml_arg(p_args, 14, 5.0);
	 double high_output_gain = _get_mml_arg(p_args, 15, 5.0);
	 double attack = _get_mml_arg(p_args, 16, 0.25);
	 double release = _get_mml_arg(p_args, 17, 0.25);
	 double mix = _get_mml_arg(p_args, 18, 1.0);
	 double lm_frequency = _get_mml_arg(p_args, 19, 120.0);
	 double mh_frequency = _get_mml_arg(p_args, 20, 2500.0);
 
	 set_params(enabled_bands,
				low_upper_threshold, band_upper_threshold, high_upper_threshold,
				low_lower_threshold, band_lower_threshold, high_lower_threshold,
				low_upper_ratio, band_upper_ratio, high_upper_ratio,
				low_lower_ratio, band_lower_ratio, high_lower_ratio,
				low_output_gain, band_output_gain, high_output_gain,
				attack, release, mix,
				lm_frequency, mh_frequency);
 }
 
 void SiEffectMultibandCompressor::reset() {
	 std::lock_guard<std::mutex> guard(_state_mutex);
 
	 _low_band_compressor.reset();
	 _band_high_compressor.reset();
	 
	 if (_lm_low_filter.is_valid()) _lm_low_filter->reset();
	 if (_lm_high_filter.is_valid()) _lm_high_filter->reset();
	 if (_mh_low_filter.is_valid()) _mh_low_filter->reset();
	 if (_mh_high_filter.is_valid()) _mh_high_filter->reset();
 
	 _was_low_enabled = false;
	 _was_high_enabled = false;
 }
 
 void SiEffectMultibandCompressor::_bind_methods() {
	 ClassDB::bind_method(D_METHOD("set_params", "enabled_bands", "low_upper_threshold", "band_upper_threshold", "high_upper_threshold",
									"low_lower_threshold", "band_lower_threshold", "high_lower_threshold",
									"low_upper_ratio", "band_upper_ratio", "high_upper_ratio",
									"low_lower_ratio", "band_lower_ratio", "high_lower_ratio",
									"low_output_gain", "band_output_gain", "high_output_gain",
									"attack", "release", "mix", "lm_frequency", "mh_frequency"),
						  &SiEffectMultibandCompressor::set_params,
						  DEFVAL(BAND_MULTIBAND), DEFVAL(-12.0), DEFVAL(-12.0), DEFVAL(-12.0),
						  DEFVAL(-35.0), DEFVAL(-35.0), DEFVAL(-35.0),
						  DEFVAL(0.85), DEFVAL(0.85), DEFVAL(0.85),
						  DEFVAL(0.7), DEFVAL(0.7), DEFVAL(0.7),
						  DEFVAL(5.0), DEFVAL(5.0), DEFVAL(5.0),
						  DEFVAL(0.25), DEFVAL(0.25), DEFVAL(1.0),
						  DEFVAL(120.0), DEFVAL(2500.0));
 }