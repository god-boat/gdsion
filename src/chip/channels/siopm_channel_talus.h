#ifndef SIOPM_CHANNEL_TALUS_H
#define SIOPM_CHANNEL_TALUS_H

#include <cstdint>
#include "chip/channels/siopm_channel_base.h"
#include "chip/channels/siopm_talus_params.h"
#include "dsp/allpass.h"
#include "dsp/bar_modes.h"
#include "dsp/biquad.h"
#include "dsp/fractional_delay.h"
#include "dsp/membrane_modes.h"
#include "dsp/modal_bank.h"
#include "dsp/one_pole.h"

using namespace godot;

class SiOPMChannelParams;
class SiOPMSoundChip;

// Talus: physical-model percussion.
//
// An exciter (a half-sine stick pulse, a noise burst and a tone blip) drives a
// tuned resonator sample by sample: a modal bank with the ratios of a circular
// membrane or of a bar, or a feedback delay loop tuned to the fundamental. On a
// string the pulse is a pluck. Snare wires rattle off the head's motion, and a
// body rings with it. Head, Direct, Wires and Body mix the resonator, the
// exciter, the wires and the body into the output.
//
// Velocity sets only the strike force. A harder hit is brighter because its
// contact time shortens, a loud hit glides down as head tension relaxes, and a
// ghost note keeps its wires because they saturate. A new hit adds to whatever
// rings, so the channel is linear while Tension and Wires are off.
//
// Every element of the loop has gain at most 1, so the channel is passive for
// any parameter. Renders into the standard SiON integer pipe, so the base class
// buffer() provides the SV filter, kill fade and stream routing.
class SiOPMChannelTalus : public SiOPMChannelBase {
	GDCLASS(SiOPMChannelTalus, SiOPMChannelBase)

	static constexpr int MODE_MAX = MAX(sion::dsp::MEMBRANE_MODE_COUNT, sion::dsp::BAR_MODE_COUNT);
	static constexpr int BODY_MODE_COUNT = 6;
	static constexpr int EXCITER_COUNT = 2;

	enum Model {
		MODEL_MEMBRANE,
		MODEL_LOOP,
		MODEL_BAR,
		MODEL_TUNED_BAR,
		MODEL_STRING, // The loop, plucked.
		MODEL_COUNT,
	};

	// What a modal model rings at. The loop models have no table.
	struct ModeTable {
		const sion::dsp::ModeShape *modes = nullptr;
		int count = 0;
		double stiffness_max = 0.0; // The stiffness that puts the eighth partial a semitone sharp.
	};
	static const ModeTable MODE_TABLES[MODEL_COUNT];

	enum ReleaseMode {
		RELEASE_RING,
		RELEASE_MUTE,
	};

	// One hit's exciter: the pulse, the burst and the blip, summed. Two run at
	// once, so a hit's tail still sounds when the next hit lands.
	struct Exciter {
		int remaining = 0; // Samples until the hit has nothing left above the floor.
		int pulse_samples = 0; // Half-sine samples still to come.
		double pulse_step = 0.0; // 2 cos(w), for the sine recurrence.
		double pulse = 0.0;
		double pulse_previous = 0.0;
		bool plucked = false; // The pulse reaches the string gathered over a few periods.
		double pluck_coeff = 0.0;
		double pluck_drain_coeff = 0.0;
		sion::dsp::OnePole pluck;
		sion::dsp::OnePole pluck_drain;
		uint32_t noise_step = 0;
		double noise_level = 0.0;
		sion::dsp::BiquadState noise_band;
		double tone_cos = 1.0;
		double tone_sin = 0.0;
		double tone_re = 1.0;
		double tone_im = 0.0;
		double tone_level = 0.0;
	};

	TalusParams _params;

	// Param-derived values.
	double _strike_level = 1.0;
	double _hardness = 0.6;
	double _velocity_amount = 0.8;
	double _noise_level = 0.3;
	double _noise_decay = 0.0;
	sion::dsp::BiquadCoeffs _noise_band_coeffs;
	double _tone_level = 0.0;
	double _tone_cents = 0.0;
	double _tone_drive = 1.0;
	double _tone_normal = 1.0;
	double _tone_decay = 0.0;
	double _variation = 0.2;
	int _model = MODEL_MEMBRANE;
	double _tune = 0.0; // Semitones, with Fine.
	double _keytrack = 1.0;
	double _decay_seconds = 0.8;
	double _damping_power = 1.2;
	double _position = 0.3;
	double _stiffness = 0.0;
	double _low_cut_hz = 1.0;
	sion::dsp::BiquadCoeffs _cut_coeffs;
	double _tension_cents = 0.0;
	double _drive = 0.0;
	double _inverse_drive = 1.0;
	double _wires_level = 0.0;
	double _wire_inverse_knee = 1.0;
	sion::dsp::BiquadCoeffs _wire_band_coeffs;
	double _wire_release = 0.0;
	double _head_level = 1.0;
	double _direct_level = 0.2;
	int _release_mode = RELEASE_RING;
	double _release_seconds = 0.08;
	double _body_level = 0.0;
	sion::dsp::ModalCoeffs<BODY_MODE_COUNT> _body;
	double _output_gain = 1.0;
	double _head_silence = 0.0;

	// Sample-rate-derived values.
	double _sample_rate = 48000.0;
	double _resonator_input = 1.0;
	double _direct_coeff = 0.0;
	double _wire_attack = 0.0;
	double _tension_attack = 0.0;
	double _delay_smoothing = 0.0;

	// The current hit and tuning.
	int _current_pitch = 0;
	bool _pitch_changed = false;
	double _expression = 1.0;
	uint32_t _random_seed = 0;
	uint32_t _hit_index = 0;
	double _hit_position = 0.285; // Strike radius, 0 center to POSITION_REACH.
	double _hit_cents = 0.0;
	bool _muted = false;
	double _f1 = 261.6;
	double _tension_applied = 0.0; // The glide in cents the resonator is tuned to.
	int _update_countdown = 1;
	Exciter _exciters[EXCITER_COUNT];

	// Membrane and the bars.
	sion::dsp::ModalCoeffs<MODE_MAX> _modes;
	sion::dsp::ModalState<MODE_MAX> _mode_state;

	// Loop and String.
	sion::dsp::FractionalDelay _loop;
	sion::dsp::OnePole _loop_lowpass;
	sion::dsp::OnePole _loop_highpass;
	sion::dsp::BiquadState _loop_cut;
	sion::dsp::Allpass _loop_allpasses[2];
	double _period = 183.0;
	double _loop_input_scale = 1.0;
	double _loop_lowpass_coeff = 1.0;
	double _loop_highpass_coeff = 0.0;
	double _loop_gain = 0.0;
	double _allpass_coeff = 0.0;
	double _loop_phase_delay = 0.0; // Every in-loop filter's, at the fundamental.
	double _read_delay = 183.0;
	double _read_delay_target = 183.0;
	double _tap_fraction = 0.36;
	int _loop_quiet_samples = 0;

	// Tension, wires, body and direct.
	double _tension_power = 0.0;
	double _tension_release = 0.0;
	sion::dsp::OnePole _wire_motion;
	double _wire_motion_coeff = 0.0;
	double _wire_envelope = 0.0;
	sion::dsp::BiquadState _wire_band;
	uint32_t _wire_step = 0;
	sion::dsp::ModalState<BODY_MODE_COUNT> _body_state;
	sion::dsp::OnePole _direct_highpass;

	void _strike(double p_velocity);
	static int _samples_above_floor(double p_level, double p_decay);
	double _tick_exciter(Exciter &r_exciter);

	bool _is_modal() const { return MODE_TABLES[_model].modes; }
	void _retune();
	void _retune_modes();
	void _retune_loop();
	double _loop_filter_delay(double p_w, double p_radius) const;
	static double _allpass_delay(double p_coeff, double p_w, double p_radius);
	double _loop_kept(double p_w, double p_radius) const;
	double _fit_loop_allpass(double p_w1, double p_radius) const;
	void _apply_tension(double p_cents);

	bool _update();
	bool _is_silent() const;
	void _clear();

	template <bool MODAL, bool DRIVEN>
	double _tick_resonator(double p_input, bool p_excited);
	template <bool MODAL, bool DRIVEN>
	void _render(int p_length);
	void _process_talus(int p_length);

protected:
	static void _bind_methods();
	String _to_string() const;

public:
	enum EngineParameter : uint32_t {
		PARAM_STRIKE_LEVEL = 0x800,
		PARAM_STRIKE_HARDNESS = 0x801,
		PARAM_STRIKE_VELOCITY = 0x802,
		PARAM_NOISE_LEVEL = 0x803,
		PARAM_NOISE_DECAY = 0x804,
		PARAM_NOISE_COLOR = 0x805,
		PARAM_TONE_LEVEL = 0x806,
		PARAM_TONE_PITCH = 0x807,
		PARAM_TONE_SHAPE = 0x808,
		PARAM_TONE_DECAY = 0x809,
		PARAM_STRIKE_VARIATION = 0x80a,
		PARAM_RESONATOR_MODEL = 0x80b,
		PARAM_RESONATOR_TUNE = 0x80c,
		PARAM_RESONATOR_FINE = 0x80d,
		PARAM_RESONATOR_KEYTRACK = 0x80e,
		PARAM_RESONATOR_DECAY = 0x80f,
		PARAM_RESONATOR_DAMPING = 0x810,
		PARAM_RESONATOR_POSITION = 0x811,
		PARAM_RESONATOR_STIFFNESS = 0x812,
		PARAM_RESONATOR_LOW_CUT = 0x813,
		PARAM_RESONATOR_CUT_FREQ = 0x814,
		PARAM_RESONATOR_CUT_DEPTH = 0x815,
		PARAM_RESONATOR_TENSION = 0x816,
		PARAM_RESONATOR_DRIVE = 0x817,
		PARAM_WIRES_LEVEL = 0x818,
		PARAM_WIRES_TENSION = 0x819,
		PARAM_WIRES_TONE = 0x81a,
		PARAM_WIRES_DECAY = 0x81b,
		PARAM_OUTPUT_HEAD = 0x81c,
		PARAM_OUTPUT_DIRECT = 0x81d,
		PARAM_RELEASE_MODE = 0x81e,
		PARAM_RELEASE_TIME = 0x81f,
		PARAM_BODY_LEVEL = 0x820,
		PARAM_BODY_FREQ = 0x821,
		PARAM_BODY_DECAY = 0x822,
	};
	static const EngineParamSchema &get_engine_param_schema();
	virtual void apply_engine_param(uint32_t p_id, int p_index, EngineParamValue p_value) override;

	void _refresh_coefficients();
	const TalusParams &get_talus_params() const { return _params; }

	virtual int get_pitch() const override { return _current_pitch; }
	virtual void set_pitch(int p_value) override;

	virtual void get_channel_params(const Ref<SiOPMChannelParams> &p_params) const override;
	virtual void set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation = true) override;

	virtual void offset_volume(int p_expression, int p_velocity) override;

	virtual void note_on() override;
	virtual void note_off() override;

	virtual void initialize(SiOPMChannelBase *p_prev, int p_buffer_index) override;
	virtual void reset() override;

	SiOPMChannelTalus(SiOPMSoundChip *p_chip = nullptr);
};

#endif // SIOPM_CHANNEL_TALUS_H
