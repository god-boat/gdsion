#ifndef SIOPM_CHANNEL_IRON_H
#define SIOPM_CHANNEL_IRON_H

#include <cstdint>
#include "chip/channels/siopm_channel_base.h"
#include "chip/channels/siopm_iron_params.h"
#include "dsp/biquad.h"
#include "dsp/fractional_delay.h"
#include "dsp/halfband.h"
#include "dsp/one_pole.h"

using namespace godot;

class SiOPMChannelParams;
class SiOPMSoundChip;

// Iron: a distorted electric guitar for heavy styles.
//
// Each note picks up to three waveguide strings (the voicing turns one key into
// a power chord), sums them at a magnetic pickup and drives one high-gain amp:
// a tight/mid-hump pre-EQ, four tube-style gain stages run 2x oversampled, a
// tone stack and a speaker cabinet. The strings of a voicing share that amp, so
// a power chord intermodulates the way a real one does.
//
// Humanize varies every pick: its strength, position and hardness, the palm
// pressure, each string's intonation and the pitch bloom of a hard pick. Timing
// lands each pick a few random ms late and strums the voicing strings. Draws
// hash a per-channel step counter, so a channel repeats its sequence.
//
// Renders into the standard SiON integer pipe, so the base class buffer()
// provides the SV filter, kill fade and stream routing.
class SiOPMChannelIron : public SiOPMChannelBase {
	GDCLASS(SiOPMChannelIron, SiOPMChannelBase)

	static constexpr int STRING_COUNT = 3;
	static constexpr int AMP_STAGE_COUNT = 4;
	static constexpr int TONE_BIQUAD_COUNT = 4;
	static constexpr int CABINET_BIQUAD_COUNT = 5;

	// A pick queued on a string until its humanized onset.
	struct Pick {
		int semitones = 0;
		double level = 1.0;
		double velocity = 1.0;
		double mute_offset = 0.0;
		double position = 0.1; // Fraction of the string length from the bridge.
		double hardness = 0.5;
		double cents = 0.0;
		double bloom_cents = 0.0;
	};

	struct GuitarString {
		sion::dsp::FractionalDelay delay;
		sion::dsp::OnePole loop_lowpass;
		double allpass_x[2] = {};
		double allpass_y[2] = {};

		bool active = false;
		// The fretting hand let go: the loop damps toward the release time.
		bool released = false;
		double note = 0.0; // Semitones (MIDI scale) of the channel pitch this string follows.
		int semitones = 0;
		double cents = 0.0;
		// What this pick adds to the palm mute param: soft-pick muting plus humanize.
		double mute_offset = 0.0;

		double period = 0.0;
		double pickup_delay = 0.0;
		double read_delay = 0.0;
		double read_delay_target = 0.0;
		double loop_lowpass_coeff = 1.0;
		double loop_lowpass_coeff_target = 1.0;
		double loop_gain = 0.0;
		double loop_gain_target = 0.0;
		double allpass_coeff = 0.0;
		double bloom_cents = 0.0;

		double fade = 1.0;
		double fade_step = 0.0;
		int quiet_samples = 0;

		double click_level = 0.0;
		double click_amplitude = 0.0;
		sion::dsp::OnePole click_lowpass;

		int pending_samples = -1; // Samples until the queued pick lands; -1 when none is queued.
		Pick pending;
	};

	struct AmpStage {
		sion::dsp::OnePole input_lowpass;
		sion::dsp::OnePole coupling;
		sion::dsp::OnePole output_lowpass;
	};

	IronParams _params;

	// Param-derived values.
	int _voicing = 0;
	double _pick_attack = 0.6;
	double _pick_position = 0.1;
	double _palm_mute = 0.0;
	double _mute_velocity = 0.0;
	double _sustain_seconds = 4.0;
	double _release_seconds = 0.04;
	double _stiffness = 0.3;
	double _pickup_position = 0.06;
	double _humanize = 0.35;
	double _timing_seconds = 0.004;
	double _output_trim = 1.0;

	// Sample-rate-derived values.
	double _sample_rate = 48000.0;
	double _delay_smoothing = 0.0;
	double _loop_smoothing = 0.0;
	double _bloom_decay = 0.0;
	double _click_decay = 0.0;
	double _click_lowpass_coeff = 0.0;
	double _dc_coeff = 0.0;
	int _retrigger_fade_samples = 1;
	int _attack_fade_samples = 1;

	GuitarString _strings[STRING_COUNT];

	sion::dsp::BiquadCoeffs _pickup_coeffs;
	sion::dsp::BiquadCoeffs _tight_coeffs;
	sion::dsp::BiquadCoeffs _hump_coeffs;
	sion::dsp::BiquadState _pickup_state;
	sion::dsp::BiquadState _tight_state;
	sion::dsp::BiquadState _hump_state;

	double _stage_gain[AMP_STAGE_COUNT] = {};
	double _stage_bias_offset[AMP_STAGE_COUNT] = {};
	double _stage_input_coeff[AMP_STAGE_COUNT] = {};
	double _stage_coupling_coeff[AMP_STAGE_COUNT] = {};
	double _stage_output_coeff[AMP_STAGE_COUNT] = {};
	AmpStage _stages[AMP_STAGE_COUNT];
	sion::dsp::Halfband _upsampler;
	sion::dsp::Halfband _downsampler;
	sion::dsp::OnePole _dc;

	sion::dsp::BiquadCoeffs _tone_coeffs[TONE_BIQUAD_COUNT];
	sion::dsp::BiquadState _tone_states[TONE_BIQUAD_COUNT];
	sion::dsp::BiquadCoeffs _cabinet_coeffs[CABINET_BIQUAD_COUNT];
	sion::dsp::BiquadState _cabinet_states[CABINET_BIQUAD_COUNT];
	int _cabinet_biquad_count = CABINET_BIQUAD_COUNT;

	int _current_pitch = 0;
	bool _pitch_changed = false;
	double _expression = 1.0;
	uint32_t _random_seed = 0;
	uint32_t _random_step = 0;

	double _random();
	double _random_unit();

	void _tune_string(GuitarString &r_string);
	void _pluck(GuitarString &r_string);
	double _tick_string(GuitarString &r_string);
	double _tick_stages(double p_input);
	double _tick_amp(double p_input);
	void _clear_amp();

	void _process_iron(int p_length);

protected:
	static void _bind_methods();
	String _to_string() const;

public:
	void set_iron_params(const IronParams &p_params);
	const IronParams &get_iron_params() const { return _params; }

	virtual int get_pitch() const override { return _current_pitch; }
	virtual void set_pitch(int p_value) override;

	virtual void get_channel_params(const Ref<SiOPMChannelParams> &p_params) const override;
	virtual void set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation = true) override;

	virtual void offset_volume(int p_expression, int p_velocity) override;

	virtual void note_on() override;
	virtual void note_off() override;

	virtual void initialize(SiOPMChannelBase *p_prev, int p_buffer_index) override;
	virtual void reset() override;

	SiOPMChannelIron(SiOPMSoundChip *p_chip = nullptr);
};

#endif // SIOPM_CHANNEL_IRON_H
