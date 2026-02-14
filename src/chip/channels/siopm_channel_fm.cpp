/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_channel_fm.h"

#include <godot_cpp/core/class_db.hpp>
#include "chip/channels/siopm_operator.h"
#include "chip/siopm_channel_params.h"
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"
#include "chip/wave/siopm_wave_pcm_data.h"
#include "chip/wave/siopm_wave_pcm_table.h"
#include "chip/wave/siopm_wave_table.h"
#include "utils/godot_util.h"
#include <algorithm>
#include <cmath>

List<SiOPMOperator *> SiOPMChannelFM::_operator_pool;

// Forward declaration of helper (definition further below).
static _FORCE_INLINE_ int _safe_log_lookup(class SiOPMRefTable *p_table, int p_index);

void SiOPMChannelFM::finalize_pool() {
	for (SiOPMOperator *op : _operator_pool) {
		memdelete(op);
	}
	_operator_pool.clear();
}

SiOPMOperator *SiOPMChannelFM::_alloc_operator() {
	// Try to reuse an operator from the pool, but skip over any null pointers
	// that might have slipped in due to previous erroneous releases. This
	// prevents the caller from receiving a nullptr which would later crash when
	// methods are invoked on it.
	while (_operator_pool.size() > 0) {
		SiOPMOperator *op = _operator_pool.back()->get();
		_operator_pool.pop_back();
		if (op != nullptr) {
			// CRITICAL: Update the sound chip pointer when reusing from pool.
			// After a buffer size change, the old sound chip is destroyed
			// and a new one is created, so pooled operators have stale pointers.
			op->update_sound_chip(_sound_chip);
			return op;
		}
		// If the stored pointer was null just continue the loop to fetch the next
		// available element.
	}
	// Pool exhausted – create a fresh operator instance.
	return memnew(SiOPMOperator(_sound_chip));
}

void SiOPMChannelFM::_release_operator(SiOPMOperator *p_operator) {
	// Safeguard against accidental null pushes that could later be popped and
	// dereferenced, leading to crashes such as the null-`this` access seen in
	// SiOPMOperator::note_on().
	if (unlikely(p_operator == nullptr)) {
		return; // Nothing to release.
	}
	_operator_pool.push_back(p_operator);
}

//

void SiOPMChannelFM::_update_process_function() {
	_process_function = _process_function_list[_lfo_on][_process_function_type];
}

void SiOPMChannelFM::_update_operator_count(int p_count) {
	// CORRECT FIX: All 4 operators are allocated at construction time.
	// This function ONLY updates the logical count - zero allocation, zero deletion.
	// This eliminates ALL lifecycle races with the audio thread.
	
	// Re-initialize operators that are becoming active
	if (_operator_count < p_count) {
		for (int i = _operator_count; i < p_count; i++) {
			_operators[i]->initialize();
		}
	}
	// Reset operators that are becoming inactive to ensure they produce zero output
	else if (_operator_count > p_count) {
		for (int i = p_count; i < _operator_count; i++) {
			_operators[i]->reset(); // Sets EG to OFF, zeroes phase
		}
	}
	
	// Just update the logical count - operators always exist
	_operator_count = p_count;
	_process_function_type = (ProcessType)(p_count - 1);
	_update_process_function();

	_active_operator = _operators[_operator_count - 1];

	if (_input_mode == INPUT_FEEDBACK) {
		set_feedback(0, 0);
	}
}

//

void SiOPMChannelFM::get_channel_params(const Ref<SiOPMChannelParams> &p_params) const {
	p_params->set_operator_count(_operator_count);

	p_params->set_algorithm(_algorithm);
	p_params->set_envelope_frequency_ratio(_frequency_ratio);

	p_params->set_feedback(0);
	p_params->set_feedback_connection(0);
	for (int i = 0; i < _operator_count; i++) {
		if (_in_pipe == _operators[i]->get_feed_pipe()) {
			p_params->set_feedback(_input_level - 6);
			p_params->set_feedback_connection(i);
			break;
		}
	}

	p_params->set_lfo_wave_shape(_lfo_wave_shape);
	// Set mode first so that per-mode setters sync lfo_frequency_step correctly.
	p_params->set_lfo_time_mode(get_lfo_time_mode());
	switch (get_lfo_time_mode()) {
		case LFO_TIME_MODE_RATE:
			p_params->set_lfo_rate_value(_lfo_timer_step_buffer);
			break;
		case LFO_TIME_MODE_TIME:
			p_params->set_lfo_time_value(_lfo_timer_step_buffer);
			break;
		default:
			p_params->set_lfo_beat_value(_lfo_beat_division);
			break;
	}

	p_params->set_amplitude_modulation_depth(_amplitude_modulation_depth);
	p_params->set_pitch_modulation_depth(_pitch_modulation_depth);

	for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
		p_params->set_master_volume(i, _volumes[i]);
	}
	p_params->set_instrument_gain_db(get_instrument_gain_db());
	p_params->set_pan(_pan);

	for (int i = 0; i < _operator_count; i++) {
		_operators[i]->get_operator_params(p_params->get_operator_params(i));
	}

	// Build carrier mask reflecting current routing
	PackedInt32Array mask;
	mask.resize(_operator_count);
	for (int i = 0; i < _operator_count; i++) {
		mask.set(i, _operators[i]->is_final() ? 1 : 0);
	}
	p_params->set_carrier_mask(mask);
}

void SiOPMChannelFM::set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation) {
	if (p_params->get_operator_count() == 0) {
		return;
	}

	set_algorithm(p_params->get_operator_count(), p_params->is_analog_like(), p_params->get_algorithm());
	set_frequency_ratio(p_params->get_envelope_frequency_ratio());
	set_feedback(p_params->get_feedback(), p_params->get_feedback_connection());

	if (p_with_modulation) {
		initialize_lfo(p_params->get_lfo_wave_shape());

		set_lfo_time_mode(p_params->get_lfo_time_mode());
		switch (p_params->get_lfo_time_mode()) {
			case LFO_TIME_MODE_RATE:
				set_lfo_frequency_step(p_params->get_lfo_rate_value());
				break;
			case LFO_TIME_MODE_TIME:
				set_lfo_frequency_step(p_params->get_lfo_time_value());
				break;
			default:
				set_lfo_frequency_step(p_params->get_lfo_beat_value());
				break;
		}

		set_amplitude_modulation(p_params->get_amplitude_modulation_depth());
		set_pitch_modulation(p_params->get_pitch_modulation_depth());
	}

	if (p_with_volume) {
		for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			_volumes.write[i] = p_params->get_master_volume(i);
		}

		_has_effect_send = false;
		for (int i = 1; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			if (_volumes[i] > 0) {
				_has_effect_send = true;
				break;
			}
		}

		_pan = p_params->get_pan();
	}
	set_instrument_gain_db(p_params->get_instrument_gain_db());

	_filter_type = p_params->get_filter_type();
	{
		int filter_cutoff = p_params->get_filter_cutoff();
		int filter_resonance = p_params->get_filter_resonance();
		int filter_ar = p_params->get_filter_attack_rate();
		int filter_dr1 = p_params->get_filter_decay_rate1();
		int filter_dr2 = p_params->get_filter_decay_rate2();
		int filter_rr = p_params->get_filter_release_rate();
		int filter_dc1 = p_params->get_filter_decay_offset1();
		int filter_dc2 = p_params->get_filter_decay_offset2();
		int filter_sc = p_params->get_filter_sustain_offset();
		int filter_rc = p_params->get_filter_release_offset();
		set_sv_filter(filter_cutoff, filter_resonance, filter_ar, filter_dr1, filter_dr2, filter_rr, filter_dc1, filter_dc2, filter_sc, filter_rc);
	}

	for (int i = 0; i < _operator_count; i++) {
		_operators[i]->set_operator_params(p_params->get_operator_params(i));
	}
}

void SiOPMChannelFM::set_params_by_value(int p_ar, int p_dr, int p_sr, int p_rr, int p_sl, int p_tl, int p_ksr, int p_ksl, int p_mul, int p_dt1, int p_dt2, int p_ams, int p_phase, int p_fix_note) {
#define SET_OP_PARAM(m_setter, m_value)      \
	if (m_value != INT32_MIN) {              \
		_active_operator->m_setter(m_value); \
	}

	SET_OP_PARAM(set_attack_rate,                p_ar);
	SET_OP_PARAM(set_decay_rate,                 p_dr);
	SET_OP_PARAM(set_sustain_rate,               p_sr);
	SET_OP_PARAM(set_release_rate,               p_rr);
	SET_OP_PARAM(set_sustain_level,              p_sl);
	SET_OP_PARAM(set_total_level,                p_tl);
	SET_OP_PARAM(set_key_scaling_rate,           p_ksr);
	SET_OP_PARAM(set_key_scaling_level,          p_ksl);
	SET_OP_PARAM(set_multiple,                   p_mul);
	SET_OP_PARAM(set_detune1,                    p_dt1);
	SET_OP_PARAM(set_ptss_detune,                p_dt2);
	SET_OP_PARAM(set_amplitude_modulation_shift, p_ams);
	SET_OP_PARAM(set_key_on_phase,               p_phase);

	if (p_fix_note != INT32_MIN) {
		_active_operator->set_fixed_pitch_index(p_fix_note << 6);
	}

#undef SET_OP_PARAM
}

void SiOPMChannelFM::set_wave_data(const Ref<SiOPMWaveBase> &p_wave_data) {
	Ref<SiOPMWavePCMData> pcm_data = p_wave_data;
	Ref<SiOPMWavePCMTable> pcm_table = p_wave_data;
	if (pcm_table.is_valid()) {
		pcm_data = pcm_table->get_note_data(60);
	}

	if (pcm_data.is_valid() && !pcm_data->get_wavelet().is_empty()) {
		// Skip operator count update if already configured for PCM with 1 operator
		// to prevent race with audio thread during rapid note triggers
		if (_operator_count != 1 || _process_function_type != PROCESS_PCM) {
			_update_operator_count(1);
			_process_function_type = PROCESS_PCM;
			_update_process_function();
		}
		_active_operator->set_pcm_data(pcm_data);
		set_envelope_reset(true);

		return;
	}

	Ref<SiOPMWaveTable> wave_table = p_wave_data;
	if (wave_table.is_valid() && !wave_table->get_wavelet().is_empty()) {
		_operators[0]->set_wave_table(wave_table);
		if (_operators[1]) {
			_operators[1]->set_wave_table(wave_table);
		}
		if (_operators[2]) {
			_operators[2]->set_wave_table(wave_table);
		}
		if (_operators[3]) {
			_operators[3]->set_wave_table(wave_table);
		}

		return;
	}
}

void SiOPMChannelFM::set_channel_number(int p_value) {
	_register_map_channel = p_value;
}

void SiOPMChannelFM::_set_by_opm_register(int p_address, int p_data) {
	static int _pmd = 0;
	static int _amd = 0;

	if (p_address < 0x20) { // Module parameter
		switch (p_address) {
			case 15: { // NOIZE:7 FREQ:4-0 for channel#7
				if (_register_map_channel == 7 && _operator_count == 4 && (p_data & 128)) {
					_operators[3]->set_pulse_generator_type(SiONPulseGeneratorType::PULSE_NOISE_PULSE);
					_operators[3]->set_pitch_table_type(SiONPitchTableType::PITCH_TABLE_OPM_NOISE);
					_operators[3]->set_pitch_index(((p_data & 31) << 6) + 2048);
				}
			} break;

			case 24: { // LFO FREQ:7-0 for all 8 channels
				_set_lfo_timer(_table->lfo_timer_steps[p_data]);
			} break;

			case 25: { // A(0)/P(1):7 DEPTH:6-0 for all 8 channels
				// NOTE: Original code has pitch and amp the other way around, which is inconsistent with the rest of the code.
				if (p_data & 128) {
					_pmd = p_data & 127;
				} else {
					_amd = p_data & 127;
				}
			} break;

			case 27: { // LFO WS:10 for all 8 channels
				initialize_lfo(p_data & 3);
			} break;
		}
	} else if (_register_map_channel == (p_address & 7)) {
		if (p_address < 0x40) { // Channel parameter
			switch ((p_address - 0x20) >> 3) {
				case 0: { // L:7 R:6 FB:5-3 ALG:2-0
					set_algorithm(4, false, p_data & 7);
					set_feedback((p_data >> 3) & 7, 0);

					int value = p_data >> 6;
					_volumes.write[0] = (value != 0 ? 0.5 : 0);
					_pan = (value == 1 ? 128 : (value == 2 ? 0 : 64));
				} break;

				case 1: { // KC:6-0
					for (int i = 0; i < 4; i++) {
						_operators[i]->set_key_code(p_data & 127);
					}
				} break;

				case 2: { // KF:6-0
					for (int i = 0; i < 4; i++) {
						_operators[i]->set_key_fraction(p_data & 127);
					}
				} break;

				case 3: { // PMS:6-4 AMS:10
					int pitch_mod_shift = (p_data >> 4) & 7;
					int amplitude_mod_shift = p_data & 3;

					if (p_data & 128) {
						set_pitch_modulation(pitch_mod_shift < 6 ? (_pmd >> (6 - pitch_mod_shift)) : (_pmd << (pitch_mod_shift - 5)));
					} else {
						set_amplitude_modulation(amplitude_mod_shift > 0 ? (_amd << (amplitude_mod_shift - 1)) : 0);
					}
				} break;
			}
		} else { // Operator parameter
			int ops[4] = { 0, 2, 1, 3 }; // NOTE: This is the opposite of SiOPMChannelParams.
			int op_index = ops[(p_address >> 3) & 3];
			SiOPMOperator *op = _operators[op_index];

			switch ((p_address - 0x40) >> 5) {
				case 0: { // DT1:6-4 MUL:3-0
					op->set_detune1((p_data >> 4) & 7);
					op->set_multiple(p_data & 15);
				} break;
				case 1: { // TL:6-0
					op->set_total_level(p_data & 127);
				} break;
				case 2: { // KS:76 AR:4-0
					op->set_key_scaling_rate((p_data >> 6) & 3);
					op->set_attack_rate((p_data & 31) << 1);
				} break;
				case 3: { // AMS:7 DR:4-0
					op->set_amplitude_modulation_shift(((p_data >> 7) & 1) << 1);
					op->set_decay_rate((p_data & 31) << 1);
				} break;
				case 4: { // DT2:76 SR:4-0
					int options[4] = { 0, 384, 500, 608 };
					op->set_ptss_detune(options[(p_data >> 6) & 3]);
					op->set_sustain_rate((p_data & 31) << 1);
				} break;
				case 5: { // SL:7-4 RR:3-0
					op->set_sustain_level((p_data >> 4) & 15);
					op->set_release_rate((p_data & 15) << 2);
				} break;
			}
		}
	}
}

void SiOPMChannelFM::set_register(int p_address, int p_data) {
	switch (_register_map_type) {
		case REGISTER_OPM: {
			_set_by_opm_register(p_address, p_data);
		} break;

		default: break;
	}
}

void SiOPMChannelFM::_set_algorithm_operator1(int p_algorithm) {
	_update_operator_count(1);
	_algorithm = p_algorithm;

	_operators[0]->set_pipes(_pipe0, nullptr, true);
}

void SiOPMChannelFM::_set_algorithm_operator2(int p_algorithm) {
	_update_operator_count(2);
	_algorithm = p_algorithm;

	switch (_algorithm) {
		case 0: // OPL3/MA3:con=0, OPX:con=0, 1(fbc=1)
			// o1(o0)
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0, _pipe0, true);
			break;
		case 1: // OPL3/MA3:con=1, OPX:con=2
			// o0+o1
			_operators[0]->set_pipes(_pipe0, nullptr, true);
			_operators[1]->set_pipes(_pipe0, nullptr, true);
			break;
		case 2: // OPX:con=3
			// o0+o1(o0)
			_operators[0]->set_pipes(_pipe0, nullptr,   true);
			_operators[1]->set_pipes(_pipe0, _pipe0, true);
			_operators[1]->set_base_pipe(_pipe0);
			break;
		default:
			// o0+o1
			_operators[0]->set_pipes(_pipe0, nullptr, true);
			_operators[1]->set_pipes(_pipe0, nullptr, true);
			break;
	}
}

void SiOPMChannelFM::_set_algorithm_operator3(int p_algorithm) {
	_update_operator_count(3);
	_algorithm = p_algorithm;

	switch (_algorithm) {
		case 0: // OPX:con=0, 1(fbc=1)
			// o2(o1(o0))
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0, _pipe0);
			_operators[2]->set_pipes(_pipe0, _pipe0, true);
			break;
		case 1: // OPX:con=2
			// o2(o0+o1)
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0);
			_operators[2]->set_pipes(_pipe0, _pipe0, true);
			break;
		case 2: // OPX:con=3
			// o0+o2(o1)
			_operators[0]->set_pipes(_pipe0, nullptr,   true);
			_operators[1]->set_pipes(_pipe1);
			_operators[2]->set_pipes(_pipe0, _pipe1, true);
			break;
		case 3: // OPX:con=4, 5(fbc=1)
			// o1(o0)+o2
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0, _pipe0, true);
			_operators[2]->set_pipes(_pipe0, nullptr,   true);
			break;
		case 4:
			// o1(o0)+o2(o0)
			_operators[0]->set_pipes(_pipe1);
			_operators[1]->set_pipes(_pipe0, _pipe1, true);
			_operators[2]->set_pipes(_pipe0, _pipe1, true);
			break;
		case 5: // OPX:con=6
			// o0+o1+o2
			_operators[0]->set_pipes(_pipe0, nullptr, true);
			_operators[1]->set_pipes(_pipe0, nullptr, true);
			_operators[2]->set_pipes(_pipe0, nullptr, true);
			break;
		case 6: // OPX:con=7
			// o0+o1(o0)+o2
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0, _pipe0, true);
			_operators[1]->set_base_pipe(_pipe0);
			_operators[2]->set_pipes(_pipe0, nullptr,   true);
			break;
		default:
			// o0+o1+o2
			_operators[0]->set_pipes(_pipe0, nullptr, true);
			_operators[1]->set_pipes(_pipe0, nullptr, true);
			_operators[2]->set_pipes(_pipe0, nullptr, true);
			break;
	}
}

void SiOPMChannelFM::_set_algorithm_operator4(int p_algorithm) {
	_update_operator_count(4);
	_algorithm = p_algorithm;

	switch (_algorithm) {
		case 0: // OPL3:con=0, MA3:con=4, OPX:con=0, 1(fbc=1)
			// o3(o2(o1(o0)))
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0, _pipe0);
			_operators[2]->set_pipes(_pipe0, _pipe0);
			_operators[3]->set_pipes(_pipe0, _pipe0, true);
			break;
		case 1: // OPX:con=2
			// o3(o2(o0+o1))
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0);
			_operators[2]->set_pipes(_pipe0, _pipe0);
			_operators[3]->set_pipes(_pipe0, _pipe0, true);
			break;
		case 2: // MA3:con=3, OPX:con=3
			// o3(o0+o2(o1))
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe1);
			_operators[2]->set_pipes(_pipe0, _pipe1);
			_operators[3]->set_pipes(_pipe0, _pipe0, true);
			break;
		case 3: // OPX:con=4, 5(fbc=1)
			// o3(o1(o0)+o2)
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0, _pipe0);
			_operators[2]->set_pipes(_pipe0);
			_operators[3]->set_pipes(_pipe0, _pipe0, true);
			break;
		case 4: // OPL3:con=1, MA3:con=5, OPX:con=6, 7(fbc=1)
			// o1(o0)+o3(o2)
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0, _pipe0, true);
			_operators[2]->set_pipes(_pipe1);
			_operators[3]->set_pipes(_pipe0, _pipe1, true);
			break;
		case 5: // OPX:con=12
			// o1(o0)+o2(o0)+o3(o0)
			_operators[0]->set_pipes(_pipe1);
			_operators[1]->set_pipes(_pipe0, _pipe1, true);
			_operators[2]->set_pipes(_pipe0, _pipe1, true);
			_operators[3]->set_pipes(_pipe0, _pipe1, true);
			break;
		case 6: // OPX:con=10, 11(fbc=1)
			// o1(o0)+o2+o3
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0, _pipe0, true);
			_operators[2]->set_pipes(_pipe0, nullptr,   true);
			_operators[3]->set_pipes(_pipe0, nullptr,   true);
			break;
		case 7: // MA3:con=2, OPX:con=15
			// o0+o1+o2+o3
			_operators[0]->set_pipes(_pipe0, nullptr, true);
			_operators[1]->set_pipes(_pipe0, nullptr, true);
			_operators[2]->set_pipes(_pipe0, nullptr, true);
			_operators[3]->set_pipes(_pipe0, nullptr, true);
			break;
		case 8: // OPL3:con=2, MA3:con=6, OPX:con=8
			// o0+o3(o2(o1))
			_operators[0]->set_pipes(_pipe0, nullptr,   true);
			_operators[1]->set_pipes(_pipe1);
			_operators[2]->set_pipes(_pipe1, _pipe1);
			_operators[3]->set_pipes(_pipe0, _pipe1, true);
			break;
		case 9: // OPL3:con=3, MA3:con=7, OPX:con=13
			// o0+o2(o1)+o3
			_operators[0]->set_pipes(_pipe0, nullptr,   true);
			_operators[1]->set_pipes(_pipe1);
			_operators[2]->set_pipes(_pipe0, _pipe1, true);
			_operators[3]->set_pipes(_pipe0, nullptr,   true);
			break;
		case 10: // for DX7 emulation
			// o3(o0+o1+o2)
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0);
			_operators[2]->set_pipes(_pipe0);
			_operators[3]->set_pipes(_pipe0, _pipe0, true);
			break;
		case 11: // OPX:con=9
			// o0+o3(o1+o2)
			_operators[0]->set_pipes(_pipe0, nullptr,   true);
			_operators[1]->set_pipes(_pipe1);
			_operators[2]->set_pipes(_pipe1);
			_operators[3]->set_pipes(_pipe0, _pipe1, true);
			break;
		case 12: // OPX:con=14
			// o0+o1(o0)+o3(o2)
			_operators[0]->set_pipes(_pipe0);
			_operators[1]->set_pipes(_pipe0, _pipe0, true);
			_operators[1]->set_base_pipe(_pipe0);
			_operators[2]->set_pipes(_pipe1);
			_operators[3]->set_pipes(_pipe0, _pipe1, true);
			break;
		default:
			// o0+o1+o2+o3
			_operators[0]->set_pipes(_pipe0, nullptr, true);
			_operators[1]->set_pipes(_pipe0, nullptr, true);
			_operators[2]->set_pipes(_pipe0, nullptr, true);
			_operators[3]->set_pipes(_pipe0, nullptr, true);
			break;
	}
}

void SiOPMChannelFM::_set_algorithm_analog_like(int p_algorithm) {
	int target_algorithm = (p_algorithm >= 0 && p_algorithm <= 3) ? p_algorithm : 0;
	ProcessType target_process_type = (ProcessType)(PROCESS_ANALOG_LIKE + target_algorithm);
	
	// Skip if already configured for this analog-like algorithm to prevent redundant updates
	if (_operator_count == 2 && _algorithm == target_algorithm && _process_function_type == target_process_type) {
		return;
	}
	
	_update_operator_count(2);
	_operators[0]->set_pipes(_pipe0, nullptr, true);
	_operators[1]->set_pipes(_pipe0, nullptr, true);

	_algorithm = target_algorithm;
	_process_function_type = target_process_type;
	_update_process_function();
}

void SiOPMChannelFM::set_algorithm(int p_operator_count, bool p_analog_like, int p_algorithm) {
	// Skip if algorithm hasn't changed - prevents race conditions with audio thread
	// during rapid note triggers with the same voice
	bool is_analog_like_now = (_process_function_type >= PROCESS_ANALOG_LIKE && _process_function_type < PROCESS_ANALOG_LIKE + 4);
	if (p_analog_like == is_analog_like_now && _operator_count == p_operator_count && _algorithm == p_algorithm) {
		return;
	}


	if (p_analog_like) {
		_set_algorithm_analog_like(p_algorithm);
		return;
	}

	switch (p_operator_count) {
		case 1:
			_set_algorithm_operator1(p_algorithm);
			break;
		case 2:
			_set_algorithm_operator2(p_algorithm);
			break;
		case 3:
			_set_algorithm_operator3(p_algorithm);
			break;
		case 4:
			_set_algorithm_operator4(p_algorithm);
			break;
		default:
			ERR_FAIL_MSG("SiOPMChannelFM: Invalid number of operators.");
	}
}

void SiOPMChannelFM::set_feedback(int p_level, int p_connection) {
	if (p_level > 0) {
		// Connect the feedback pipe.
		if (p_connection < 0 || p_connection >= _operator_count) {
			p_connection = 0;
		}

		_in_pipe = _operators[p_connection]->get_feed_pipe();
		_in_pipe->get()->value = 0;
		_input_level = p_level + 6;
		_input_mode = INPUT_FEEDBACK;
	} else {
		// Disable feedback.
		_in_pipe = _sound_chip->get_zero_buffer();
		_input_level = 0;
		_input_mode = INPUT_ZERO;
	}
}

void SiOPMChannelFM::set_parameters(Vector<int> p_params) {
	set_params_by_value(
			p_params[1],  p_params[2],  p_params[3],  p_params[4],  p_params[5],
			p_params[6],  p_params[7],  p_params[8],  p_params[9],  p_params[10],
			p_params[11], p_params[12], p_params[13], p_params[14]
	);
}

void SiOPMChannelFM::set_types(int p_pg_type, SiONPitchTableType p_pt_type) {
	if (p_pg_type >= SiONPulseGeneratorType::PULSE_PCM) {
		Ref<SiOPMWavePCMTable> pcm_table = _table->get_pcm_data(p_pg_type - SiONPulseGeneratorType::PULSE_PCM);
		if (pcm_table.is_valid()) {
			set_wave_data(pcm_table);
		}
	} else {
		_active_operator->set_pulse_generator_type(p_pg_type);
		_active_operator->set_pitch_table_type(p_pt_type);
		_update_process_function();
	}
}

void SiOPMChannelFM::set_all_attack_rate(int p_value) {
	for (int i = 0; i < _operator_count; i++) {
		SiOPMOperator *op = _operators[i];
		if (op->is_final()) {
			op->set_attack_rate(p_value);
		}
	}
}

void SiOPMChannelFM::set_all_release_rate(int p_value) {
	for (int i = 0; i < _operator_count; i++) {
		SiOPMOperator *op = _operators[i];
		if (op->is_final()) {
			op->set_release_rate(p_value);
		}
	}
}

int SiOPMChannelFM::get_pitch() const {
	return _operators[_operator_count - 1]->get_pitch_index();
}

void SiOPMChannelFM::set_pitch(int p_value) {
	for (int i = 0; i < _operator_count; i++) {
		_operators[i]->set_pitch_index(p_value);
	}
}

void SiOPMChannelFM::set_active_operator_index(int p_value) {
	int index = CLAMP(p_value, 0, _operator_count - 1);
	_active_operator = _operators[index];
}

void SiOPMChannelFM::set_release_rate(int p_value) {
	_active_operator->set_release_rate(p_value);
}

void SiOPMChannelFM::set_total_level(int p_value) {
	_active_operator->set_total_level(p_value);
}

void SiOPMChannelFM::set_fine_multiple(int p_value) {
	_active_operator->set_fine_multiple(p_value);
}

void SiOPMChannelFM::set_attack_rate(int p_value) {
	_active_operator->set_attack_rate(p_value);
}

void SiOPMChannelFM::set_decay_rate(int p_value) {
	_active_operator->set_decay_rate(p_value);
}

void SiOPMChannelFM::set_sustain_rate(int p_value) {
	_active_operator->set_sustain_rate(p_value);
}

void SiOPMChannelFM::set_sustain_level(int p_value) {
	_active_operator->set_sustain_level(p_value);
}

void SiOPMChannelFM::set_multiple(int p_value) {
	_active_operator->set_multiple(p_value);
}

void SiOPMChannelFM::set_detune1(int p_value) {
	_active_operator->set_detune1(p_value);
}

void SiOPMChannelFM::set_operator_super_count(int p_value) {
	_active_operator->set_super_wave(p_value, _active_operator->get_super_spread());
}

void SiOPMChannelFM::set_operator_super_spread(int p_value) {
	_active_operator->set_super_wave(_active_operator->get_super_count(), p_value);
}

void SiOPMChannelFM::set_operator_super_stereo_spread(int p_value) {
	_active_operator->set_super_stereo_spread(p_value);
}

void SiOPMChannelFM::set_mute(bool p_mute) {
	_active_operator->set_mute(p_mute);
}

void SiOPMChannelFM::set_envelope_reset_on_attack(bool p_reset) {
	_active_operator->set_envelope_reset_on_attack(p_reset);
}

bool SiOPMChannelFM::_is_stereo_super_mode() const {
	// Stereo super mode is active when:
	// 1. We're in single-operator mode (PROCESS_OP1)
	// 2. The active operator has stereo spread enabled
	// 3. The operator has more than 1 super voice
	if (_process_function_type != PROCESS_OP1) {
		return false;
	}
	if (!_active_operator) {
		return false;
	}
	return _active_operator->get_super_stereo_spread() > 0 && _active_operator->get_super_count() > 1;
}

void SiOPMChannelFM::set_phase(int p_value) {
	_active_operator->set_key_on_phase(p_value);
}

void SiOPMChannelFM::set_detune(int p_value) {
	_active_operator->set_ptss_detune(p_value);
}

void SiOPMChannelFM::set_fixed_pitch(int p_value) {
	_active_operator->set_fixed_pitch_index(p_value);
}

void SiOPMChannelFM::set_ssg_envelope_control(int p_value) {
	_active_operator->set_ssg_type(p_value);
}

void SiOPMChannelFM::set_envelope_reset(bool p_reset) {
	for (int i = 0; i < _operator_count; i++) {
		_operators[i]->set_envelope_reset_on_attack(p_reset);
	}
}


// Volume control.

void SiOPMChannelFM::offset_volume(int p_expression, int p_velocity) {
	int expression_index = p_expression << 1;
	int offset = _expression_table[expression_index] + _velocity_table[p_velocity];

	for (int i = 0; i < _operator_count; i++) {
		SiOPMOperator *op = _operators[i];

		if (op->is_final()) {
			op->offset_total_level(offset);
		} else {
			op->offset_total_level(0);
		}
	}
}

// LFO control.

void SiOPMChannelFM::_set_lfo_state(bool p_enabled) {
	_lfo_on = (int)p_enabled;
	_update_process_function();

	_lfo_timer_step = p_enabled ? _lfo_timer_step_buffer : 0;
}

void SiOPMChannelFM::_set_lfo_timer(int p_value) {
	_lfo_timer = (p_value > 0 ? 1 : 0);
	_lfo_timer_step = p_value;
	_lfo_timer_step_buffer = p_value;
}

void SiOPMChannelFM::set_frequency_ratio(int p_ratio) {
	_frequency_ratio = p_ratio;

	double value_coef = (p_ratio != 0) ? (100.0 / p_ratio) : 1.0;
	_eg_timer_initial = (int)(SiOPMRefTable::ENV_TIMER_INITIAL * value_coef);
	_lfo_timer_initial = (int)(SiOPMRefTable::LFO_TIMER_INITIAL * value_coef);
}

void SiOPMChannelFM::initialize_lfo(int p_waveform, Vector<int> p_custom_wave_table) {
	SiOPMChannelBase::initialize_lfo(p_waveform, p_custom_wave_table);

	_set_lfo_state(false);

	_amplitude_modulation_depth = 0;
	_pitch_modulation_depth = 0;
	_amplitude_modulation_output_level = 0;
	_pitch_modulation_output_level = 0;

	for (SiOPMOperator *op : _operators) {
		if (op) {
			op->set_pm_detune(0);
		}
	}
}

void SiOPMChannelFM::set_amplitude_modulation(int p_depth) {
	_amplitude_modulation_depth = p_depth << 2;
	_amplitude_modulation_output_level = (_lfo_wave_table[_lfo_phase] * _amplitude_modulation_depth) >> 7 << 3;

	_set_lfo_state(_pitch_modulation_depth != 0 || _amplitude_modulation_depth != 0);
}

void SiOPMChannelFM::set_pitch_modulation(int p_depth) {
	_pitch_modulation_depth = p_depth;
	_pitch_modulation_output_level = (((_lfo_wave_table[_lfo_phase] << 1) - 255) * _pitch_modulation_depth) >> 8;

	_set_lfo_state(_pitch_modulation_depth != 0 || _amplitude_modulation_depth != 0);

	if (_pitch_modulation_depth == 0) {
		for (SiOPMOperator *op : _operators) {
			if (op) {
				op->set_pm_detune(0);
			}
		}
	}
}

// Processing.

void SiOPMChannelFM::_update_lfo(int p_op_count) {
	_lfo_timer -= _lfo_timer_step;
	if (_lfo_timer >= 0) {
		return;
	}

	_lfo_phase = (_lfo_phase + 1) & 255;

	int value_base = _lfo_wave_table[_lfo_phase];
	_amplitude_modulation_output_level = (value_base * _amplitude_modulation_depth) >> 7 << 3;
	_pitch_modulation_output_level = (((value_base << 1) - 255) * _pitch_modulation_depth) >> 8;

	if (p_op_count > 0 && _operators[0]) {
		_operators[0]->set_pm_detune(_pitch_modulation_output_level);
	}
	if (p_op_count > 1 && _operators[1]) {
		_operators[1]->set_pm_detune(_pitch_modulation_output_level);
	}
	if (p_op_count > 2 && _operators[2]) {
		_operators[2]->set_pm_detune(_pitch_modulation_output_level);
	}
	if (p_op_count > 3 && _operators[3]) {
		_operators[3]->set_pm_detune(_pitch_modulation_output_level);
	}

	_lfo_timer += _lfo_timer_initial;
}

void SiOPMChannelFM::_process_operator1_lfo_off(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	SiOPMOperator *ope0 = _operators[0];

	// Check for stereo super mode.
	bool stereo_mode = _is_stereo_super_mode();
	SinglyLinkedList<int>::Element *left_pipe = stereo_mode ? _stereo_left_pipe->get() : nullptr;
	SinglyLinkedList<int>::Element *right_pipe = stereo_mode ? _stereo_right_pipe->get() : nullptr;

	for (int i = 0; i < p_length; i++) {
		int output = 0;

		// Update EG.
		ope0->tick_eg(_eg_timer_initial);

		// Update PG.
		{
			ope0->tick_pulse_generator();

			if (stereo_mode) {
				int left_out, right_out;
				ope0->get_super_output_stereo(in_pipe->value, _input_level, 0, left_out, right_out);

				left_pipe->value = left_out + base_pipe->value;
				right_pipe->value = right_out + base_pipe->value;

				// For feedback, use mono mix.
				output = (left_out + right_out) >> 1;
				ope0->get_feed_pipe()->get()->value = output;
			} else {
				output = ope0->get_super_output(in_pipe->value, _input_level, 0);
				ope0->get_feed_pipe()->get()->value = output;
			}
		}

		// Output and increment pointers.
		{
			if (!stereo_mode) {
				out_pipe->value = output + base_pipe->value;
			}

			in_pipe = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe = out_pipe->next();
			if (stereo_mode) {
				left_pipe = left_pipe->next();
				right_pipe = right_pipe->next();
			}
		}
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
	if (stereo_mode) {
		_stereo_left_pipe->set(left_pipe);
		_stereo_right_pipe->set(right_pipe);
	}
}

void SiOPMChannelFM::_process_operator1_lfo_on(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	SiOPMOperator *ope0 = _operators[0];

	// Check for stereo super mode.
	bool stereo_mode = _is_stereo_super_mode();
	SinglyLinkedList<int>::Element *left_pipe = stereo_mode ? _stereo_left_pipe->get() : nullptr;
	SinglyLinkedList<int>::Element *right_pipe = stereo_mode ? _stereo_right_pipe->get() : nullptr;

	for (int i = 0; i < p_length; i++) {
		int output = 0;

		// Update LFO.
		_update_lfo(1);

		// Update EG.
		ope0->tick_eg(_eg_timer_initial);

		// Update PG.
		{
			ope0->tick_pulse_generator();
			int am_level = _amplitude_modulation_output_level >> ope0->get_amplitude_modulation_shift();

			if (stereo_mode) {
				int left_out, right_out;
				ope0->get_super_output_stereo(in_pipe->value, _input_level, am_level, left_out, right_out);

				left_pipe->value = left_out + base_pipe->value;
				right_pipe->value = right_out + base_pipe->value;

				// For feedback, use mono mix.
				output = (left_out + right_out) >> 1;
				ope0->get_feed_pipe()->get()->value = output;
			} else {
				output = ope0->get_super_output(in_pipe->value, _input_level, am_level);
				ope0->get_feed_pipe()->get()->value = output;
			}
		}

		// Output and increment pointers.
		{
			if (!stereo_mode) {
				out_pipe->value = output + base_pipe->value;
			}
			in_pipe = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe = out_pipe->next();
			if (stereo_mode) {
				left_pipe = left_pipe->next();
				right_pipe = right_pipe->next();
			}
		}
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
	if (stereo_mode) {
		_stereo_left_pipe->set(left_pipe);
		_stereo_right_pipe->set(right_pipe);
	}
}

void SiOPMChannelFM::_process_operator2(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	SiOPMOperator *ope0 = _operators[0];
	SiOPMOperator *ope1 = _operators[1];

	for (int i = 0; i < p_length; i++) {
		// Clear pipes.
		_pipe0->get()->value = 0;

		// Update LFO.
		_update_lfo(2);

		// Operator 0.
		{
			// Update EG.
			ope0->tick_eg(_eg_timer_initial);

			// Update PG.
			{
				ope0->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope0->get_amplitude_modulation_shift();
				int output = ope0->get_super_output(in_pipe->value, _input_level, am_level);

				ope0->get_feed_pipe()->get()->value = output;
				ope0->get_out_pipe()->get()->value  = output + ope0->get_base_pipe()->get()->value;
			}
		}

		// Operator 1.
		{
			// Update EG.
			ope1->tick_eg(_eg_timer_initial);

			// Update PG.
			{
				ope1->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope1->get_amplitude_modulation_shift();
				int output = ope1->get_super_output(ope1->get_in_pipe()->get()->value, ope1->get_fm_shift(), am_level);

				ope1->get_feed_pipe()->get()->value = output;
				ope1->get_out_pipe()->get()->value  = output + ope1->get_base_pipe()->get()->value;
			}
		}

		// Output and increment pointers.
		{
			out_pipe->value = _pipe0->get()->value + base_pipe->value;
			in_pipe = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe = out_pipe->next();
		}
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

void SiOPMChannelFM::_process_operator3(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	SiOPMOperator *ope0 = _operators[0];
	SiOPMOperator *ope1 = _operators[1];
	SiOPMOperator *ope2 = _operators[2];

	for (int i = 0; i < p_length; i++) {
		// Clear pipes.
		_pipe0->get()->value = 0;
		_pipe1->get()->value = 0;

		// Update LFO.
		_update_lfo(3);

		// Operator 0.
		{
			// Update EG.
			ope0->tick_eg(_eg_timer_initial);

			// Update PG.
			{
				ope0->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope0->get_amplitude_modulation_shift();
				int output = ope0->get_super_output(in_pipe->value, _input_level, am_level);

				ope0->get_feed_pipe()->get()->value = output;
				ope0->get_out_pipe()->get()->value  = output + ope0->get_base_pipe()->get()->value;
			}
		}

		// Operator 1.
		{
			// Update EG.
			ope1->tick_eg(_eg_timer_initial);

			// Update PG.
			{
				ope1->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope1->get_amplitude_modulation_shift();
				int output = ope1->get_super_output(ope1->get_in_pipe()->get()->value, ope1->get_fm_shift(), am_level);

				ope1->get_feed_pipe()->get()->value = output;
				ope1->get_out_pipe()->get()->value  = output + ope1->get_base_pipe()->get()->value;
			}
		}

		// Operator 2.
		{
			// Update EG.
			ope2->tick_eg(_eg_timer_initial);

			// Update PG.
			{
				ope2->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope2->get_amplitude_modulation_shift();
				int output = ope2->get_super_output(ope2->get_in_pipe()->get()->value, ope2->get_fm_shift(), am_level);

				ope2->get_feed_pipe()->get()->value = output;
				ope2->get_out_pipe()->get()->value  = output + ope2->get_base_pipe()->get()->value;
			}
		}


		// Output and increment pointers.
		{
			out_pipe->value = _pipe0->get()->value + base_pipe->value;
			in_pipe = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe = out_pipe->next();
		}
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

void SiOPMChannelFM::_process_operator4(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	SiOPMOperator *ope0 = _operators[0];
	SiOPMOperator *ope1 = _operators[1];
	SiOPMOperator *ope2 = _operators[2];
	SiOPMOperator *ope3 = _operators[3];

	for (int i = 0; i < p_length; i++) {
		// Clear pipes.
		_pipe0->get()->value = 0;
		_pipe1->get()->value = 0;

		// Update LFO.
		_update_lfo(4);

		// Operator 0.
		{
			// Update EG.
			ope0->tick_eg(_eg_timer_initial);

			// Update PG.
			{
				ope0->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope0->get_amplitude_modulation_shift();
				int output = ope0->get_super_output(in_pipe->value, _input_level, am_level);

				ope0->get_feed_pipe()->get()->value = output;
				ope0->get_out_pipe()->get()->value  = output + ope0->get_base_pipe()->get()->value;
			}
		}

		// Operator 1.
		{
			// Update EG.
			ope1->tick_eg(_eg_timer_initial);

			// Update PG.
			{
				ope1->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope1->get_amplitude_modulation_shift();
				int output = ope1->get_super_output(ope1->get_in_pipe()->get()->value, ope1->get_fm_shift(), am_level);

				ope1->get_feed_pipe()->get()->value = output;
				ope1->get_out_pipe()->get()->value  = output + ope1->get_base_pipe()->get()->value;
			}

		}

		// Operator 2.
		{
			// Update EG.
			ope2->tick_eg(_eg_timer_initial);

			// Update PG.
			{
				ope2->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope2->get_amplitude_modulation_shift();
				int output = ope2->get_super_output(ope2->get_in_pipe()->get()->value, ope2->get_fm_shift(), am_level);

				ope2->get_feed_pipe()->get()->value = output;
				ope2->get_out_pipe()->get()->value  = output + ope2->get_base_pipe()->get()->value;
			}
		}

		// Operator 3.
		{
			// Update EG.
			ope3->tick_eg(_eg_timer_initial);

			// Update PG.
			{
				ope3->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope3->get_amplitude_modulation_shift();
				int output = ope3->get_super_output(ope3->get_in_pipe()->get()->value, ope3->get_fm_shift(), am_level);

				ope3->get_feed_pipe()->get()->value = output;
				ope3->get_out_pipe()->get()->value  = output + ope3->get_base_pipe()->get()->value;
			}

		}

		// Output and increment pointers.
		{
			out_pipe->value = _pipe0->get()->value + base_pipe->value;
			in_pipe = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe = out_pipe->next();
		}
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

void SiOPMChannelFM::_process_pcm_lfo_off(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	SiOPMOperator *ope0 = _operators[0];

	for (int i = 0; i < p_length; i++) {
		int output = 0;

		// Update EG.
		ope0->tick_eg(_eg_timer_initial);

		// Update PG.
		{
			ope0->tick_pulse_generator();
			int t = (ope0->get_phase() + (in_pipe->value << _input_level)) >> ope0->get_wave_fixed_bits();

			if (t >= ope0->get_pcm_end_point()) {
				if (ope0->get_pcm_loop_point() == -1) {
					ope0->set_eg_state(SiOPMOperator::EG_OFF);
					ope0->update_eg_output();

					// Fast forward.
					for (; i < p_length; i++) {
						out_pipe->value = base_pipe->value;
						in_pipe = in_pipe->next();
						base_pipe = base_pipe->next();
						out_pipe = out_pipe->next();
					}
					break;
				} else {
					t -= ope0->get_pcm_end_point() - ope0->get_pcm_loop_point();
					int phase_diff = (ope0->get_pcm_end_point() - ope0->get_pcm_loop_point()) << ope0->get_wave_fixed_bits();
					ope0->adjust_phase(-phase_diff);
				}
			}

			int log_idx = ope0->get_wave_value_fast(t);
			log_idx += ope0->get_eg_output();
			output = _safe_log_lookup(_table, log_idx);

			ope0->get_feed_pipe()->get()->value = output;
		}

		// Output and increment pointers.
		{
			out_pipe->value = output + base_pipe->value;
			in_pipe = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe = out_pipe->next();
		}
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

void SiOPMChannelFM::_process_pcm_lfo_on(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	SiOPMOperator *ope0 = _operators[0];

	for (int i = 0; i < p_length; i++) {
		int output = 0;

		// Update LFO.
		_update_lfo(1);

		// Update EG.
		ope0->tick_eg(_eg_timer_initial);

		// Update PG.
		{
			ope0->tick_pulse_generator();
			int t = (ope0->get_phase() + (in_pipe->value<<_input_level)) >> ope0->get_wave_fixed_bits();

			if (t >= ope0->get_pcm_end_point()) {
				if (ope0->get_pcm_loop_point() == -1) {
					ope0->set_eg_state(SiOPMOperator::EG_OFF);
					ope0->update_eg_output();

					// Fast forward.
					for (; i < p_length; i++) {
						out_pipe->value = base_pipe->value;
						in_pipe = in_pipe->next();
						base_pipe = base_pipe->next();
						out_pipe = out_pipe->next();
					}
					break;
				} else {
					t -=  ope0->get_pcm_end_point() - ope0->get_pcm_loop_point();
					int phase_diff = ((ope0->get_pcm_end_point() - ope0->get_pcm_loop_point()) << ope0->get_wave_fixed_bits());
					ope0->adjust_phase(-phase_diff);
				}
			}

			int log_idx = ope0->get_wave_value_fast(t);
			log_idx += ope0->get_eg_output() + (_amplitude_modulation_output_level>>ope0->get_amplitude_modulation_shift());
			output = _safe_log_lookup(_table, log_idx);

			ope0->get_feed_pipe()->get()->value = output;
		}

		// Output and increment pointers.
		{
			out_pipe->value = output + base_pipe->value;
			in_pipe = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe = out_pipe->next();
		}
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

void SiOPMChannelFM::_process_analog_like(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	SiOPMOperator *ope0 = _operators[0];
	SiOPMOperator *ope1 = _operators[1];

	for (int i = 0; i < p_length; i++) {
		int output0 = 0;
		int output1 = 0;

		// Update LFO.
		_update_lfo(2);

		// Update EG.
		ope0->tick_eg(_eg_timer_initial);
		ope1->update_eg_output_from(ope0);

		// Update PG.
		{
			// Operator 0.
			{
				ope0->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope0->get_amplitude_modulation_shift();
				output0 = ope0->get_super_output(in_pipe->value, _input_level, am_level);
			}

			// Operator 1 (w/ operator0's envelope and AMS).
			{
				ope1->tick_pulse_generator();
				int am_level = _amplitude_modulation_output_level >> ope0->get_amplitude_modulation_shift();
				output1 = ope1->get_super_output(0, 0, am_level);
			}

			ope0->get_feed_pipe()->get()->value = output0;
		}

		// Output and increment pointers.
		{
			out_pipe->value = output0 + output1 + base_pipe->value;
			in_pipe = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe = out_pipe->next();
		}
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

void SiOPMChannelFM::_process_ring(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	SiOPMOperator *ope0 = _operators[0];
	SiOPMOperator *ope1 = _operators[1];

	for (int i = 0; i < p_length; i++) {
		int output = 0;

		// Update LFO.
		_update_lfo(2);

		// Update EG.
		ope0->tick_eg(_eg_timer_initial);
		ope1->update_eg_output_from(ope0);

		// Update PG.
		{
			int log_idx = 0;

			// Operator 0.
			{
				ope0->tick_pulse_generator();
				int t = ((ope0->get_phase() + (in_pipe->value << _input_level)) & SiOPMRefTable::PHASE_FILTER) >> ope0->get_wave_fixed_bits();
				log_idx = ope0->get_wave_value_fast(t);
			}

			// Operator 1 (w/ operator0's envelope and AMS).
			{
				ope1->tick_pulse_generator();
				int t = (ope1->get_phase() & SiOPMRefTable::PHASE_FILTER) >> ope1->get_wave_fixed_bits();

				log_idx += ope1->get_wave_value_fast(t);
				log_idx += ope1->get_eg_output() + (_amplitude_modulation_output_level >> ope0->get_amplitude_modulation_shift());
				output = _safe_log_lookup(_table, log_idx);
			}

			ope0->get_feed_pipe()->get()->value = output;
		}

		// Output and increment pointers.
		{
			out_pipe->value = output + base_pipe->value;
			in_pipe = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe = out_pipe->next();
		}
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

void SiOPMChannelFM::_process_sync(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	SiOPMOperator *ope0 = _operators[0];
	SiOPMOperator *ope1 = _operators[1];

	for (int i = 0; i < p_length; i++) {
		int output = 0;

		// Update LFO.
		_update_lfo(2);

		// Update EG.
		ope0->tick_eg(_eg_timer_initial);
		ope1->update_eg_output_from(ope0);

		// Update PG.
		{
			// Operator 0.
			{
				ope0->tick_pulse_generator(in_pipe->value << _input_level);
				if (ope0->get_phase() & SiOPMRefTable::PHASE_MAX) {
					ope1->set_phase(ope1->get_key_on_phase_raw());
				}

				ope0->set_phase(ope0->get_phase() & SiOPMRefTable::PHASE_FILTER);
			}

			// Operator 1 (w/ operator0's envelope and AMS).
			{
				ope1->tick_pulse_generator();
				int t = (ope1->get_phase() & SiOPMRefTable::PHASE_FILTER) >> ope1->get_wave_fixed_bits();

				int log_idx = ope1->get_wave_value_fast(t);
				log_idx += ope1->get_eg_output() + (_amplitude_modulation_output_level >> ope0->get_amplitude_modulation_shift());
				output = _safe_log_lookup(_table, log_idx);
			}

			ope0->get_feed_pipe()->get()->value = output;
		}

		// Output and increment pointers.
		{
			out_pipe->value = output + base_pipe->value;
			in_pipe = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe = out_pipe->next();
		}
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

void SiOPMChannelFM::note_on() {
	// If this channel is already active, this note_on represents a voice-steal
	// event from the channel's point of view. Let the operators know so they
	// can defer envelope ATTACK and phase reset until their envelopes reach
	// (practically) zero instead of restarting abruptly mid-release.
	bool is_voice_steal = _is_note_on && !_is_idling;

	for (int i = 0; i < _operator_count; i++) {
		if (_operators[i]) {
			_operators[i]->set_voice_steal_hint(is_voice_steal);
			_operators[i]->note_on();
		}
	}

	_is_note_on = true;
	_is_idling = false;
	SiOPMChannelBase::note_on();
}

void SiOPMChannelFM::note_off() {
	for (int i = 0; i < _operator_count; i++) {
		_operators[i]->note_off();
	}

	_is_note_on = false;
	SiOPMChannelBase::note_off();
}

void SiOPMChannelFM::reset_channel_buffer_status() {
	_buffer_index = 0;
	_is_idling = true;

	for (int i = 0; i < _operator_count; i++) {
		SiOPMOperator *op = _operators[i];

		if (op->is_final() && (op->get_eg_output() < IDLING_THRESHOLD || op->get_eg_state() == SiOPMOperator::EG_ATTACK)) {
			_is_idling = false;
			break;
		}
	}

	// Reset stereo pipe cursors.
	if (_stereo_left_pipe) {
		_stereo_left_pipe->front();
	}
	if (_stereo_right_pipe) {
		_stereo_right_pipe->front();
	}
}

void SiOPMChannelFM::buffer(int p_length) {
	if (_is_idling) {
		buffer_no_process(p_length);
		return;
	}

	bool stereo_mode = _is_stereo_super_mode();

	// Preserve the start of the output pipes.
	SinglyLinkedList<int>::Element *mono_out = _out_pipe->get();
	SinglyLinkedList<int>::Element *left_start = stereo_mode ? _stereo_left_pipe->get() : nullptr;
	SinglyLinkedList<int>::Element *right_start = stereo_mode ? _stereo_right_pipe->get() : nullptr;

	// Update the output pipe for the provided length.
	// Note: _process_function is always initialized in constructor and updated
	// atomically via _update_process_function(), so no validity check needed.
	// The validity check itself was causing crashes due to Callable internal state
	// access from the audio thread during concurrent updates.
	_process_function.call(p_length);

	if (_ring_pipe) {
		if (stereo_mode) {
			_apply_ring_modulation(left_start, p_length);
			_apply_ring_modulation(right_start, p_length);
		} else {
			_apply_ring_modulation(mono_out, p_length);
		}
	}
	if (_filter_on) {
		if (stereo_mode) {
			_apply_sv_filter(left_start, p_length, _filter_variables);
			_apply_sv_filter(right_start, p_length, _filter_variables2);
		} else {
			_apply_sv_filter(mono_out, p_length, _filter_variables);
		}
	}
	if (_kill_fade_remaining_samples > 0) {
		if (stereo_mode) {
			_apply_kill_fade_stereo(left_start, right_start, p_length);
		} else {
			_apply_kill_fade(mono_out, p_length);
		}
	}

	if (_output_mode == OutputMode::OUTPUT_STANDARD && !_mute) {
		if (stereo_mode) {
			// Stereo super mode: write left/right channels separately.
			if (_has_effect_send) {
				for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
					if (_volumes[i] > 0) {
						SiOPMStream *stream = _streams[i] ? _streams[i] : _sound_chip->get_stream_slot(i);
						if (stream) {
							stream->write_stereo(left_start, right_start, _buffer_index, p_length, _volumes[i] * _instrument_gain, _pan);
						}
					}
				}
			} else {
				SiOPMStream *stream = _streams[0] ? _streams[0] : _sound_chip->get_output_stream();
				stream->write_stereo(left_start, right_start, _buffer_index, p_length, _volumes[0] * _instrument_gain, _pan);
			}
		} else {
			// Standard mono mode.
			if (_has_effect_send) {
				for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
					if (_volumes[i] > 0) {
						SiOPMStream *stream = _streams[i] ? _streams[i] : _sound_chip->get_stream_slot(i);
						if (stream) {
							stream->write(mono_out, _buffer_index, p_length, _volumes[i] * _instrument_gain, _pan);
						}
					}
				}
			} else {
				SiOPMStream *stream = _streams[0] ? _streams[0] : _sound_chip->get_output_stream();
				stream->write(mono_out, _buffer_index, p_length, _volumes[0] * _instrument_gain, _pan);
			}
		}
	}

	// Copy to meter ring (use mono mix for metering in stereo mode).
	if (stereo_mode) {
		// For stereo mode, mix down to mono for metering.
		SinglyLinkedList<int>::Element *left_elem = left_start;
		SinglyLinkedList<int>::Element *right_elem = right_start;
		SinglyLinkedList<int>::Element *mono_elem = mono_out;
		for (int i = 0; i < p_length && left_elem && right_elem && mono_elem; i++) {
			mono_elem->value = (left_elem->value + right_elem->value) >> 1;
			left_elem = left_elem->next();
			right_elem = right_elem->next();
			mono_elem = mono_elem->next();
		}
	}
	// // Debug: inspect edge discontinuities at buffer boundaries to diagnose
	// // clicky artefacts when new voices start (e.g. during voice stealing).
	// if (mono_out) {
	// 	// Read first and last sample in this block.
	// 	int first_sample = mono_out->value;
	// 	int last_sample = first_sample;
	// 	SinglyLinkedList<int>::Element *cursor = mono_out;
	// 	for (int i = 1; i < p_length && cursor; i++) {
	// 		cursor = cursor->next();
	// 		if (!cursor) {
	// 			break;
	// 		}
	// 		if (i == p_length - 1) {
	// 			last_sample = cursor->value;
	// 		}
	// 	}
	
	// 	if (_debug_have_last_sample) {
	// 		int prev = (int)_debug_last_mono_sample;
	// 		int delta = first_sample - prev;
	// 		// Only log suspiciously large steps to avoid flooding the console.
	// 		if (delta > 1024 || delta < -1024) {
	// 			UtilityFunctions::print(
	// 				"FM_EDGE_JUMP",
	// 				" chan_ptr=", (int64_t)this,
	// 				" first=", first_sample,
	// 				" prev=", prev,
	// 				" delta=", delta,
	// 				" len=", p_length
	// 			);
	// 		}
	// 	}
	
	// 	// Scan for intra-buffer discontinuities
	// 	cursor = mono_out;
	// 	int prev_sample = cursor->value;
		
	// 	for (int i = 1; i < p_length && cursor; i++) {
	// 		cursor = cursor->next();
	// 		if (!cursor) break;
			
	// 		int curr_sample = cursor->value;
	// 		int delta = curr_sample - prev_sample;
			
	// 		// Detect large intra-buffer jumps
	// 		if (delta > 256 || delta < -256) {
	// 			UtilityFunctions::print(
	// 				"FM_INTRA_BUFFER_JUMP",
	// 				" chan_ptr=", (int64_t)this,
	// 				" sample_idx=", i,
	// 				" curr=", curr_sample,
	// 				" prev=", prev_sample,
	// 				" delta=", delta
	// 			);
	// 		}
	// 		prev_sample = curr_sample;
	// 	}

	// 	int prev = mono_out->value;
	// 	int prev_delta = 0;
	// 	cursor = mono_out->next();

	// 	for (int i = 1; i < p_length && cursor; i++) {
	// 		int curr = cursor->value;
	// 		int delta = curr - prev;
	// 		int accel = delta - prev_delta;

	// 		if (abs(accel) > 128) {
	// 			// Diagnostic only: do NOT modify audio here, just log enough
	// 			// context to correlate audible clicks with operator EG state.
	// 			if (_active_operator) {
	// 				UtilityFunctions::print(
	// 					"FM_CURVATURE_SPIKE",
	// 					" idx=", i,
	// 					" accel=", accel,
	// 					" delta=", delta,
	// 					" eg_state=", (int)_active_operator->get_eg_state(),
	// 					" eg_out=", _active_operator->get_eg_output()
	// 				);
	// 			} else {
	// 				UtilityFunctions::print(
	// 					"FM_CURVATURE_SPIKE",
	// 					" idx=", i,
	// 					" accel=", accel,
	// 					" delta=", delta
	// 				);
	// 			}
	// 		}

	// 		prev_delta = delta;
	// 		prev = curr;
	// 		cursor = cursor->next();
	// 	}

		
	// 	_debug_last_mono_sample = (double)last_sample;
	// 	_debug_have_last_sample = true;
	// }

	_buffer_index += p_length;
}

//

void SiOPMChannelFM::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	_update_operator_count(1);
	_operators[0]->initialize();

	_is_note_on = false;
	SiOPMChannelBase::initialize(p_prev, p_buffer_index);

	// Reset stereo filter state.
	_filter_variables2[0] = 0;
	_filter_variables2[1] = 0;
	_filter_variables2[2] = 0;
}

void SiOPMChannelFM::reset() {
	// Reset all 4 operators, not just the active ones
	// This ensures operators beyond _operator_count are also in a clean state
	for (int i = 0; i < 4; i++) {
		if (_operators[i]) {
			_operators[i]->reset();
		}
	}

	_is_note_on = false;
	_is_idling = true;
}

String SiOPMChannelFM::_to_string() const {
	String params = "";

	params += "ops=" + itos(_operator_count) + ", ";

	params += "feedback=" + itos(_input_level - 6) + ", ";
	params += "vol=" + rtos(_volumes[0]) + ", ";
	params += "pan=" + itos(_pan - 64) + "";

	return "SiOPMChannelFM: " + params;
}

void SiOPMChannelFM::_bind_methods() {
	// To be used as callables.
	ClassDB::bind_method(D_METHOD("_process_operator1_lfo_off", "length"), &SiOPMChannelFM::_process_operator1_lfo_off);
	ClassDB::bind_method(D_METHOD("_process_operator1_lfo_on", "length"),  &SiOPMChannelFM::_process_operator1_lfo_on);
	ClassDB::bind_method(D_METHOD("_process_operator2", "length"),         &SiOPMChannelFM::_process_operator2);
	ClassDB::bind_method(D_METHOD("_process_operator3", "length"),         &SiOPMChannelFM::_process_operator3);
	ClassDB::bind_method(D_METHOD("_process_operator4", "length"),         &SiOPMChannelFM::_process_operator4);
	ClassDB::bind_method(D_METHOD("_process_pcm_lfo_off", "length"),       &SiOPMChannelFM::_process_pcm_lfo_off);
	ClassDB::bind_method(D_METHOD("_process_pcm_lfo_on", "length"),        &SiOPMChannelFM::_process_pcm_lfo_on);
	ClassDB::bind_method(D_METHOD("_process_analog_like", "length"),       &SiOPMChannelFM::_process_analog_like);
	ClassDB::bind_method(D_METHOD("_process_ring", "length"),              &SiOPMChannelFM::_process_ring);
	ClassDB::bind_method(D_METHOD("_process_sync", "length"),              &SiOPMChannelFM::_process_sync);
}

SiOPMChannelFM::SiOPMChannelFM(SiOPMSoundChip *p_chip) : SiOPMChannelBase(p_chip) {
	_process_function_list = {
		{
			Callable(this, "_process_operator1_lfo_off"),
			Callable(this, "_process_operator2"),
			Callable(this, "_process_operator3"),
			Callable(this, "_process_operator4"),
			Callable(this, "_process_analog_like"),
			Callable(this, "_process_ring"),
			Callable(this, "_process_sync"),
			Callable(this, "_process_operator2"),
			Callable(this, "_process_pcm_lfo_off")
		},
		{
			Callable(this, "_process_operator1_lfo_on"),
			Callable(this, "_process_operator2"),
			Callable(this, "_process_operator3"),
			Callable(this, "_process_operator4"),
			Callable(this, "_process_analog_like"),
			Callable(this, "_process_ring"),
			Callable(this, "_process_sync"),
			Callable(this, "_process_operator2"),
			Callable(this, "_process_pcm_lfo_on")
		}
	};

	// CRITICAL: Allocate all 4 operators at construction time.
	// Never allocate/deallocate during playback to avoid races with audio thread.
	_operators.resize_zeroed(4);
	for (int i = 0; i < 4; i++) {
		_operators.write[i] = _alloc_operator();
		_operators[i]->initialize();
	}
	
	_operator_count = 1;
	_active_operator = _operators[0];

	_update_process_function();

	_pipe0 = memnew(SinglyLinkedList<int>(1, 0, true));
	_pipe1 = memnew(SinglyLinkedList<int>(1, 0, true));

	// Allocate stereo super wave pipes with the same size as the sound chip's buffer.
	int buffer_size = p_chip ? p_chip->get_buffer_length() : 2048;
	_stereo_left_pipe = memnew(SinglyLinkedList<int>(buffer_size, 0, true));
	_stereo_right_pipe = memnew(SinglyLinkedList<int>(buffer_size, 0, true));

	initialize(nullptr, 0);
}

SiOPMChannelFM::~SiOPMChannelFM() {
	_active_operator = nullptr;

	for (SiOPMOperator *op : _operators) {
		if (op) {
			_release_operator(op);
		}
	}
	_operators.clear();

	memdelete(_pipe0);
	memdelete(_pipe1);
	memdelete(_stereo_left_pipe);
	memdelete(_stereo_right_pipe);
}

// --- Internal helper ---------------------------------------------------------
// Safely fetch a value from the global logarithmic volume table. The raw
// synthesis code can produce negative or over-sized indices when the engine is
// heavily modulated which would otherwise index outside the array and crash.
static _FORCE_INLINE_ int _safe_log_lookup(class SiOPMRefTable *p_table, int p_index) {
	// Clamp instead of wrapping – matches behaviour of original SiON driver
	// which relied on ActionScript's automatic array bounds saturation.
	if (unlikely(p_index < 0)) {
		p_index = 0;
	} else {
		const int max_idx = SiOPMRefTable::LOG_TABLE_SIZE * 3 - 1;
		if (unlikely(p_index > max_idx)) {
			p_index = max_idx;
		}
	}
	return p_table->log_table[p_index];
}
