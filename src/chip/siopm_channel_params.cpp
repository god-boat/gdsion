/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_channel_params.h"

#include <godot_cpp/core/class_db.hpp>
#include "sion_enums.h"
#include "chip/siopm_operator_params.h"
#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "sequencer/base/mml_sequence.h"

using namespace godot;

Ref<SiOPMOperatorParams> SiOPMChannelParams::get_operator_params(int p_index) {
	ERR_FAIL_INDEX_V(p_index, operator_count, nullptr);

	return operator_params[p_index];
}

void SiOPMChannelParams::set_operator_count(int p_value) {
	ERR_FAIL_COND(p_value > MAX_OPERATORS);

	operator_count = p_value;
	_update_carrier_mask();
}

void SiOPMChannelParams::set_algorithm(int p_value) {
	algorithm = p_value;
	_update_carrier_mask();
}

void SiOPMChannelParams::_update_carrier_mask() {
	// Carrier mask lookup tables derived from SiOPMChannelFM algorithm routing.
	// Each entry is a bitmask where bit i = 1 means operator i is a carrier.
	// Index by algorithm number (0-15).

	// 1-operator: always carrier
	static const int CARRIER_MASK_1OP[16] = {
		0b0001, 0b0001, 0b0001, 0b0001, 0b0001, 0b0001, 0b0001, 0b0001,
		0b0001, 0b0001, 0b0001, 0b0001, 0b0001, 0b0001, 0b0001, 0b0001
	};

	// 2-operator algorithms (from _set_algorithm_operator2):
	// alg 0: o1(o0) -> only op1 is carrier
	// alg 1: o0+o1 -> both carriers
	// alg 2: o0+o1(o0) -> both carriers
	// default: o0+o1 -> both carriers
	static const int CARRIER_MASK_2OP[16] = {
		0b0010, 0b0011, 0b0011, 0b0011, 0b0011, 0b0011, 0b0011, 0b0011,
		0b0011, 0b0011, 0b0011, 0b0011, 0b0011, 0b0011, 0b0011, 0b0011
	};

	// 3-operator algorithms (from _set_algorithm_operator3):
	// alg 0: o2(o1(o0)) -> only op2 carrier
	// alg 1: o2(o0+o1) -> only op2 carrier
	// alg 2: o0+o2(o1) -> op0, op2 carriers
	// alg 3: o1(o0)+o2 -> op1, op2 carriers
	// alg 4: o1(o0)+o2(o0) -> op1, op2 carriers
	// alg 5: o0+o1+o2 -> all carriers
	// alg 6: o0+o1(o0)+o2 -> op1, op2 carriers (op0 feeds op1, op1 outputs to mix)
	// default: o0+o1+o2 -> all carriers
	static const int CARRIER_MASK_3OP[16] = {
		0b0100, 0b0100, 0b0101, 0b0110, 0b0110, 0b0111, 0b0110, 0b0111,
		0b0111, 0b0111, 0b0111, 0b0111, 0b0111, 0b0111, 0b0111, 0b0111
	};

	// 4-operator algorithms (from _set_algorithm_operator4):
	// alg 0: o3(o2(o1(o0))) -> only op3 carrier
	// alg 1: o3(o2(o0+o1)) -> only op3 carrier
	// alg 2: o3(o0+o2(o1)) -> only op3 carrier
	// alg 3: o3(o1(o0)+o2) -> only op3 carrier
	// alg 4: o1(o0)+o3(o2) -> op1, op3 carriers
	// alg 5: o1(o0)+o2(o0)+o3(o0) -> op1, op2, op3 carriers
	// alg 6: o1(o0)+o2+o3 -> op1, op2, op3 carriers
	// alg 7: o0+o1+o2+o3 -> all carriers
	// alg 8: o0+o3(o2(o1)) -> op0, op3 carriers
	// alg 9: o0+o2(o1)+o3 -> op0, op2, op3 carriers
	// alg 10: o3(o0+o1+o2) -> only op3 carrier
	// alg 11: o0+o3(o1+o2) -> op0, op3 carriers
	// alg 12: o0+o1(o0)+o3(o2) -> op1, op3 carriers
	// default: o0+o1+o2+o3 -> all carriers
	static const int CARRIER_MASK_4OP[16] = {
		0b1000, 0b1000, 0b1000, 0b1000, 0b1010, 0b1110, 0b1110, 0b1111,
		0b1001, 0b1101, 0b1000, 0b1001, 0b1010, 0b1111, 0b1111, 0b1111
	};

	carrier_mask.resize(operator_count);

	int alg = (algorithm >= 0 && algorithm < 16) ? algorithm : 0;
	int mask_bits = 0;

	switch (operator_count) {
		case 1:
			mask_bits = CARRIER_MASK_1OP[alg];
			break;
		case 2:
			mask_bits = CARRIER_MASK_2OP[alg];
			break;
		case 3:
			mask_bits = CARRIER_MASK_3OP[alg];
			break;
		case 4:
			mask_bits = CARRIER_MASK_4OP[alg];
			break;
		default:
			// For 0 operators or invalid count, clear the mask
			carrier_mask.clear();
			return;
	}

	for (int i = 0; i < operator_count; i++) {
		carrier_mask.set(i, (mask_bits >> i) & 1);
	}
}

bool SiOPMChannelParams::has_amplitude_modulation() const {
	return amplitude_modulation_depth > 0;
}

bool SiOPMChannelParams::has_pitch_modulation() const {
	return pitch_modulation_depth > 0;
}

double SiOPMChannelParams::get_master_volume(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, master_volumes.size(), 0);

	return master_volumes[p_index];
}

void SiOPMChannelParams::set_master_volume(int p_index, double p_value) {
	ERR_FAIL_INDEX(p_index, master_volumes.size());

	master_volumes.write[p_index] = p_value;
}

bool SiOPMChannelParams::has_filter() const {
	return filter_cutoff < 128 || filter_resonance > 0;
}

bool SiOPMChannelParams::has_filter_advanced() const {
	return filter_attack_rate > 0 || filter_release_rate > 0;
}

int SiOPMChannelParams::get_lfo_frame() const {
	return (int)(SiOPMRefTable::LFO_TIMER_INITIAL * 0.346938775510204 / lfo_frequency_step);
}

void SiOPMChannelParams::set_lfo_frame(int p_fps) {
	lfo_frequency_step = (int)(SiOPMRefTable::LFO_TIMER_INITIAL/(p_fps * 2.882352941176471));
}

void SiOPMChannelParams::set_lfo_time_mode(int p_value) {
	lfo_time_mode = p_value;
	// Sync the active lfo_frequency_step from the newly-active per-mode
	// field so that code reading get_lfo_frequency_step() always sees the
	// value for the current mode.
	switch (lfo_time_mode) {
		case 0:  lfo_frequency_step = lfo_rate_value; break;
		case 1:  lfo_frequency_step = lfo_time_value; break;
		default: lfo_frequency_step = lfo_beat_division; break;
	}
}

void SiOPMChannelParams::set_lfo_rate_value(int p_value) {
	lfo_rate_value = p_value;
	if (lfo_time_mode == 0) {
		lfo_frequency_step = p_value;
	}
}

void SiOPMChannelParams::set_lfo_time_value(int p_value) {
	lfo_time_value = p_value;
	if (lfo_time_mode == 1) {
		lfo_frequency_step = p_value;
	}
}

void SiOPMChannelParams::set_lfo_beat_value(int p_value) {
	lfo_beat_division = p_value;
	if (lfo_time_mode >= 2) {
		lfo_frequency_step = p_value;
	}
}

void SiOPMChannelParams::set_by_opm_register(int p_channel, int p_address, int p_data) {
	if (p_address < 0x20) { // Module parameter
		switch (p_address) {
			case 15: { // NOIZE:7 FREQ:4-0 for channel#7
				if (p_channel == 7 && (p_data & 128)) {
					operator_params[3]->pulse_generator_type = SiONPulseGeneratorType::PULSE_NOISE_PULSE;
					operator_params[3]->pitch_table_type = SiONPitchTableType::PITCH_TABLE_OPM_NOISE;
					operator_params[3]->fixed_pitch = ((p_data & 31) << 6) + 2048;
				}
			} break;

			case 24: { // LFO FREQ:7-0 for all 8 channels
				lfo_frequency_step = SiOPMRefTable::get_instance()->lfo_timer_steps[p_data];
			} break;

			case 25: { // A(0)/P(1):7 DEPTH:6-0 for all 8 channels
				if (p_data & 128) {
					pitch_modulation_depth = p_data & 127;
				} else {
					amplitude_modulation_depth = p_data & 127;
				}
			} break;

			case 27: { // LFO WS:10 for all 8 channels
				lfo_wave_shape = p_data & 3;
			} break;
		}
	} else if (p_channel == (p_address & 7)) {
		if (p_address < 0x40) { // Channel parameter
			switch ((p_address - 0x20) >> 3) {
				case 0: { // L:7 R:6 FB:5-3 ALG:2-0
					algorithm = p_data & 7;
					feedback = (p_data >> 3) & 7;

					int value = p_data >> 6;
					master_volumes.write[0] = (value != 0 ? 0.5 : 0);
					pan = (value == 1 ? 128 : (value == 2 ? 0 : 64));
				} break;

				case 1: break; // KC:6-0
				case 2: break; // KF:6-0
				case 3: break; // PMS:6-4 AMS:10
			}
		} else { // Operator parameter
			int ops[4] = { 3, 1, 2, 0 };
			int op_index = ops[(p_address >> 3) & 3];
			Ref<SiOPMOperatorParams> op_params = operator_params[op_index];

			switch ((p_address - 0x40) >> 5) {
				case 0: { // DT1:6-4 MUL:3-0
					op_params->detune1    = (p_data >> 4) & 7;
					op_params->set_multiple((p_data     ) & 15);
				} break;
				case 1: { // TL:6-0
					op_params->total_level = p_data & 127;
				} break;
				case 2: { // KS:76 AR:4-0
					op_params->key_scaling_rate = (p_data >> 6) & 3;
					op_params->attack_rate      = (p_data & 31) << 1;
				} break;
				case 3: { // AMS:7 DR:4-0
					op_params->amplitude_modulation_shift = ((p_data >> 7) & 1) << 1;
					op_params->decay_rate                  = (p_data & 31) << 1;
				} break;
				case 4: { // DT2:76 SR:4-0
					int options[4] = { 0, 384, 500, 608 };
					op_params->detune2 = options[(p_data >> 6) & 3];
					op_params->sustain_rate    = (p_data & 31) << 1;
				} break;
				case 5: { // SL:7-4 RR:3-0
					op_params->sustain_level = (p_data >> 4) & 15;
					op_params->release_rate  = (p_data & 15) << 2;
				} break;
			}
		}
	}
}

void SiOPMChannelParams::initialize() {
	operator_count = 1;

	algorithm = 0;
	feedback = 0;
	feedback_connection = 0;

	lfo_wave_shape = SiOPMRefTable::LFO_WAVE_TRIANGLE;
	lfo_frequency_step = 12126; // 12126 = 30 frames / 100 fratio
	lfo_time_mode = 0; // Default to Rate mode
	lfo_beat_division = 2; // Default to 1/4 note
	lfo_rate_value = 12126;
	lfo_time_value = 0;

	amplitude_modulation_depth = 0;
	pitch_modulation_depth = 0;
	envelope_frequency_ratio = 100;

	for (int i = 1; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
		master_volumes.write[i] = 0;
	}
	master_volumes.write[0] = 0.5;
	instrument_gain_db = 0;
	pan = 64;

	_update_carrier_mask();

	filter_type = 0;
	filter_cutoff = 128;
	filter_resonance = 0;
	filter_attack_rate = 0;
	filter_decay_rate1 = 0;
	filter_decay_rate2 = 0;
	filter_release_rate = 0;
	filter_decay_offset1 = 128;
	filter_decay_offset2 = 64;
	filter_sustain_offset = 32;
	filter_release_offset = 128;

	amplitude_attack_rate = 63;
	amplitude_decay_rate = 63;
	amplitude_sustain_level = 128;
	amplitude_release_rate = 63;

	for (int i = 0; i < MAX_OPERATORS; i++) {
		operator_params[i]->initialize();
	}

	init_sequence->clear();
}

void SiOPMChannelParams::copy_from(const Ref<SiOPMChannelParams> &p_params) {
	operator_count = p_params->operator_count;

	// Preserve Analog-Like mode flag when cloning/copying voices.
	// Without this, AL presets cloned via SiONVoice::clone() lose their
	// special processing path and sound different.
	set_analog_like(p_params->is_analog_like());

	algorithm = p_params->algorithm;
	feedback = p_params->feedback;
	feedback_connection = p_params->feedback_connection;

	lfo_wave_shape = p_params->lfo_wave_shape;
	lfo_frequency_step = p_params->lfo_frequency_step;
	lfo_time_mode = p_params->lfo_time_mode;
	lfo_beat_division = p_params->lfo_beat_division;
	lfo_rate_value = p_params->lfo_rate_value;
	lfo_time_value = p_params->lfo_time_value;

	amplitude_modulation_depth = p_params->amplitude_modulation_depth;
	pitch_modulation_depth = p_params->pitch_modulation_depth;
	envelope_frequency_ratio = p_params->envelope_frequency_ratio;

	for (int i = 1; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
		master_volumes.write[i] = p_params->master_volumes[i];
	}
	instrument_gain_db = p_params->instrument_gain_db;
	pan = p_params->pan;

	carrier_mask = p_params->carrier_mask;

	filter_type = p_params->filter_type;
	filter_cutoff = p_params->filter_cutoff;
	filter_resonance = p_params->filter_resonance;
	filter_attack_rate = p_params->filter_attack_rate;
	filter_decay_rate1 = p_params->filter_decay_rate1;
	filter_decay_rate2 = p_params->filter_decay_rate2;
	filter_release_rate = p_params->filter_release_rate;
	filter_decay_offset1 = p_params->filter_decay_offset1;
	filter_decay_offset2 = p_params->filter_decay_offset2;
	filter_sustain_offset = p_params->filter_sustain_offset;
	filter_release_offset = p_params->filter_release_offset;

	amplitude_attack_rate = p_params->amplitude_attack_rate;
	amplitude_decay_rate = p_params->amplitude_decay_rate;
	amplitude_sustain_level = p_params->amplitude_sustain_level;
	amplitude_release_rate = p_params->amplitude_release_rate;

	for (int i = 0; i < MAX_OPERATORS; i++) {
		operator_params[i]->copy_from(p_params->operator_params[i]);
	}

	init_sequence->clear();
}

String SiOPMChannelParams::_to_string() const {
	String params = "";

	params += "ops=" + itos(operator_count) + ", ";
	params += "alg=" + itos(algorithm) + ", ";
	params += "feedback=(" + itos(feedback) + ", " + itos(feedback_connection) + "), ";
	params += "fratio=" + itos(envelope_frequency_ratio) + ", ";

	const double lfo_frequency = SiOPMRefTable::LFO_TIMER_INITIAL * 0.005782313 / lfo_frequency_step;
	params += "lfo=(" + itos(lfo_wave_shape) + ", " + rtos(lfo_frequency) + "), ";

	params += "amp=" + itos(amplitude_modulation_depth) + ", ";
	params += "pitch=" + itos(pitch_modulation_depth) + ", ";
	params += "vol=" + rtos(master_volumes[0]) + ", ";
	params += "inst_gain_db=" + itos(instrument_gain_db) + ", ";
	params += "pan=" + itos(pan - 64) + ", ";

	params += "filter=(" + itos(filter_type) + ", " + itos(filter_cutoff) + ", " + itos(filter_resonance) + "), ";
	params += "frate=("   + itos(filter_attack_rate) + ", "   + itos(filter_decay_rate1) + ", "   + itos(filter_decay_rate2) + ", "    + itos(filter_release_rate) + "), ";
	params += "foffset=(" + itos(filter_decay_offset1) + ", " + itos(filter_decay_offset2) + ", " + itos(filter_sustain_offset) + ", " + itos(filter_release_offset) + ")";

	return "SiOPMChannelParams: " + params;
}

void SiOPMChannelParams::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_operator_count"), &SiOPMChannelParams::get_operator_count);
	ClassDB::bind_method(D_METHOD("set_operator_count", "value"), &SiOPMChannelParams::set_operator_count);

	ClassDB::bind_method(D_METHOD("get_operator_params", "index"), &SiOPMChannelParams::get_operator_params);

	ClassDB::bind_method(D_METHOD("is_analog_like"), &SiOPMChannelParams::is_analog_like);
	ClassDB::bind_method(D_METHOD("set_analog_like", "value"), &SiOPMChannelParams::set_analog_like);

	ClassDB::bind_method(D_METHOD("get_algorithm"), &SiOPMChannelParams::get_algorithm);
	ClassDB::bind_method(D_METHOD("set_algorithm", "value"), &SiOPMChannelParams::set_algorithm);
	ClassDB::bind_method(D_METHOD("get_feedback"), &SiOPMChannelParams::get_feedback);
	ClassDB::bind_method(D_METHOD("set_feedback", "value"), &SiOPMChannelParams::set_feedback);
	ClassDB::bind_method(D_METHOD("get_feedback_connection"), &SiOPMChannelParams::get_feedback_connection);
	ClassDB::bind_method(D_METHOD("set_feedback_connection", "value"), &SiOPMChannelParams::set_feedback_connection);

	ClassDB::bind_method(D_METHOD("get_envelope_frequency_ratio"), &SiOPMChannelParams::get_envelope_frequency_ratio);
	ClassDB::bind_method(D_METHOD("set_envelope_frequency_ratio", "value"), &SiOPMChannelParams::set_envelope_frequency_ratio);
	ClassDB::bind_method(D_METHOD("get_lfo_wave_shape"), &SiOPMChannelParams::get_lfo_wave_shape);
	ClassDB::bind_method(D_METHOD("set_lfo_wave_shape", "value"), &SiOPMChannelParams::set_lfo_wave_shape);
	ClassDB::bind_method(D_METHOD("get_lfo_frequency_step"), &SiOPMChannelParams::get_lfo_frequency_step);
	ClassDB::bind_method(D_METHOD("set_lfo_frequency_step", "value"), &SiOPMChannelParams::set_lfo_frequency_step);
	ClassDB::bind_method(D_METHOD("get_lfo_time_mode"), &SiOPMChannelParams::get_lfo_time_mode);
	ClassDB::bind_method(D_METHOD("set_lfo_time_mode", "value"), &SiOPMChannelParams::set_lfo_time_mode);

	ClassDB::bind_method(D_METHOD("get_lfo_rate_value"), &SiOPMChannelParams::get_lfo_rate_value);
	ClassDB::bind_method(D_METHOD("set_lfo_rate_value", "value"), &SiOPMChannelParams::set_lfo_rate_value);
	ClassDB::bind_method(D_METHOD("get_lfo_time_value"), &SiOPMChannelParams::get_lfo_time_value);
	ClassDB::bind_method(D_METHOD("set_lfo_time_value", "value"), &SiOPMChannelParams::set_lfo_time_value);
	ClassDB::bind_method(D_METHOD("get_lfo_beat_value"), &SiOPMChannelParams::get_lfo_beat_value);
	ClassDB::bind_method(D_METHOD("set_lfo_beat_value", "value"), &SiOPMChannelParams::set_lfo_beat_value);

	ClassDB::bind_method(D_METHOD("get_amplitude_modulation_depth"), &SiOPMChannelParams::get_amplitude_modulation_depth);
	ClassDB::bind_method(D_METHOD("set_amplitude_modulation_depth", "value"), &SiOPMChannelParams::set_amplitude_modulation_depth);
	ClassDB::bind_method(D_METHOD("get_pitch_modulation_depth"), &SiOPMChannelParams::get_pitch_modulation_depth);
	ClassDB::bind_method(D_METHOD("set_pitch_modulation_depth", "value"), &SiOPMChannelParams::set_pitch_modulation_depth);

	ClassDB::bind_method(D_METHOD("get_master_volume", "index"), &SiOPMChannelParams::get_master_volume);
	ClassDB::bind_method(D_METHOD("set_master_volume", "index", "value"), &SiOPMChannelParams::set_master_volume);
	ClassDB::bind_method(D_METHOD("get_instrument_gain_db"), &SiOPMChannelParams::get_instrument_gain_db);
	ClassDB::bind_method(D_METHOD("set_instrument_gain_db", "value"), &SiOPMChannelParams::set_instrument_gain_db);

	ClassDB::bind_method(D_METHOD("get_pan"), &SiOPMChannelParams::get_pan);
	ClassDB::bind_method(D_METHOD("set_pan", "value"), &SiOPMChannelParams::set_pan);

	ClassDB::bind_method(D_METHOD("get_filter_type"), &SiOPMChannelParams::get_filter_type);
	ClassDB::bind_method(D_METHOD("set_filter_type", "value"), &SiOPMChannelParams::set_filter_type);
	ClassDB::bind_method(D_METHOD("get_filter_cutoff"), &SiOPMChannelParams::get_filter_cutoff);
	ClassDB::bind_method(D_METHOD("set_filter_cutoff", "value"), &SiOPMChannelParams::set_filter_cutoff);
	ClassDB::bind_method(D_METHOD("get_filter_resonance"), &SiOPMChannelParams::get_filter_resonance);
	ClassDB::bind_method(D_METHOD("set_filter_resonance", "value"), &SiOPMChannelParams::set_filter_resonance);
	ClassDB::bind_method(D_METHOD("get_filter_attack_rate"), &SiOPMChannelParams::get_filter_attack_rate);
	ClassDB::bind_method(D_METHOD("set_filter_attack_rate", "value"), &SiOPMChannelParams::set_filter_attack_rate);
	ClassDB::bind_method(D_METHOD("get_filter_decay_rate1"), &SiOPMChannelParams::get_filter_decay_rate1);
	ClassDB::bind_method(D_METHOD("set_filter_decay_rate1", "value"), &SiOPMChannelParams::set_filter_decay_rate1);
	ClassDB::bind_method(D_METHOD("get_filter_decay_rate2"), &SiOPMChannelParams::get_filter_decay_rate2);
	ClassDB::bind_method(D_METHOD("set_filter_decay_rate2", "value"), &SiOPMChannelParams::set_filter_decay_rate2);
	ClassDB::bind_method(D_METHOD("get_filter_release_rate"), &SiOPMChannelParams::get_filter_release_rate);
	ClassDB::bind_method(D_METHOD("set_filter_release_rate", "value"), &SiOPMChannelParams::set_filter_release_rate);
	ClassDB::bind_method(D_METHOD("get_filter_decay_offset1"), &SiOPMChannelParams::get_filter_decay_offset1);
	ClassDB::bind_method(D_METHOD("set_filter_decay_offset1", "value"), &SiOPMChannelParams::set_filter_decay_offset1);
	ClassDB::bind_method(D_METHOD("get_filter_decay_offset2"), &SiOPMChannelParams::get_filter_decay_offset2);
	ClassDB::bind_method(D_METHOD("set_filter_decay_offset2", "value"), &SiOPMChannelParams::set_filter_decay_offset2);
	ClassDB::bind_method(D_METHOD("get_filter_sustain_offset"), &SiOPMChannelParams::get_filter_sustain_offset);
	ClassDB::bind_method(D_METHOD("set_filter_sustain_offset", "value"), &SiOPMChannelParams::set_filter_sustain_offset);
	ClassDB::bind_method(D_METHOD("get_filter_release_offset"), &SiOPMChannelParams::get_filter_release_offset);
	ClassDB::bind_method(D_METHOD("set_filter_release_offset", "value"), &SiOPMChannelParams::set_filter_release_offset);

	//

	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "operator_count"), "set_operator_count", "get_operator_count");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::BOOL, "analog_like"), "set_analog_like", "is_analog_like");

	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "algorithm"), "set_algorithm", "get_algorithm");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "feedback"), "set_feedback", "get_feedback");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "feedback_connection"), "set_feedback_connection", "get_feedback_connection");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "envelope_frequency_ratio"), "set_envelope_frequency_ratio", "get_envelope_frequency_ratio");

	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "lfo_wave_shape"), "set_lfo_wave_shape", "get_lfo_wave_shape");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "lfo_frequency_step"), "set_lfo_frequency_step", "get_lfo_frequency_step");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "lfo_time_mode"), "set_lfo_time_mode", "get_lfo_time_mode");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "lfo_rate_value"), "set_lfo_rate_value", "get_lfo_rate_value");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "lfo_time_value"), "set_lfo_time_value", "get_lfo_time_value");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "lfo_beat_value"), "set_lfo_beat_value", "get_lfo_beat_value");

	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "amplitude_modulation_depth"), "set_amplitude_modulation_depth", "get_amplitude_modulation_depth");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "pitch_modulation_depth"), "set_pitch_modulation_depth", "get_pitch_modulation_depth");

	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "instrument_gain_db"), "set_instrument_gain_db", "get_instrument_gain_db");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "pan"), "set_pan", "get_pan");

	ClassDB::bind_method(D_METHOD("get_carrier_mask"), &SiOPMChannelParams::get_carrier_mask);
	ClassDB::bind_method(D_METHOD("set_carrier_mask", "mask"), &SiOPMChannelParams::set_carrier_mask);
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::PACKED_INT32_ARRAY, "carrier_mask"), "set_carrier_mask", "get_carrier_mask");

	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_type"), "set_filter_type", "get_filter_type");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_cutoff"), "set_filter_cutoff", "get_filter_cutoff");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_resonance"), "set_filter_resonance", "get_filter_resonance");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_attack_rate"), "set_filter_attack_rate", "get_filter_attack_rate");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_decay_rate1"), "set_filter_decay_rate1", "get_filter_decay_rate1");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_decay_rate2"), "set_filter_decay_rate2", "get_filter_decay_rate2");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_release_rate"), "set_filter_release_rate", "get_filter_release_rate");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_decay_offset1"), "set_filter_decay_offset1", "get_filter_decay_offset1");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_decay_offset2"), "set_filter_decay_offset2", "get_filter_decay_offset2");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_sustain_offset"), "set_filter_sustain_offset", "get_filter_sustain_offset");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "filter_release_offset"), "set_filter_release_offset", "get_filter_release_offset");

	ClassDB::bind_method(D_METHOD("get_amplitude_attack_rate"), &SiOPMChannelParams::get_amplitude_attack_rate);
	ClassDB::bind_method(D_METHOD("set_amplitude_attack_rate", "value"), &SiOPMChannelParams::set_amplitude_attack_rate);
	ClassDB::bind_method(D_METHOD("get_amplitude_decay_rate"), &SiOPMChannelParams::get_amplitude_decay_rate);
	ClassDB::bind_method(D_METHOD("set_amplitude_decay_rate", "value"), &SiOPMChannelParams::set_amplitude_decay_rate);
	ClassDB::bind_method(D_METHOD("get_amplitude_sustain_level"), &SiOPMChannelParams::get_amplitude_sustain_level);
	ClassDB::bind_method(D_METHOD("set_amplitude_sustain_level", "value"), &SiOPMChannelParams::set_amplitude_sustain_level);
	ClassDB::bind_method(D_METHOD("get_amplitude_release_rate"), &SiOPMChannelParams::get_amplitude_release_rate);
	ClassDB::bind_method(D_METHOD("set_amplitude_release_rate", "value"), &SiOPMChannelParams::set_amplitude_release_rate);

	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "amplitude_attack_rate"), "set_amplitude_attack_rate", "get_amplitude_attack_rate");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "amplitude_decay_rate"), "set_amplitude_decay_rate", "get_amplitude_decay_rate");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "amplitude_sustain_level"), "set_amplitude_sustain_level", "get_amplitude_sustain_level");
	ClassDB::add_property("SiOPMChannelParams", PropertyInfo(Variant::INT, "amplitude_release_rate"), "set_amplitude_release_rate", "get_amplitude_release_rate");

	BIND_CONSTANT(MAX_OPERATORS);
}

SiOPMChannelParams::SiOPMChannelParams() {
	init_sequence = memnew(MMLSequence);
	master_volumes.clear();
	master_volumes.resize_zeroed(SiOPMSoundChip::STREAM_SEND_SIZE);

	operator_params.clear();
	for (int i = 0; i < MAX_OPERATORS; i++) {
		operator_params.push_back(memnew(SiOPMOperatorParams));
	}

	initialize();
}

SiOPMChannelParams::~SiOPMChannelParams() {
	memdelete(init_sequence);
	init_sequence = nullptr;

	operator_params.clear();
}
