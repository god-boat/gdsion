#ifndef SIOPM_CHANNEL_STRATA_H
#define SIOPM_CHANNEL_STRATA_H

#include "chip/channels/siopm_channel_base.h"
#include "chip/braids/macro_oscillator.h"

using namespace godot;

class SiOPMSoundChip;

// SiOPM channel wrapper around Mutable Instruments Braids' MacroOscillator.
//
// Braids' DSP runs at a hardcoded 96 kHz internal sample rate and processes
// 24-sample blocks. We render Braids in 24-sample sub-blocks at the host
// rate, applying a sample-rate pitch correction so MIDI note numbers translate
// to the same audible pitch regardless of host rate.
class SiOPMChannelStrata : public SiOPMChannelBase {
	GDCLASS(SiOPMChannelStrata, SiOPMChannelBase)

	static constexpr int BRAIDS_BLOCK_SIZE = 24;
	// Internal Braids sample rate, baked into all phase increment / delay tables.
	static constexpr double REFERENCE_SAMPLE_RATE = 96000.0;

	braids::MacroOscillator _osc;

	// Channel-level Braids params (in Braids' native units).
	int _shape = (int)braids::MACRO_OSC_SHAPE_CSAW;
	int _timbre = 0;     // 0..32767 ("parameter[0]" / TIMBRE)
	int _color = 0;      // 0..32767 ("parameter[1]" / COLOR)

	int _current_pitch = 0;          // SiON pitch (64 units / semitone).
	int _pitch_correction = 0;       // Added to braids pitch to compensate for sample rate.
	double _expression = 1.0;

	// 24-sample render buffers.
	int16_t _render_buffer[BRAIDS_BLOCK_SIZE] = {};
	uint8_t _sync_buffer[BRAIDS_BLOCK_SIZE] = {};

	// Larger block scratch (stereo, sized to host buffer).
	double _scratch_left[2048] = {};
	double _scratch_right[2048] = {};

	void _recompute_pitch_correction(double p_sample_rate);
	void _set_strata_pitch();

protected:
	static void _bind_methods() {}

	String _to_string() const;

public:
	void set_strata_params(int p_shape, int p_timbre, int p_color);

	int get_strata_shape() const { return _shape; }
	int get_strata_timbre() const { return _timbre; }
	int get_strata_color() const { return _color; }

	virtual int get_pitch() const override { return _current_pitch; }
	virtual void set_pitch(int p_value) override { _current_pitch = p_value; }

	virtual void set_all_attack_rate(int p_value) override {}
	virtual void set_all_release_rate(int p_value) override {}

	virtual void offset_volume(int p_expression, int p_velocity) override;

	virtual void note_on() override;
	virtual void note_off() override;

	virtual void reset_channel_buffer_status() override;
	virtual void buffer(int p_length) override;

	virtual void initialize(SiOPMChannelBase *p_prev, int p_buffer_index) override;
	virtual void reset() override;

	SiOPMChannelStrata(SiOPMSoundChip *p_chip = nullptr);
};

#endif // SIOPM_CHANNEL_STRATA_H
