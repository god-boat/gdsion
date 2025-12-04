/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SIOPM_CHANNEL_SAMPLER_H
#define SIOPM_CHANNEL_SAMPLER_H

#include "chip/channels/siopm_channel_base.h"
#include "templates/singly_linked_list.h"

enum SiONPitchTableType : unsigned int;
class SiOPMChannelParams;
class SiOPMSoundChip;
class SiOPMWaveBase;
class SiOPMWaveSamplerData;
class SiOPMWaveSamplerTable;

class SiOPMChannelSampler : public SiOPMChannelBase {
	GDCLASS(SiOPMChannelSampler, SiOPMChannelBase)

	int _bank_number = 0;
	int _wave_number = -1;
	double _expression = 1;

	Ref<SiOPMWaveSamplerTable> _sampler_table;
	Ref<SiOPMWaveSamplerData> _sample_data;
	int _sample_start_phase = 0;
	int _sample_index = 0;
	// Pan of the current note.
	int _sample_pan = 0;

	// Pitching support.
	int _fine_pitch = 0; // Fine pitch (0-63).
	double _pitch_step = 1.0; // Playback rate step per output sample.
	double _sample_index_fp = 0.0; // Fractional sample position.

	enum AmplitudeStage {
		AMP_STAGE_IDLE = 0,
		AMP_STAGE_ATTACK,
		AMP_STAGE_DECAY,
		AMP_STAGE_SUSTAIN,
		AMP_STAGE_RELEASE,
	};

	int _amp_attack_rate = 0;
	int _amp_decay_rate = 0;
	int _amp_release_rate = 0;
	int _amp_sustain_level = 128;
	AmplitudeStage _amp_stage = AMP_STAGE_IDLE;
	double _amp_level = 0.0;
	double _amp_stage_target_level = 0.0;
	double _amp_stage_increment = 0.0;
	int _amp_stage_samples_left = 0;
	double _amp_rate_scale = 1.0;
	double _envelope_level = 1.0;

	bool _click_guard_active = false;
	int _click_guard_samples_left = 0;
	double _click_guard_level = 1.0;
	static const int RELEASE_SAMPLES = 512; // ≈ 512 / 48 000 Hz ≈ 10.7 ms at 48 kHz.

	// LFO (AM/PM) state mirrored from PCM behavior.
	int _amplitude_modulation_depth = 0; // = chip.amd << (ams - 1)
	int _amplitude_modulation_output_level = 0;
	int _pitch_modulation_depth = 0; // = chip.pmd << (pms - 1)
	int _pitch_modulation_output_level = 0;
	int _lfo_timer_initial = 0; // LFO_TIMER_INITIAL * freq_ratio
	// AM linear gain derived from log domain (parity with FM).
	double _amplitude_modulation_gain = 1.0;

	// Second output pipe and filter variables for stereo processing.
	SinglyLinkedList<int> *_out_pipe2 = nullptr;
	double _filter_variables2[3] = { 0, 0, 0 };

	// LFO helpers.
	void _set_lfo_state(bool p_enabled);
	void _set_lfo_timer(int p_value);
	void _update_lfo();

	// Amplitude envelope helpers.
	void _reset_amp_envelope();
	void _start_amp_envelope();
	void _begin_amp_release();
	void _advance_amp_stage();
	void _set_amp_stage(AmplitudeStage p_stage);
	void _configure_amp_stage(double p_target_level, int p_rate);
	void _refresh_active_amp_stage();
	int _compute_amp_samples_per_unit(int p_rate) const;
	void _update_amp_envelope();
	void _begin_click_guard();
	void _stop_click_guard();

	// Stream writers (mirror PCM).
	void _write_stream_mono(SinglyLinkedList<int>::Element *p_output, int p_length);
	void _write_stream_stereo(SinglyLinkedList<int>::Element *p_output_left, SinglyLinkedList<int>::Element *p_output_right, int p_length);

protected:
	static void _bind_methods() {}

	String _to_string() const;

public:
	virtual void get_channel_params(const Ref<SiOPMChannelParams> &p_params) const override;
	virtual void set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation = true) override;

	virtual void set_wave_data(const Ref<SiOPMWaveBase> &p_wave_data) override;

	virtual void set_types(int p_pg_type, SiONPitchTableType p_pt_type) override;

	virtual int get_pitch() const override;
	virtual void set_pitch(int p_value) override;

	virtual void set_phase(int p_value) override;

	// Volume control.

	virtual void offset_volume(int p_expression, int p_velocity) override;

	// LFO control.
	virtual void set_frequency_ratio(int p_ratio) override;
	virtual void initialize_lfo(int p_waveform, Vector<int> p_custom_wave_table = Vector<int>()) override;
	virtual void set_amplitude_modulation(int p_depth) override;
	virtual void set_pitch_modulation(int p_depth) override;

	// Processing.

	virtual void note_on() override;
	virtual void note_off() override;

	virtual void buffer(int p_length) override;
	virtual void buffer_no_process(int p_length) override;

	//

	virtual void initialize(SiOPMChannelBase *p_prev, int p_buffer_index) override;
	virtual void reset() override;

	void set_amp_attack_rate(int p_value);
	void set_amp_decay_rate(int p_value);
	void set_amp_sustain_level(int p_value);
	void set_amp_release_rate(int p_value);

	SiOPMChannelSampler(SiOPMSoundChip *p_chip = nullptr);
};

#endif // SIOPM_CHANNEL_SAMPLER_H
