#ifndef SIOPM_CHANNEL_IRON_H
#define SIOPM_CHANNEL_IRON_H

#include <cstdint>
#include "chip/channels/siopm_channel_base.h"
#include "chip/channels/siopm_iron_params.h"
#include "dsp/allpass.h"
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
// The hands make noise too. Scrape drags the pick over the windings before it
// lets go, which holds the note back by the time the drag takes. Pick Bend is
// how sharp a hard pick stretches the string, settling as it rings down. Fret
// Noise is the fretting hand: the string buzzing on the fret as a finger lifts
// or a slur lands, and fingers dragging off the windings after a release.
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
		double amplitude = 0.5;
		double velocity = 1.0;
		double mute_offset = 0.0;
		double position = 0.1; // Fraction of the string length from the bridge.
		double hardness = 0.5;
		double cents = 0.0;
		double bloom_cents = 0.0;
		// The drag over the windings that leads the pick in.
		int contact_samples = 0;
		double scrape_level = 0.0;
		double scrape_delay = 1.0; // Round trip of the string between the pick and the bridge.
	};

	struct GuitarString {
		sion::dsp::FractionalDelay delay;
		sion::dsp::OnePole loop_lowpass;
		sion::dsp::Allpass allpasses[2];

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
		double bloom_decay = 0.0;

		double fade = 1.0;
		double fade_step = 0.0;
		int quiet_samples = 0;

		// The pick's noise, through one highpass: the drag over the windings, then the click as it lets go.
		double click_level = 0.0;
		double click_amplitude = 0.0;
		sion::dsp::OnePole click_lowpass;
		sion::dsp::FractionalDelay scrape_comb;
		double ridge_phase = 0.0;
		// How hard the string buzzes on the fret under a finger that just moved.
		double rattle = 0.0;

		int pending_samples = -1; // Samples until the queued pick lands; -1 when none is queued.
		Pick pending;
	};

	// The fretting hand dragging along the windings as it leaves a note.
	struct FretDrag {
		int samples = 0; // Left to run.
		int length = 1;
		double level = 0.0;
		double ridge_rate = 0.0; // Ridges per sample at the hand's top speed.
		double ridge_phase = 0.0;
		sion::dsp::BiquadState resonance;
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
	double _pick_scrape = 0.5;
	double _pick_bend_cents = 20.0;
	double _fret_noise = 0.16;
	double _output_trim = 1.0;

	// Sample-rate-derived values.
	double _sample_rate = 48000.0;
	double _delay_smoothing = 0.0;
	double _loop_smoothing = 0.0;
	double _click_decay = 0.0;
	double _click_lowpass_coeff = 0.0;
	double _scrape_ridge_rate = 0.0;
	double _rattle_decay = 0.0;
	sion::dsp::BiquadCoeffs _drag_coeffs;
	double _dc_coeff = 0.0;
	int _retrigger_fade_samples = 1;
	int _attack_fade_samples = 1;

	GuitarString _strings[STRING_COUNT];
	FretDrag _drag;

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

	double _note_period(double p_note) const;
	void _tune_string(GuitarString &r_string);
	void _release_string(GuitarString &r_string);
	void _pluck(GuitarString &r_string);
	double _tick_string(GuitarString &r_string);
	double _tick_ridges(double &r_phase, double p_rate);
	double _tick_scrape(GuitarString &r_string);
	void _drag_off();
	double _tick_drag();
	double _tick_stages(double p_input);
	double _tick_amp(double p_input);
	void _clear_amp();

	void _process_iron(int p_length);

protected:
	static void _bind_methods();
	String _to_string() const;

public:
	enum EngineParameter : uint32_t {
		PARAM_PICK_VOICING = 0x700,
		PARAM_PICK_ATTACK = 0x701,
		PARAM_PICK_POSITION = 0x702,
		PARAM_MUTE_AMOUNT = 0x703,
		PARAM_MUTE_VELOCITY = 0x704,
		PARAM_STRING_SUSTAIN = 0x705,
		PARAM_STRING_RELEASE = 0x706,
		PARAM_STRING_STIFFNESS = 0x707,
		PARAM_PICKUP_POSITION = 0x708,
		PARAM_AMP_GAIN = 0x709,
		PARAM_AMP_TIGHT = 0x70a,
		PARAM_AMP_BASS = 0x70b,
		PARAM_AMP_MID = 0x70c,
		PARAM_AMP_TREBLE = 0x70d,
		PARAM_CABINET_TYPE = 0x70e,
		PARAM_CABINET_MIC = 0x70f,
		PARAM_HUMANIZE_AMOUNT = 0x710,
		PARAM_HUMANIZE_TIMING = 0x711,
		PARAM_PICK_SCRAPE = 0x712,
		PARAM_PICK_BEND = 0x713,
		PARAM_FRET_NOISE = 0x714,
	};
	static const EngineParamSchema &get_engine_param_schema();
	virtual void apply_engine_param(uint32_t p_id, int p_index, EngineParamValue p_value) override;

	void _refresh_coefficients();
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
