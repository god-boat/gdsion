/* Copyright 2013-2019 Matt Tytel
 *           2021 Yegor Suslin
 *           2025 Refactor
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

 #ifndef SI_EFFECT_MB_COMPRESSOR_H
 #define SI_EFFECT_MB_COMPRESSOR_H
 
 #include "effector/si_effect_base.h"
 #include "si_effect_linkwitz_riley_filter.h"
 #include <godot_cpp/templates/vector.hpp>
 #include <mutex>
 
 using namespace godot;
 
 class SiEffectMultibandCompressor : public SiEffectBase {
	 GDCLASS(SiEffectMultibandCompressor, SiEffectBase)
 
	 // Internal compressor class for processing individual bands
	 class Compressor {
	 public:
		 Compressor(double p_base_attack_ms_first, double p_base_release_ms_first,
					double p_base_attack_ms_second, double p_base_release_ms_second);
 
		 void process_band(double *p_audio_left, double *p_audio_right, int p_length,
						  double p_upper_threshold_db, double p_lower_threshold_db,
						  double p_upper_ratio, double p_lower_ratio,
						  double p_output_gain_db, double p_attack, double p_release,
						  double p_mix, int p_sample_rate);
 
		 double get_input_mean_squared() const { return _input_mean_squared; }
		 double get_output_mean_squared() const { return _output_mean_squared; }
 
		 void reset();
 
	 private:
		 double _base_attack_ms_first;
		 double _base_release_ms_first;
		 double _base_attack_ms_second;
		 double _base_release_ms_second;
 
		 double _input_mean_squared;
		 double _output_mean_squared;
		 double _high_enveloped_mean_squared;
		 double _low_enveloped_mean_squared;
 
		 double _output_mult;
		 double _mix;
 
		 double _compute_mean_squared(const double *p_audio_left, const double *p_audio_right, int p_length, double p_mean_squared, int p_sample_rate);
	 };
 
	 // Constants
	 static constexpr double RMS_TIME = 0.025;
	 static constexpr double MAX_EXPAND_MULT = 32.0;
	 static constexpr double LOW_ATTACK_MS = 2.8;
	 static constexpr double BAND_ATTACK_MS = 1.4;
	 static constexpr double HIGH_ATTACK_MS = 0.7;
	 static constexpr double LOW_RELEASE_MS = 40.0;
	 static constexpr double BAND_RELEASE_MS = 28.0;
	 static constexpr double HIGH_RELEASE_MS = 15.0;
 
	 static constexpr double MIN_GAIN = -30.0;
	 static constexpr double MAX_GAIN = 30.0;
	 static constexpr double MIN_THRESHOLD = -100.0;
	 static constexpr double MAX_THRESHOLD = 12.0;
	 static constexpr double MIN_SAMPLE_ENVELOPE = 5.0;
 
	 enum BandOptions {
		 BAND_MULTIBAND = 0,
		 BAND_LOW = 1,
		 BAND_HIGH = 2,
		 BAND_SINGLE = 3
	 };
 
	 mutable std::mutex _state_mutex;
 
	 // Band enable state
	 int _enabled_bands = BAND_MULTIBAND;
	 bool _was_low_enabled = false;
	 bool _was_high_enabled = false;
 
	 // Filter instances
	 // Updated: Separate instances for Low/High paths at both split points to preserve state
	 Ref<SiEffectLinkwitzRileyFilter> _lm_low_filter;
	 Ref<SiEffectLinkwitzRileyFilter> _lm_high_filter;
	 Ref<SiEffectLinkwitzRileyFilter> _mh_low_filter;
	 Ref<SiEffectLinkwitzRileyFilter> _mh_high_filter;
 
	 // Compressor instances
	 Compressor _low_band_compressor;
	 Compressor _band_high_compressor;
 
	 // Parameters
	 double _low_upper_threshold = -12.0;
	 double _band_upper_threshold = -12.0;
	 double _high_upper_threshold = -12.0;
	 double _low_lower_threshold = -35.0;
	 double _band_lower_threshold = -35.0;
	 double _high_lower_threshold = -35.0;
	 double _low_upper_ratio = 0.85;
	 double _band_upper_ratio = 0.85;
	 double _high_upper_ratio = 0.85;
	 double _low_lower_ratio = 0.7;
	 double _band_lower_ratio = 0.7;
	 double _high_lower_ratio = 0.7;
	 double _low_output_gain = 5.0;
	 double _band_output_gain = 5.0;
	 double _high_output_gain = 5.0;
	 double _attack = 0.25;
	 double _release = 0.25;
	 double _mix = 1.0;
	 double _lm_frequency = 120.0;
	 double _mh_frequency = 2500.0;
 
	 // Temporary buffers for processing
	 Vector<double> _temp_buffer;
	 Vector<double> _low_buffer;
	 Vector<double> _band_buffer;
	 Vector<double> _high_buffer;
	 
	 // Updated: Planar scratch buffers for compressor processing
	 Vector<double> _left_vec;
	 Vector<double> _right_vec;
 
	 // Updated: Helper to prevent re-allocation on audio thread
	 void _ensure_buffer_size(int p_frames);
 
	 void _process_multiband(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length);
	 void _process_low_band(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length);
	 void _process_high_band(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length);
	 void _process_single_band(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length);
 
 protected:
	 static void _bind_methods();
 
 public:
	 void set_params(int p_enabled_bands = BAND_MULTIBAND,
					 double p_low_upper_threshold = -12.0, double p_band_upper_threshold = -12.0, double p_high_upper_threshold = -12.0,
					 double p_low_lower_threshold = -35.0, double p_band_lower_threshold = -35.0, double p_high_lower_threshold = -35.0,
					 double p_low_upper_ratio = 0.85, double p_band_upper_ratio = 0.85, double p_high_upper_ratio = 0.85,
					 double p_low_lower_ratio = 0.7, double p_band_lower_ratio = 0.7, double p_high_lower_ratio = 0.7,
					 double p_low_output_gain = 5.0, double p_band_output_gain = 5.0, double p_high_output_gain = 5.0,
					 double p_attack = 0.25, double p_release = 0.25, double p_mix = 1.0,
					 double p_lm_frequency = 120.0, double p_mh_frequency = 2500.0);
 
	 virtual int prepare_process() override;
	 virtual int process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;
 
	 virtual void set_by_mml(Vector<double> p_args) override;
	 virtual void reset() override;
 
	 SiEffectMultibandCompressor(int p_enabled_bands = BAND_MULTIBAND,
								 double p_low_upper_threshold = -12.0, double p_band_upper_threshold = -12.0, double p_high_upper_threshold = -12.0,
								 double p_low_lower_threshold = -35.0, double p_band_lower_threshold = -35.0, double p_high_lower_threshold = -35.0,
								 double p_low_upper_ratio = 0.85, double p_band_upper_ratio = 0.85, double p_high_upper_ratio = 0.85,
								 double p_low_lower_ratio = 0.7, double p_band_lower_ratio = 0.7, double p_high_lower_ratio = 0.7,
								 double p_low_output_gain = 5.0, double p_band_output_gain = 5.0, double p_high_output_gain = 5.0,
								 double p_attack = 0.25, double p_release = 0.25, double p_mix = 1.0,
								 double p_lm_frequency = 120.0, double p_mh_frequency = 2500.0);
	 ~SiEffectMultibandCompressor() {}
 };
 
 #endif // SI_EFFECT_MB_COMPRESSOR_H