#ifndef SIOPM_CHANNEL_MONOLITH_H
#define SIOPM_CHANNEL_MONOLITH_H

#include "chip/channels/siopm_channel_base.h"
#include <cstdint>

using namespace godot;

class SiOPMChannelParams;
class SiOPMSoundChip;

// Monolith bass engine channel.
//
// Dedicated bass synth with a protected sub oscillator, two main oscillators
// with phase warp / wavefold / sync shapes, multi-mode distortion, motion
// LFO, crossover-aware low-lock, and a small-speaker "lens" harmonic
// enhancer.  Outputs to the standard SiON integer pipe so the base class
// buffer() provides SV filter, kill-fade, and stream routing for free.
class SiOPMChannelMonolith : public SiOPMChannelBase {
	GDCLASS(SiOPMChannelMonolith, SiOPMChannelBase)

public:
	enum SubShape {
		SUB_SINE = 0,
		SUB_TRIANGLE = 1,
		SUB_ROUNDED_SQUARE = 2,
		SUB_SATURATED_SINE = 3,
		SUB_OCTAVE_STACK = 4,
		SUB_CLICK = 5,
		SUB_808 = 6,
		SUB_SHAPE_MAX
	};

	enum OscShape {
		OSC_SAW = 0,
		OSC_PULSE = 1,
		OSC_TRIANGLE = 2,
		OSC_SINE_FOLD = 3,
		OSC_FORMANT = 4,
		OSC_SYNC = 5,
		OSC_DIGITAL = 6,
		OSC_NOISE = 7,
		OSC_SHAPE_MAX
	};

	enum DriveMode {
		DRIVE_WARM = 0,
		DRIVE_CLIP = 1,
		DRIVE_FOLD = 2,
		DRIVE_TEAR = 3,
		DRIVE_TUBE = 4,
		DRIVE_DIGITAL = 5,
		DRIVE_MODE_MAX
	};

	enum MotionTarget {
		MOTION_OFF = 0,
		MOTION_OSC_SHAPE = 1,
		MOTION_WARP = 2,
		MOTION_FILTER = 3,
		MOTION_DRIVE_TONE = 4,
		MOTION_PHASE = 5,
		MOTION_RESONANCE = 6,
		MOTION_SUB_LEVEL = 7,
		MOTION_TARGET_MAX
	};

private:
	// Grouped-setter parameters.
	int _sub_shape = SUB_SINE;
	int _sub_level = 80;
	int _sub_drive = 0;
	int _pitch_drop = 0;
	int _osc1_shape = OSC_SAW;
	int _osc2_shape = OSC_SAW;
	int _mass = 40;
	int _bite = 40;
	int _shape_warp = 0;
	int _drive_mode = DRIVE_WARM;
	int _grind = 0;
	int _motion_target_param = MOTION_OFF;
	int _motion_amount = 0;
	int _motion_rate = 40;
	int _width = 0;
	int _low_lock = 100;
	int _lens = 0;
	int _glide_time = 0;

	// Pitch state.
	int _current_pitch = 0;
	double _expression = 1.0;

	// Oscillator phase accumulators (uint32 wrap-around).
	uint32_t _sub_phase = 0;
	uint32_t _osc1_phase = 0;
	uint32_t _osc2_phase = 0;
	uint32_t _noise_state = 0x12345678;

	// Phase increments computed from pitch.
	uint32_t _sub_phase_inc = 0;
	uint32_t _osc1_phase_inc = 0;
	uint32_t _osc2_phase_inc = 0;

	// 808 pitch envelope.
	double _pitch_env_level = 0.0;
	double _pitch_env_decay_coeff = 0.9995;

	// Sub click transient.
	double _click_level = 0.0;
	static constexpr double CLICK_DECAY = 0.992;

	// Motion LFO.
	uint32_t _motion_phase = 0;
	uint32_t _motion_phase_inc = 0;
	double _motion_value = 0.0;

	// Glide.
	double _glide_current_pitch = 0.0;
	double _target_pitch_f = 0.0;
	double _glide_coeff = 1.0;
	bool _has_previous_note = false;

	// Declick.
	static constexpr int DECLICK_SAMPLES = 256;
	static constexpr double DECLICK_INCREMENT = 1.0 / DECLICK_SAMPLES;
	double _declick_level = 0.0;
	double _declick_target = 0.0;

	// Lens high-pass state.
	double _lens_hp_z1 = 0.0;

	// Cached sample rate.
	double _sample_rate = 44100.0;

	// Internal DSP helpers.
	void _update_phase_increments(double p_motion_mod);
	double _generate_sub_sample();
	double _generate_osc_sample(uint32_t p_phase, int p_shape, double p_warp);
	double _apply_drive(double p_sample, int p_mode, double p_amount);
	double _wavefold(double p_sample, double p_amount);
	double _generate_noise();

	uint32_t _pitch_to_phase_inc(double p_pitch_units);
	uint32_t _freq_to_phase_inc(double p_freq);

	void _process_monolith(int p_length);

protected:
	static void _bind_methods();
	String _to_string() const;

public:
	void set_monolith_params(
			int p_sub_shape, int p_sub_level, int p_sub_drive, int p_pitch_drop,
			int p_osc1_shape, int p_osc2_shape,
			int p_mass, int p_bite, int p_shape,
			int p_drive_mode, int p_grind,
			int p_motion_target, int p_motion_amount, int p_motion_rate,
			int p_width, int p_low_lock, int p_lens, int p_glide);

	int get_monolith_sub_shape() const { return _sub_shape; }
	int get_monolith_sub_level() const { return _sub_level; }
	int get_monolith_sub_drive() const { return _sub_drive; }
	int get_monolith_pitch_drop() const { return _pitch_drop; }
	int get_monolith_osc1_shape() const { return _osc1_shape; }
	int get_monolith_osc2_shape() const { return _osc2_shape; }
	int get_monolith_mass() const { return _mass; }
	int get_monolith_bite() const { return _bite; }
	int get_monolith_shape() const { return _shape_warp; }
	int get_monolith_drive_mode() const { return _drive_mode; }
	int get_monolith_grind() const { return _grind; }
	int get_monolith_motion_target() const { return _motion_target_param; }
	int get_monolith_motion_amount() const { return _motion_amount; }
	int get_monolith_motion_rate() const { return _motion_rate; }
	int get_monolith_width() const { return _width; }
	int get_monolith_low_lock() const { return _low_lock; }
	int get_monolith_lens() const { return _lens; }
	int get_monolith_glide() const { return _glide_time; }

	virtual int get_pitch() const override { return _current_pitch; }
	virtual void set_pitch(int p_value) override;

	virtual void get_channel_params(const Ref<SiOPMChannelParams> &p_params) const override;
	virtual void set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation = true) override;

	virtual void set_all_attack_rate(int p_value) override {}
	virtual void set_all_release_rate(int p_value) override {}

	virtual void offset_volume(int p_expression, int p_velocity) override;

	virtual void note_on() override;
	virtual void note_off() override;

	virtual void reset_channel_buffer_status() override;

	virtual void initialize(SiOPMChannelBase *p_prev, int p_buffer_index) override;
	virtual void reset() override;

	SiOPMChannelMonolith(SiOPMSoundChip *p_chip = nullptr);
};

#endif // SIOPM_CHANNEL_MONOLITH_H
