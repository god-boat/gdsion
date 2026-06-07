/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SIOPM_CHANNEL_KS_H
#define SIOPM_CHANNEL_KS_H

#include <godot_cpp/templates/vector.hpp>
#include "chip/channels/siopm_channel_fm.h"
#include "templates/singly_linked_list.h"

using namespace godot;

enum SiONPitchTableType : unsigned int;
class SiOPMSoundChip;

// Karplus-Strong resonator synth with exciter, loop filter, inharmonicity,
// body resonance, pitch behavior, and release mode sections.
class SiOPMChannelKS : public SiOPMChannelFM {
	GDCLASS(SiOPMChannelKS, SiOPMChannelFM)

	static const int KS_BUFFER_SIZE = 5400;

public:
	enum KSSeedType {
		KS_SEED_DEFAULT = 0,
		KS_SEED_FM = 1,
		KS_SEED_PCM = 2,
	};

	enum ExciterType {
		EXCITER_NOISE = 0,
		EXCITER_PULSE = 1,
		EXCITER_CLICK = 2,
		EXCITER_FM = 3,
		EXCITER_PCM = 4,
		EXCITER_BURST = 5,
		EXCITER_IMPULSE = 6,
		EXCITER_MAX
	};

	enum LoopFilterMode {
		LOOP_DARK = 0,
		LOOP_BRIGHT = 1,
		LOOP_BAND = 2,
		LOOP_NOTCH = 3,
		LOOP_COMB = 4,
		LOOP_METALLIC = 5,
		LOOP_DIFFUSED = 6,
		LOOP_MAX
	};

	enum BodyType {
		BODY_NONE = 0,
		BODY_WOOD = 1,
		BODY_GLASS = 2,
		BODY_METAL = 3,
		BODY_PLASTIC = 4,
		BODY_TUBE = 5,
		BODY_BOX = 6,
		BODY_TINY_SPEAKER = 7,
		BODY_MAX
	};

	enum ReleaseMode {
		RELEASE_NATURAL = 0,
		RELEASE_PALM_MUTE = 1,
		RELEASE_CHOKE = 2,
		RELEASE_FREEZE = 3,
		RELEASE_BLOOM = 4,
		RELEASE_MAX
	};

private:
	KSSeedType _ks_seed_type = KS_SEED_DEFAULT;
	int _ks_seed_index = 0;

	Vector<int> _ks_delay_buffer;
	double _ks_delay_buffer_index = 0;
	int _ks_pitch_index = 0;

	double _ks_decay_lpf = 0.875;
	double _ks_decay = 0.98;
	double _ks_mute_decay_lpf = 0.5;
	double _ks_mute_decay = 0.75;
	int _ks_tension = 8;

	double _output = 0;
	double _decay_lpf = 0.5;
	double _decay = 0.75;
	double _expression = 1;

	// --- Exciter section ---
	ExciterType _exciter_type = EXCITER_NOISE;
	double _exciter_color = 0.5;
	double _exciter_length = 0.5;
	double _exciter_shape = 0.5;
	double _exciter_drive = 0.0;
	double _exciter_pitch_follow = 1.0;
	double _exciter_randomness = 0.0;
	uint32_t _exciter_rng_state = 12345;

	// --- Loop filter section ---
	LoopFilterMode _loop_filter_mode = LOOP_DARK;
	double _loop_damping = 0.5;
	double _loop_brightness = 0.5;
	double _loop_loss = 0.02;
	double _loop_tone_tilt = 0.0;

	double _loop_ap_coef = 0.0;
	double _loop_ap_z1 = 0.0;
	double _loop_shelf_coef = 0.0;
	double _loop_shelf_z1 = 0.0;
	double _loop_notch_freq = 0.5;
	double _loop_notch_z1 = 0.0;
	double _loop_notch_z2 = 0.0;
	double _loop_comb_z1 = 0.0;
	int _loop_comb_delay = 3;
	Vector<double> _loop_comb_buffer;
	int _loop_comb_write_pos = 0;

	// --- Inharmonicity / stiffness ---
	double _stiffness = 0.0;
	double _dispersion = 0.0;
	double _bend = 0.0;
	double _odd_even_balance = 0.5;

	double _allpass_coef = 0.0;
	double _allpass_z1 = 0.0;
	double _allpass2_coef = 0.0;
	double _allpass2_z1 = 0.0;

	// --- Body / cavity resonance ---
	BodyType _body_type = BODY_NONE;
	double _body_amount = 0.0;
	double _body_tune = 0.5;
	double _body_width = 0.5;

	struct BodyResonator {
		double b0 = 0, a1 = 0, a2 = 0, gain = 0;
		double z1 = 0, z2 = 0;
		void init(double p_freq, double p_q, double p_gain, double p_sample_rate);
		double process(double p_in);
		void reset();
	};
	static const int BODY_RESONATOR_COUNT = 3;
	BodyResonator _body_resonators[BODY_RESONATOR_COUNT];

	// --- Pitch behavior ---
	double _pitch_drift = 0.0;
	double _pitch_drop = 0.0;
	double _pick_bend = 0.0;
	double _tension_mod = 0.0;
	double _pitch_keytrack = 1.0;
	double _pitch_glide = 0.0;

	double _pitch_drop_phase = 0.0;
	double _pick_bend_phase = 0.0;
	double _drift_phase = 0.0;
	double _drift_lfo = 0.0;
	double _glide_current = 0.0;
	double _glide_target = 0.0;
	double _glide_rate = 0.0;
	bool _is_gliding = false;

	// --- Release mode ---
	ReleaseMode _release_mode = RELEASE_NATURAL;
	bool _is_note_held = false;
	double _bloom_timer = 0.0;
	double _freeze_factor = 0.0;

	// Internal helpers.
	void _fill_excitation(int *p_buffer, int p_length, double p_frequency);
	double _apply_loop_filter_sample(double p_input);
	double _apply_inharmonicity(double p_input);
	double _apply_body_resonance(double p_input);
	void _update_pitch_modifiers(double &r_wave_length_mod);
	void _configure_loop_filter();
	void _configure_body_resonators(double p_sample_rate);
	void _configure_stiffness();
	uint32_t _exciter_rand();
	double _exciter_rand_bipolar();

	// LFO control.
	virtual void _set_lfo_state(bool p_enabled) override;

	// Processing.
	void _apply_karplus_strong(SinglyLinkedList<int>::Element *p_buffer_start, int p_length);

protected:
	static void _bind_methods() {}

	String _to_string() const;

public:
	void set_karplus_strong_params(int p_attack_rate = 48, int p_decay_rate = 48, int p_total_level = 0, int p_fixed_pitch = 0, int p_wave_shape = -1, int p_tension = 8);
	void apply_ks_runtime_params(int p_attack_rate, int p_decay_rate, int p_total_level, int p_fixed_pitch, int p_wave_shape, int p_tension);
	void apply_voice_params(const Ref<class SiOPMChannelParams> &p_params, const Ref<class SiOPMWaveBase> &p_wave_data, int p_tension);
	virtual void set_channel_params(const Ref<class SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation = true) override;

	virtual void set_parameters(Vector<int> p_params) override;
	virtual void set_types(int p_pg_type, SiONPitchTableType p_pt_type) override;
	virtual void set_all_attack_rate(int p_value) override;
	virtual void set_all_release_rate(int p_value) override;

	virtual int get_pitch() const override { return _ks_pitch_index; }
	virtual void set_pitch(int p_value) override { _ks_pitch_index = p_value; }

	virtual void set_release_rate(int p_value) override;
	virtual void set_fixed_pitch(int p_value) override;

	// Aggregate setter for the extended resonator params. Takes the raw nominal
	// ranges stored by the voice/mailbox (unipolar 0-100, bipolar 0-100 centered
	// at 50) and performs the normalization internally, so call sites don't.
	void set_ks_extended_params(
			int p_exciter_type, int p_exciter_color, int p_exciter_length,
			int p_exciter_shape, int p_exciter_drive, int p_exciter_pitch_follow, int p_exciter_randomness,
			int p_loop_filter_mode, int p_loop_damping, int p_loop_brightness,
			int p_loop_loss, int p_loop_tone_tilt,
			int p_stiffness, int p_dispersion, int p_bend, int p_odd_even,
			int p_body_type, int p_body_amount, int p_body_tune, int p_body_width,
			int p_pitch_drift, int p_pitch_drop, int p_pick_bend,
			int p_tension_mod, int p_keytrack, int p_glide,
			int p_release_mode);

	// Exciter.
	void set_exciter_type(ExciterType p_type) { _exciter_type = p_type; }
	ExciterType get_exciter_type() const { return _exciter_type; }
	void set_exciter_color(double p_value) { _exciter_color = CLAMP(p_value, 0.0, 1.0); }
	double get_exciter_color() const { return _exciter_color; }
	void set_exciter_length(double p_value) { _exciter_length = CLAMP(p_value, 0.0, 1.0); }
	double get_exciter_length() const { return _exciter_length; }
	void set_exciter_shape(double p_value) { _exciter_shape = CLAMP(p_value, 0.0, 1.0); }
	double get_exciter_shape() const { return _exciter_shape; }
	void set_exciter_drive(double p_value) { _exciter_drive = CLAMP(p_value, 0.0, 1.0); }
	double get_exciter_drive() const { return _exciter_drive; }
	void set_exciter_pitch_follow(double p_value) { _exciter_pitch_follow = CLAMP(p_value, 0.0, 1.0); }
	double get_exciter_pitch_follow() const { return _exciter_pitch_follow; }
	void set_exciter_randomness(double p_value) { _exciter_randomness = CLAMP(p_value, 0.0, 1.0); }
	double get_exciter_randomness() const { return _exciter_randomness; }

	// Loop filter.
	void set_loop_filter_mode(LoopFilterMode p_mode);
	LoopFilterMode get_loop_filter_mode() const { return _loop_filter_mode; }
	void set_loop_damping(double p_value) { _loop_damping = CLAMP(p_value, 0.0, 1.0); _configure_loop_filter(); }
	double get_loop_damping() const { return _loop_damping; }
	void set_loop_brightness(double p_value) { _loop_brightness = CLAMP(p_value, 0.0, 1.0); _configure_loop_filter(); }
	double get_loop_brightness() const { return _loop_brightness; }
	void set_loop_loss(double p_value) { _loop_loss = CLAMP(p_value, 0.0, 1.0); }
	double get_loop_loss() const { return _loop_loss; }
	void set_loop_tone_tilt(double p_value) { _loop_tone_tilt = CLAMP(p_value, -1.0, 1.0); _configure_loop_filter(); }
	double get_loop_tone_tilt() const { return _loop_tone_tilt; }

	// Inharmonicity.
	void set_stiffness(double p_value) { _stiffness = CLAMP(p_value, 0.0, 1.0); _configure_stiffness(); }
	double get_stiffness() const { return _stiffness; }
	void set_dispersion(double p_value) { _dispersion = CLAMP(p_value, 0.0, 1.0); _configure_stiffness(); }
	double get_dispersion() const { return _dispersion; }
	void set_bend(double p_value) { _bend = CLAMP(p_value, -1.0, 1.0); }
	double get_bend() const { return _bend; }
	void set_odd_even_balance(double p_value) { _odd_even_balance = CLAMP(p_value, 0.0, 1.0); }
	double get_odd_even_balance() const { return _odd_even_balance; }

	// Body.
	void set_body_type(BodyType p_type);
	BodyType get_body_type() const { return _body_type; }
	void set_body_amount(double p_value) { _body_amount = CLAMP(p_value, 0.0, 1.0); }
	double get_body_amount() const { return _body_amount; }
	void set_body_tune(double p_value);
	double get_body_tune() const { return _body_tune; }
	void set_body_width(double p_value);
	double get_body_width() const { return _body_width; }

	// Pitch behavior.
	void set_pitch_drift(double p_value) { _pitch_drift = CLAMP(p_value, 0.0, 1.0); }
	double get_pitch_drift() const { return _pitch_drift; }
	void set_pitch_drop(double p_value) { _pitch_drop = CLAMP(p_value, 0.0, 1.0); }
	double get_pitch_drop() const { return _pitch_drop; }
	void set_pick_bend(double p_value) { _pick_bend = CLAMP(p_value, -1.0, 1.0); }
	double get_pick_bend() const { return _pick_bend; }
	void set_tension_mod(double p_value) { _tension_mod = CLAMP(p_value, -1.0, 1.0); }
	double get_tension_mod() const { return _tension_mod; }
	void set_pitch_keytrack(double p_value) { _pitch_keytrack = CLAMP(p_value, 0.0, 2.0); }
	double get_pitch_keytrack() const { return _pitch_keytrack; }
	void set_pitch_glide(double p_value) { _pitch_glide = CLAMP(p_value, 0.0, 1.0); }
	double get_pitch_glide() const { return _pitch_glide; }

	// Release mode.
	void set_release_mode(ReleaseMode p_mode) { _release_mode = p_mode; }
	ReleaseMode get_release_mode() const { return _release_mode; }

	// Volume control.
	virtual void offset_volume(int p_expression, int p_velocity) override;

	// Processing.
	virtual void note_on() override;
	virtual void note_off() override;

	virtual void reset_channel_buffer_status() override;
	virtual void buffer(int p_length) override;

	//
	virtual void initialize(SiOPMChannelBase *p_prev, int p_buffer_index) override;
	virtual void reset() override;

	SiOPMChannelKS(SiOPMSoundChip *p_chip = nullptr);
};

#endif // SIOPM_CHANNEL_KS_H
