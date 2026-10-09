#pragma once

#include "chip/siopm_sound_chip.h"

class SiOPMChannelParams {
	double _volumes[SiOPMSoundChip::STREAM_SEND_SIZE] = { 1.0 };
	int _instrument_gain_db = 0;
	int _pan = 64;

public:
	void set_operator_count(int p_value) {}
	double get_master_volume(int p_index) const { return _volumes[p_index]; }
	void set_master_volume(int p_index, double p_value) { _volumes[p_index] = p_value; }
	int get_instrument_gain_db() const { return _instrument_gain_db; }
	void set_instrument_gain_db(int p_value) { _instrument_gain_db = p_value; }
	int get_pan() const { return _pan; }
	void set_pan(int p_value) { _pan = p_value; }
	int get_filter_type() const { return 0; }
	int get_filter_cutoff() const { return 128; }
	int get_filter_resonance() const { return 0; }
	int get_filter_attack_rate() const { return 0; }
	int get_filter_decay_rate1() const { return 0; }
	int get_filter_decay_rate2() const { return 0; }
	int get_filter_release_rate() const { return 0; }
	int get_filter_decay_offset1() const { return 128; }
	int get_filter_decay_offset2() const { return 128; }
	int get_filter_sustain_offset() const { return 128; }
	int get_filter_release_offset() const { return 128; }
};
