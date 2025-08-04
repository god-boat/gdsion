/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SIOPM_CHANNEL_SAMPLER_H
#define SIOPM_CHANNEL_SAMPLER_H

#include "chip/channels/siopm_channel_base.h"

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

	// Simple ADSR envelope (currently only release stage used to avoid clicks).
	bool _is_releasing = false;
	int _release_samples_left = 0;
	static const int RELEASE_SAMPLES = 512; // ~11ms at 44.1 kHz.
	double _envelope_level = 1.0;

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

	// Processing.

	virtual void note_on() override;
	virtual void note_off() override;

	virtual void buffer(int p_length) override;
	virtual void buffer_no_process(int p_length) override;

	//

	virtual void initialize(SiOPMChannelBase *p_prev, int p_buffer_index) override;
	virtual void reset() override;

	SiOPMChannelSampler(SiOPMSoundChip *p_chip = nullptr);
};

#endif // SIOPM_CHANNEL_SAMPLER_H
