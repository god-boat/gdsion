/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_operator.h"

#include <cmath>
#include <godot_cpp/classes/random_number_generator.hpp>
#include "chip/siopm_operator_params.h"
#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "chip/wave/siopm_wave_pcm_data.h"
#include "chip/wave/siopm_wave_table.h"
#include "utils/godot_util.h"

const int SiOPMOperator::_eg_next_state_table[2][EG_MAX] = {
	// EG_ATTACK,  EG_DECAY,   EG_SUSTAIN, EG_RELEASE, EG_OFF
	{  EG_DECAY,   EG_SUSTAIN, EG_OFF,     EG_OFF,     EG_OFF }, // normal
	{  EG_DECAY,   EG_SUSTAIN, EG_ATTACK,  EG_OFF,     EG_OFF }  // ssgev
};

// FM module parameters.

void SiOPMOperator::set_attack_rate(int p_value) {
	_attack_rate = p_value & 63;

	if (_ssg_type == SiOPMOperatorParams::SSG_REPEAT_TO_ZERO || _ssg_type == SiOPMOperatorParams::SSG_REPEAT_TO_MAX) {
		_eg_ssgec_attack_rate = (_attack_rate >= 56) ? 1 : 0;
	} else {
		_eg_ssgec_attack_rate = (_attack_rate >= 60) ? 1 : 0;
	}
	
	// Update active envelope if currently in ATTACK state
	if (_eg_state == EG_ATTACK) {
		update_active_eg_timer();
	}
}

void SiOPMOperator::set_decay_rate(int p_value) {
	_decay_rate = p_value & 63;
	
	// Update active envelope if currently in DECAY state
	if (_eg_state == EG_DECAY) {
		update_active_eg_timer();
	}
}

void SiOPMOperator::set_sustain_rate(int p_value) {
	_sustain_rate = p_value & 63;
	
	// Update active envelope if currently in SUSTAIN state
	if (_eg_state == EG_SUSTAIN) {
		update_active_eg_timer();
	}
}

void SiOPMOperator::set_release_rate(int p_value) {
	_release_rate = p_value & 63;
	
	// Update active envelope if currently in RELEASE state
	if (_eg_state == EG_RELEASE) {
		update_active_eg_timer();
	}
}

void SiOPMOperator::set_sustain_level(int p_value) {
	_sustain_level = p_value & 15;
	_eg_sustain_level = _table->eg_sustain_level_table[p_value];
}

void SiOPMOperator::_update_total_level() {
	_eg_total_level = ((_total_level + (_key_code >> _eg_key_scale_level_rshift)) << SiOPMRefTable::ENV_LSHIFT) + _eg_tl_offset + _mute;

	if (_eg_total_level > SiOPMRefTable::ENV_BOTTOM) {
		_eg_total_level = SiOPMRefTable::ENV_BOTTOM;
	}
	_eg_total_level -= SiOPMRefTable::ENV_TOP; // Table index + 192.

	update_eg_output();
}

void SiOPMOperator::set_total_level(int p_value) {
	_total_level = CLAMP(p_value, 0, 127);
	_update_total_level();
}

void SiOPMOperator::offset_total_level(int p_offset) {
	_eg_tl_offset = p_offset;
	_update_total_level();
}

int SiOPMOperator::get_key_scaling_rate() const {
	return 5 - _key_scaling_rate;
}

void SiOPMOperator::set_key_scaling_rate(int p_value) {
	_key_scaling_rate = 5 - (p_value & 3);
	_eg_key_scale_rate = _key_code >> _key_scaling_rate;
}

void SiOPMOperator::set_key_scaling_level(int p_value, bool p_silent) {
	_key_scaling_level = p_value & 3;
	// [0,1,2,3]->[8,4,3,2]
	_eg_key_scale_level_rshift = (_key_scaling_level == 0) ? 8 : (5 - _key_scaling_level);

	if (!p_silent) {
		_update_total_level();
	}
}

int SiOPMOperator::get_multiple() const {
	return (_fine_multiple >> 7);
}

void SiOPMOperator::set_multiple(int p_value) {
	int multiple = p_value & 15;
	_fine_multiple = (multiple != 0) ? (multiple << 7) : 64;
	_update_pitch();
}

void SiOPMOperator::set_detune1(int p_value) {
	_detune1 = p_value & 7;
	_update_pitch();
}

void SiOPMOperator::set_detune2(int p_value) {
	_detune2 = p_value & 3;
	_pitch_index_shift = _table->dt2_table[_detune2];
	_update_pitch();
}

bool SiOPMOperator::is_amplitude_modulation_enabled() const {
	return _amplitude_modulation_shift != 16;
}

void SiOPMOperator::set_amplitude_modulation_enabled(bool p_enabled) {
	_amplitude_modulation_shift = p_enabled ? 2 : 16;
}

int SiOPMOperator::get_amplitude_modulation_shift() const {
	return (_amplitude_modulation_shift == 16) ? 0 : (3 - _amplitude_modulation_shift);
}

void SiOPMOperator::set_amplitude_modulation_shift(int p_value) {
	_amplitude_modulation_shift = (p_value != 0) ? (3 - p_value) : 16;
}

void SiOPMOperator::_update_key_code(int p_value) {
	_key_code = p_value;
	_eg_key_scale_rate = _key_code >> _key_scaling_rate;
	_update_total_level();
}

void SiOPMOperator::set_key_code(int p_value) {
	if (_pitch_fixed) {
		return;
	}

	_update_key_code(p_value & 127);
	_pitch_index = ((_key_code - (_key_code >> 2)) << 6) | (_pitch_index & 63);
	_update_pitch();
}

bool SiOPMOperator::is_mute() const {
	return _mute != 0;
}

void SiOPMOperator::set_mute(bool p_mute) {
	_mute = p_mute ? SiOPMRefTable::ENV_BOTTOM : 0;
	_update_total_level();
}

void SiOPMOperator::set_ssg_type(int p_value) {
	if (p_value >= SiOPMOperatorParams::SSG_REPEAT_TO_ZERO) {
		_eg_state_table_index = 1;
		_ssg_type = p_value;
		if (_ssg_type >= SiOPMOperatorParams::SSG_MAX) {
			_ssg_type = SiOPMOperatorParams::SSG_IGNORE;
		}
	} else {
		_eg_state_table_index = 0;
		_ssg_type = SiOPMOperatorParams::SSG_DISABLED;
	}
}

// Pulse generator.

void SiOPMOperator::_update_pitch() {
	int index = (_pitch_index + _pitch_index_shift + _pitch_index_shift2) & _pitch_table_filter;
	_update_phase_step(_pitch_table[index] >> _wave_phase_step_shift);
	_update_super_phase_steps();
}

void SiOPMOperator::_update_phase_step(int p_step) {
	_phase_step = p_step;
	_phase_step += _table->dt1_table[_detune1][_key_code];
	_phase_step *= _fine_multiple;
	_phase_step >>= (7 - _table->sample_rate_pitch_shift);  // 44kHz:1/128, 22kHz:1/256
}

void SiOPMOperator::_update_wave_table_cache() {
	const int size = _wave_table.size();
	if (size > 0) {
		_wave_table_ptr = _wave_table.ptr();
		_wave_table_size = size;
		_wave_table_is_pow2 = (size & (size - 1)) == 0;
		_wave_table_mask = size - 1;
	} else {
		_wave_table_ptr = nullptr;
		_wave_table_size = 0;
		_wave_table_is_pow2 = false;
		_wave_table_mask = 0;
	}
}

void SiOPMOperator::set_pulse_generator_type(int p_type) {
	_pg_type = p_type & SiOPMRefTable::PG_FILTER;

	Ref<SiOPMWaveTable> wave_table = _table->get_wave_table(_pg_type);
	_wave_table = wave_table->get_wavelet();
	_wave_fixed_bits = wave_table->get_fixed_bits();
	_update_wave_table_cache();
}

void SiOPMOperator::set_pitch_table_type(SiONPitchTableType p_type) {
	_pt_type = p_type;

	_wave_phase_step_shift = (SiOPMRefTable::PHASE_BITS - _wave_fixed_bits) & _table->phase_step_shift_filter[p_type];
	_pitch_table = _table->pitch_table[p_type];
	_pitch_table_filter = _pitch_table.size() - 1;
}

int SiOPMOperator::get_wave_value(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, _wave_table.size(), -1);
	return _wave_table[p_index];
}

void SiOPMOperator::set_fixed_pitch_index(int p_value) {
	if (p_value > 0) {
		_pitch_index = p_value;

		_update_key_code(_table->note_number_to_key_code[(_pitch_index >> 6) & 127]);
		_update_pitch();
		_pitch_fixed = true;
	} else {
		_pitch_fixed = false;
	}
}

void SiOPMOperator::set_pitch_index(int p_value)
{
	if (_pitch_fixed) {
		return;
	}

	_pitch_index = p_value;
	_update_key_code(_table->note_number_to_key_code[(p_value >> 6) & 127]);
	_update_pitch();
}

void SiOPMOperator::set_ptss_detune(int p_value) {
	_detune2 = 0;
	_pitch_index_shift = p_value;
	_update_pitch();
}

void SiOPMOperator::set_pm_detune(int p_value) {
	_pitch_index_shift2 = p_value;
	_update_pitch();
}

void SiOPMOperator::set_fine_multiple(int p_value) {
	_fine_multiple = p_value;
	_update_pitch();
}

int SiOPMOperator::get_key_on_phase() const {
	if (_key_on_phase >= 0) {
		return _key_on_phase >> (SiOPMRefTable::PHASE_BITS - 8);
	} else {
		return (_key_on_phase == -1) ? -1 : 255;
	}
}

void SiOPMOperator::set_key_on_phase(int p_phase) {
	if (p_phase == 255) {
		_key_on_phase = -2;
	} else if (p_phase == -1) {
		_key_on_phase = -1;
	} else {
		_key_on_phase = (p_phase & 255) << (SiOPMRefTable::PHASE_BITS - 8);
	}
}

int SiOPMOperator::get_fm_level() const {
	return (_fm_shift > 10) ? (_fm_shift - 10) : 0;
}

void SiOPMOperator::set_fm_level(int p_level) {
	_fm_shift = (p_level != 0) ? (p_level + 10) : 0;
}

int SiOPMOperator::get_key_fraction() const {
	return (_pitch_index & 63);
}

void SiOPMOperator::set_key_fraction(int p_value) {
	_pitch_index = (_pitch_index & 0xffc0) | (p_value & 63);
	_update_pitch();
}

void SiOPMOperator::set_fnumber(int p_value) {
	// Naive implementation.
	_update_key_code((p_value >> 7) & 127);
	_detune2 = 0;
	_pitch_index = 0;
	_pitch_index_shift = 0;
	_update_phase_step((p_value & 2047) << ((p_value >> 11) & 7));
}

// Super wave methods.

void SiOPMOperator::set_super_wave(int p_count, int p_spread) {
	_super_count = (p_count < 1) ? 1 : ((p_count > MAX_SUPER_VOICES) ? MAX_SUPER_VOICES : p_count);
	_super_spread = (p_spread < 0) ? 0 : ((p_spread > 1000) ? 1000 : p_spread);
	_update_super_phase_steps();
	_update_super_pan_values();
}

void SiOPMOperator::set_super_stereo_spread(int p_value) {
	_super_stereo_spread = (p_value < 0) ? 0 : ((p_value > 100) ? 100 : p_value);
	_update_super_pan_values();
}

void SiOPMOperator::_update_super_pan_values() {
	// Pan values range from 0 (full left) to 128 (full right), 64 = center.
	// Spread voices across the stereo field based on _super_stereo_spread.
	// At spread=0, all voices are centered. At spread=100, voices span full L-R.
	if (_super_count <= 1 || _super_stereo_spread == 0) {
		for (int i = 0; i < MAX_SUPER_VOICES; i++) {
			_super_pan_values[i] = 64; // Center
		}
		return;
	}

	// Calculate pan positions: distribute voices symmetrically around center.
	// Voice 0 is leftmost, voice (count-1) is rightmost when spread is 100.
	float half_spread = (_super_stereo_spread / 100.0f) * 64.0f; // Max offset from center
	for (int i = 0; i < _super_count; i++) {
		// Normalized position: -1 (leftmost) to +1 (rightmost)
		float pos = (float)(i - (_super_count - 1) * 0.5f) / ((_super_count - 1) * 0.5f);
		// Convert to pan range [0-128] with center at 64
		int pan = 64 + (int)(pos * half_spread);
		_super_pan_values[i] = CLAMP(pan, 0, 128);
	}
}

void SiOPMOperator::_update_super_phase_steps() {
	if (_super_count <= 1) {
		return;
	}

	for (int i = 0; i < _super_count; i++) {
		if (_super_count == 1) {
			_super_phase_steps[i] = _phase_step;
		} else {
			float spread_factor = (float)(i - (_super_count - 1) * 0.5f) / ((_super_count - 1) * 0.5f);
			int detune = (int)(_phase_step * spread_factor * _super_spread / 1000.0f);
			_super_phase_steps[i] = _phase_step + detune;
		}
	}
}

int SiOPMOperator::get_super_output(int p_fm_input, int p_input_level, int p_am_level) {
	if (_super_count <= 1) {
		int t = ((_phase + (p_fm_input << p_input_level)) & SiOPMRefTable::PHASE_FILTER) >> _wave_fixed_bits;
		int log_idx = _get_wave_value_fast(t) + _eg_output + p_am_level;
		if (log_idx < 0) {
			log_idx = 0;
		} else if (log_idx > SiOPMRefTable::LOG_TABLE_SIZE * 3 - 1) {
			log_idx = SiOPMRefTable::LOG_TABLE_SIZE * 3 - 1;
		}
		return _table->log_table[log_idx];
	}

	int sum = 0;
	for (int i = 0; i < _super_count; i++) {
		int t = ((_super_phases[i] + (p_fm_input << p_input_level)) & SiOPMRefTable::PHASE_FILTER) >> _wave_fixed_bits;
		int log_idx = _get_wave_value_fast(t) + _eg_output + p_am_level;
		if (log_idx < 0) {
			log_idx = 0;
		} else if (log_idx > SiOPMRefTable::LOG_TABLE_SIZE * 3 - 1) {
			log_idx = SiOPMRefTable::LOG_TABLE_SIZE * 3 - 1;
		}
		sum += _table->log_table[log_idx];
	}
	// Normalize by sqrt(n) to maintain roughly consistent perceived loudness.
	// With random phases, RMS power grows as sqrt(n), so this keeps the level
	// stable while preserving the thickness from additional voices.
	return (int)(sum * _super_norm_inv);
}

bool SiOPMOperator::get_super_output_stereo(int p_fm_input, int p_input_level, int p_am_level, int &r_left, int &r_right) {
	// If stereo spread is disabled or single voice, fall back to mono.
	if (_super_stereo_spread == 0 || _super_count <= 1) {
		r_left = r_right = get_super_output(p_fm_input, p_input_level, p_am_level);
		return false; // Not stereo
	}

	// Pan table reference for converting pan position to L/R gains.
	double (&pan_table)[129] = _table->pan_table;

	double sum_left = 0.0;
	double sum_right = 0.0;

	for (int i = 0; i < _super_count; i++) {
		int t = ((_super_phases[i] + (p_fm_input << p_input_level)) & SiOPMRefTable::PHASE_FILTER) >> _wave_fixed_bits;
		int log_idx = _get_wave_value_fast(t) + _eg_output + p_am_level;
		if (log_idx < 0) {
			log_idx = 0;
		} else if (log_idx > SiOPMRefTable::LOG_TABLE_SIZE * 3 - 1) {
			log_idx = SiOPMRefTable::LOG_TABLE_SIZE * 3 - 1;
		}
		int sample = _table->log_table[log_idx];

		// Apply per-voice panning.
		int pan = _super_pan_values[i];
		sum_left += sample * pan_table[128 - pan];
		sum_right += sample * pan_table[pan];
	}

	// Normalize by sqrt(n) to maintain consistent perceived loudness.
	double norm = sqrt((double)_super_count);
	r_left = (int)(sum_left / norm);
	r_right = (int)(sum_right / norm);

	return true; // Stereo output
}

void SiOPMOperator::tick_pulse_generator(int p_extra) {
	_phase += _phase_step + p_extra;

	if (_super_count > 1) {
		for (int i = 0; i < _super_count; i++) {
			_super_phases[i] += _super_phase_steps[i] + p_extra;
		}
	}
}

// Envelope generator.

void SiOPMOperator::_shift_eg_state(EGState p_state) {
	switch (p_state) {
		case EG_ATTACK: {
			_eg_ssgec_state++;
			if (_eg_ssgec_state == 3) {
				_eg_ssgec_state = 1;
			}

			if (_attack_rate + _eg_key_scale_rate < 62) {
				if (_envelope_reset_on_attack) {
					_eg_level = SiOPMRefTable::ENV_BOTTOM;
				}
				_eg_state = EG_ATTACK;
			_eg_level_table = make_vector<int>(_table->eg_level_tables[0]);

			const int index = _eg_rate_to_index(_attack_rate);
			_eg_increment_table = make_vector<int>(_table->eg_increment_tables_attack[_table->eg_table_selector[index]]);
			_eg_timer_step = _table->eg_timer_steps[index];
				break;
			}
		}
			[[fallthrough]];

		case EG_DECAY: {
			if (_eg_sustain_level) {
				_eg_state = EG_DECAY;

				if (_ssg_type > SiOPMOperatorParams::SSG_REPEAT_TO_ZERO) {
					_eg_level = 0;

					_eg_state_shift_level = _eg_sustain_level >> 2;
					if (_eg_state_shift_level > SiOPMRefTable::ENV_BOTTOM_SSGEC) {
						_eg_state_shift_level = SiOPMRefTable::ENV_BOTTOM_SSGEC;
					}

					const int normalized_ssg_type = _ssg_type - SiOPMOperatorParams::SSG_REPEAT_TO_ZERO;
					const int level_index = _table->eg_ssg_table_index[normalized_ssg_type][_eg_ssgec_attack_rate][_eg_ssgec_state];
					_eg_level_table = make_vector<int>(_table->eg_level_tables[level_index]);
				} else {
					_eg_level = 0;
					_eg_state_shift_level = _eg_sustain_level;
			_eg_level_table = make_vector<int>(_table->eg_level_tables[0]);
		}

		int index = _eg_rate_to_index(_decay_rate);
		_eg_increment_table = make_vector<int>(_table->eg_increment_tables[_table->eg_table_selector[index]]);
		_eg_timer_step = _table->eg_timer_steps[index];
				break;
			}
		}
			[[fallthrough]];

		case EG_SUSTAIN: {
			_eg_state = EG_SUSTAIN;

			if (_ssg_type >= SiOPMOperatorParams::SSG_REPEAT_TO_ZERO) {
				_eg_level = _eg_sustain_level >> 2;
				_eg_state_shift_level = SiOPMRefTable::ENV_BOTTOM_SSGEC;

				const int normalized_ssg_type = _ssg_type - SiOPMOperatorParams::SSG_REPEAT_TO_ZERO;
				const int level_index = _table->eg_ssg_table_index[normalized_ssg_type][_eg_ssgec_attack_rate][_eg_ssgec_state];
				_eg_level_table = make_vector<int>(_table->eg_level_tables[level_index]);
			} else {
				_eg_level = _eg_sustain_level;
				_eg_state_shift_level = SiOPMRefTable::ENV_BOTTOM;
		_eg_level_table = make_vector<int>(_table->eg_level_tables[0]);
	}

	const int index = _eg_rate_to_index(_sustain_rate);
	_eg_increment_table = make_vector<int>(_table->eg_increment_tables[_table->eg_table_selector[index]]);
	_eg_timer_step = _table->eg_timer_steps[index];
		} break;

		case EG_RELEASE: {
			if (_eg_level < SiOPMRefTable::ENV_BOTTOM) {
				_eg_state = EG_RELEASE;
				_eg_state_shift_level = SiOPMRefTable::ENV_BOTTOM;

				if (_ssg_type >= SiOPMOperatorParams::SSG_REPEAT_TO_ZERO) {
					_eg_level_table = make_vector<int>(_table->eg_level_tables[1]);
				} else {
					_eg_level_table = make_vector<int>(_table->eg_level_tables[0]);
				}

				// Voice stealing: use the absolute fastest release (~2-3ms) to quickly
				// silence the old voice before starting the new one. This ensures
				// smooth but near-instant transitions without waiting for release tails.
				// Fast release is implicit when _deferred_attack_target != EG_OFF.
				if (_deferred_attack_target != EG_OFF) {
					// Table 16 = fastest (increment by 8 per cycle), with fastest timer.
				_eg_increment_table = make_vector<int>(_table->eg_increment_tables[16]);
				_eg_timer_step = _table->eg_timer_steps[63]; // Maximum timer step
			} else {
				const int index = _eg_rate_to_index(_release_rate);
				_eg_increment_table = make_vector<int>(_table->eg_increment_tables[_table->eg_table_selector[index]]);
				_eg_timer_step = _table->eg_timer_steps[index];
			}
				break;
			}
		}
			[[fallthrough]];

		case EG_OFF:
		default: {
			_eg_state = EG_OFF;
			_eg_level = SiOPMRefTable::ENV_BOTTOM;
			_eg_state_shift_level = SiOPMRefTable::ENV_BOTTOM + 1;
			_eg_level_table = make_vector<int>(_table->eg_level_tables[0]);

			_eg_increment_table = make_vector<int>(_table->eg_increment_tables[17]); // 17 = all zero
			_eg_timer_step = _table->eg_timer_steps[96]; // 96 = all zero
		} break;
	}
}

void SiOPMOperator::_reset_note_phases() {
	if (_key_on_phase >= 0) {
		_phase = _key_on_phase;
	} else if (_key_on_phase == -1) {
		Ref<RandomNumberGenerator> rng;
		rng.instantiate();
		_phase = int(rng->randi_range(0, SiOPMRefTable::PHASE_MAX));
	}

	if (_super_count > 1) {
		// Each super voice gets a random starting phase for instant chorus/thickness.
		// This is essential for the characteristic supersaw sound - without it,
		// all voices start in phase and only gradually drift apart.
		Ref<RandomNumberGenerator> rng;
		rng.instantiate();

		for (int i = 0; i < _super_count; i++) {
			_super_phases[i] = int(rng->randi_range(0, SiOPMRefTable::PHASE_MAX));
		}
	}
}

// Original implementation inlines all this code (with private member access)
// for performance reasons, as stated in the comments. This is probably irrelevant for us.
// But if it's not, there's got to be a better way to realize this optimization.
void SiOPMOperator::tick_eg(int p_timer_initial) {
	_eg_timer -= _eg_timer_step;
	if (_eg_timer >= 0) {
		return;
	}

	// Snapshot increment table to avoid torn reads and guard indexing.
	Vector<int> inc = _eg_increment_table;
	int inc_size = inc.size();
	int step = 0;
	if (likely(inc_size > 0)) {
		int inc_idx = (_eg_counter & 7);
		if (unlikely(inc_idx >= inc_size)) {
			// Wrap just in case a non-standard table size slips through.
			if ((inc_size & (inc_size - 1)) == 0) {
				inc_idx &= (inc_size - 1);
			} else {
				inc_idx %= inc_size;
			}
		}
		step = inc[inc_idx];
	}

	if (_eg_state == SiOPMOperator::EG_ATTACK) {
		int offset = step;
		if (offset > 0) {
			_eg_level -= 1 + (_eg_level >> offset);
			if (_eg_level <= 0) {
				_shift_eg_state((EGState)_eg_next_state_table[_eg_state_table_index][_eg_state]);
			}
		}
	} else {
		_eg_level += step;
		// When a new note is triggered while the operator is still audible, we
		// defer the ATTACK (and any phase reset) until the envelope has fully
		// decayed to silence. This avoids instantaneous slope changes and phase
		// jumps that manifest as clicks during voice stealing.
		if (_deferred_attack_target != EG_OFF && _eg_state == SiOPMOperator::EG_RELEASE) {
			// During voice stealing, transition to ATTACK when envelope is "quiet enough"
			// rather than waiting for full silence. This reduces perceived latency while
			// still avoiding the loudest clicks. Use 90% of the way to silence as threshold.
			// ENV_BOTTOM is 832, so ~750 is quiet enough to mask any remaining discontinuity.
			constexpr int FAST_RELEASE_THRESHOLD = SiOPMRefTable::ENV_BOTTOM - 80;
			
			if (_eg_level >= FAST_RELEASE_THRESHOLD) {
				// Now quiet enough: reset phase and start deferred attack
				_reset_note_phases();
				_shift_eg_state(_deferred_attack_target);
				_deferred_attack_target = EG_OFF; // Clear deferred state
			}
		} else if (_eg_level >= _eg_state_shift_level) {
			_shift_eg_state((EGState)_eg_next_state_table[_eg_state_table_index][_eg_state]);
		}
	}

	update_eg_output();
	_eg_counter = (_eg_counter + 1) & 7;

	_eg_timer += p_timer_initial;
}

void SiOPMOperator::update_eg_output() {
	// Snapshot the level table because another thread can swap it while the audio
	// thread is in-flight, leaving the internal pointer dangling between the size
	// check and the indexed read.
	Vector<int> level_table = _eg_level_table;

	// Guard against the envelope level table being unexpectedly empty. This can
	// happen if initialization failed or a malformed SSG envelope was selected.
	int table_size = level_table.size();
	if (unlikely(table_size == 0)) {
		// A zero-length table makes no sense; output silence and keep the internal
		// level within a safe range to avoid undefined behaviour.
		_eg_level = 0;
		_eg_output = _eg_total_level << 3;
		return;
	}
	// Guard against envelope generator level going out of the table bounds which
	// can happen under extreme modulation/voice-overflow situations. Using
	// CLAMP keeps the original behaviour for valid ranges while preventing a
	// fatal CRASH_BAD_INDEX when the synth is overloaded.
	int safe_index = _eg_level;
	if (unlikely(safe_index < 0)) {
		safe_index = 0;
	} else if (unlikely(safe_index >= table_size)) {
		safe_index = table_size - 1;
	}
	_eg_level = safe_index; // keep internal state consistent
	_eg_output = (level_table[safe_index] + _eg_total_level) << 3;
}

void SiOPMOperator::update_eg_output_from(SiOPMOperator *p_other) {
	// Safely read other's EG level table to avoid out-of-bounds in debug.
	Vector<int> other_tbl = p_other->_eg_level_table;
	int size = other_tbl.size();
	int idx = p_other->_eg_level;
	if (unlikely(size == 0)) {
		_eg_output = _eg_total_level << 3;
		return;
	}
	if (unlikely(idx < 0)) idx = 0;
	else if (unlikely(idx >= size)) idx = size - 1;
	_eg_output = (other_tbl[idx] + _eg_total_level) << 3;
}

void SiOPMOperator::update_active_eg_timer() {
	// Only update if envelope is actively running (not OFF)
	if (_eg_state == EG_OFF) {
		return;
	}
	
	// Recalculate timer step and increment table based on current state
	int rate = 0;
	switch (_eg_state) {
		case EG_ATTACK:
			rate = _attack_rate;
			break;
		case EG_DECAY:
			rate = _decay_rate;
			break;
		case EG_SUSTAIN:
			rate = _sustain_rate;
			break;
		case EG_RELEASE:
			// Skip if in fast-release mode (voice stealing)
			if (_deferred_attack_target != EG_OFF) {
				return;
			}
			rate = _release_rate;
			break;
		default:
			return;
	}
	
	const int index = _eg_rate_to_index(rate);
	
	if (_eg_state == EG_ATTACK) {
		_eg_increment_table = make_vector<int>(_table->eg_increment_tables_attack[_table->eg_table_selector[index]]);
	} else {
		_eg_increment_table = make_vector<int>(_table->eg_increment_tables[_table->eg_table_selector[index]]);
	}
	
	_eg_timer_step = _table->eg_timer_steps[index];
}

// Pipes.

void SiOPMOperator::set_pipes(SinglyLinkedList<int> *p_out_pipe, SinglyLinkedList<int> *p_in_pipe, bool p_final) {
	_final = p_final;
	_fm_shift  = 15;

	_out_pipe = p_out_pipe;
	_in_pipe = p_in_pipe ? p_in_pipe : _sound_chip->get_zero_buffer();
	_base_pipe = (p_out_pipe != p_in_pipe) ? p_out_pipe : _sound_chip->get_zero_buffer();
}

//

void SiOPMOperator::set_operator_params(const Ref<SiOPMOperatorParams> &p_params) {
	// Some code here is duplicated from respective setters to avoid calling them
	// and triggering side effects. Modify with care.

	// Defensive check: ensure params reference is valid.
	if (unlikely(p_params.is_null())) {
		ERR_FAIL_MSG("SiOPMOperator::set_operator_params() called with null params. This indicates a lifecycle or initialization bug.");
	}

	set_pulse_generator_type(p_params->get_pulse_generator_type());
	set_pitch_table_type(p_params->get_pitch_table_type());

	set_key_on_phase(p_params->get_initial_phase());

	set_attack_rate(p_params->get_attack_rate());
	set_decay_rate(p_params->get_decay_rate());
	set_sustain_rate(p_params->get_sustain_rate());
	set_release_rate(p_params->get_release_rate());

	set_key_scaling_rate(p_params->get_key_scaling_rate());
	set_key_scaling_level(p_params->get_key_scaling_level(), true);

	set_amplitude_modulation_shift(p_params->get_amplitude_modulation_shift());

	_fine_multiple = p_params->get_fine_multiple();
	_fm_shift = (p_params->get_frequency_modulation_level() & 7) + 10;
	_detune1 = p_params->get_detune1() & 7;
	_pitch_index_shift = p_params->get_detune2();

	_mute = p_params->is_mute() ? SiOPMRefTable::ENV_BOTTOM : 0;
	set_ssg_type(p_params->get_ssg_envelope_control());
	_envelope_reset_on_attack = p_params->is_envelope_reset_on_attack();

	if (p_params->get_fixed_pitch() > 0) {
		_pitch_index = p_params->get_fixed_pitch();

		_update_key_code(_table->note_number_to_key_code[(_pitch_index >> 6) & 127]);
		_pitch_fixed = true;
	} else {
		_pitch_fixed = false;
	}

	set_sustain_level(p_params->get_sustain_level() & 15);
	set_total_level(p_params->get_total_level());

	set_super_wave(p_params->get_super_count(), p_params->get_super_spread());
	set_super_stereo_spread(p_params->get_super_stereo_spread());

	_update_pitch();
}

void SiOPMOperator::get_operator_params(const Ref<SiOPMOperatorParams> &r_params) {
	r_params->set_pulse_generator_type(_pg_type);
	r_params->set_pitch_table_type(_pt_type);

	r_params->set_attack_rate(_attack_rate);
	r_params->set_decay_rate(_decay_rate);
	r_params->set_sustain_rate(_sustain_rate);
	r_params->set_release_rate(_release_rate);
	r_params->set_sustain_level(_sustain_level);
	r_params->set_total_level(_total_level);

	r_params->set_key_scaling_rate(get_key_scaling_rate());
	r_params->set_key_scaling_level(_key_scaling_level);
	r_params->set_fine_multiple(get_fine_multiple());
	r_params->set_detune1(_detune1);
	r_params->set_detune2(get_ptss_detune());
	r_params->set_amplitude_modulation_shift(get_amplitude_modulation_shift());

	r_params->set_ssg_envelope_control(get_ssg_type());
	r_params->set_envelope_reset_on_attack(is_envelope_reset_on_attack());

	r_params->set_initial_phase(get_key_on_phase());
	r_params->set_frequency_modulation_level(get_fm_level());

	r_params->set_super_count(_super_count);
	r_params->set_super_spread(_super_spread);
	r_params->set_super_stereo_spread(_super_stereo_spread);
}

void SiOPMOperator::set_wave_table(const Ref<SiOPMWaveTable> &p_wave_table) {
	_pg_type = SiONPulseGeneratorType::PULSE_USER_CUSTOM;
	_pt_type = p_wave_table->get_default_pitch_table_type();

	_wave_table = p_wave_table->get_wavelet();
	_wave_fixed_bits = p_wave_table->get_fixed_bits();
	_update_wave_table_cache();
}

void SiOPMOperator::set_pcm_data(const Ref<SiOPMWavePCMData> &p_pcm_data) {
	if (p_pcm_data.is_valid() && !p_pcm_data->get_wavelet().is_empty()) {
		_pg_type = SiONPulseGeneratorType::PULSE_USER_PCM;
		_pt_type = SiONPitchTableType::PITCH_TABLE_PCM;

		_wave_table = p_pcm_data->get_wavelet();
		_wave_fixed_bits = PCM_WAVE_FIXED_BITS;
		_update_wave_table_cache();

		_pcm_channel_num = p_pcm_data->get_channel_count();
		_pcm_start_point = p_pcm_data->get_start_point();
		_pcm_end_point = p_pcm_data->get_end_point();
		_pcm_loop_point = p_pcm_data->get_loop_point();

		_key_on_phase = _pcm_start_point << PCM_WAVE_FIXED_BITS;
	} else {
		// Quick initialization for SiOPMChannelPCM.
		_pcm_end_point = _pcm_loop_point = 0;
		_pcm_loop_point = -1;
	}
}

void SiOPMOperator::note_on() {
	_eg_ssgec_state = -1;

	// Envelope- and phase-aware voice stealing:
	// If a new note is triggered while this operator is still audible, do not
	// jump straight into ATTACK or reset the oscillator phase. Instead, force
	// the current envelope to finish a RELEASE down to silence and only then
	// start ATTACK and apply the phase reset. This guarantees that both the
	// amplitude and the waveform are continuous at non-zero levels.
	const bool envelope_audible =
		(_eg_state != EG_OFF && _eg_level < SiOPMRefTable::ENV_BOTTOM);
	const bool treat_as_voice_steal = _is_voice_steal_hint || envelope_audible;

	if (treat_as_voice_steal) {
		// Defer attack and phase reset until envelope is quiet.
		// Fast release is implicit - will be detected in _shift_eg_state(EG_RELEASE).
		_deferred_attack_target = EG_ATTACK;

		// If we are not already in RELEASE, switch to it now so the current
		// envelope decays smoothly towards silence using the fast release rate.
		if (_eg_state != EG_RELEASE) {
			_shift_eg_state(EG_RELEASE);
		} else {
			// Already in RELEASE but with natural rate - re-shift to apply fast rate.
			_shift_eg_state(EG_RELEASE);
		}
	} else {
		// Envelope is effectively silent already: we can safely reset phases
		// and jump straight into ATTACK without introducing discontinuities.
		_deferred_attack_target = EG_OFF;
		_reset_note_phases();
		_shift_eg_state(EG_ATTACK);
	}

	// Voice-steal hint is one-shot per note.
	_is_voice_steal_hint = false;

	update_eg_output();

	// // Debug: log EG state at attack start to diagnose clicky voice starts.
	// // Uses UtilityFunctions::print so it shows up in the Godot console.
	// UtilityFunctions::print(
	// 	"FM_OP_NOTE_ON",
	// 	" ptr=", (int64_t)this,
	// 	" prev_state=", (int)prev_state,
	// 	" prev_level=", prev_level,
	// 	" prev_output=", prev_output,
	// 	" new_state=", (int)_eg_state,
	// 	" new_level=", _eg_level,
	// 	" new_output=", _eg_output,
	// 	" phase=", _phase
	// );
}

void SiOPMOperator::note_off() {
	_shift_eg_state(EG_RELEASE);
	update_eg_output();
}

//

void SiOPMOperator::initialize() {
	// Defensive check: ensure sound chip pointer is valid.
	// This should not happen in normal operation, but protects against
	// edge cases during driver reconstruction or pool reuse issues.
	if (unlikely(_sound_chip == nullptr)) {
		ERR_FAIL_MSG("SiOPMOperator::initialize() called with null sound chip pointer. This indicates a lifecycle bug.");
	}

	// Reset operator connections.
	_final = true;
	_in_pipe   = _sound_chip->get_zero_buffer();
	_base_pipe = _sound_chip->get_zero_buffer();
	_feed_pipe->get()->value = 0;

	// Reset all parameters.
	Ref<SiOPMOperatorParams> init_params = _sound_chip->get_init_operator_params();
	if (unlikely(init_params.is_null())) {
		ERR_FAIL_MSG("SiOPMOperator::initialize() called but sound chip has null init operator params.");
	}
	set_operator_params(init_params);

	// Reset some other parameters.

	_eg_tl_offset  = 0;
	_pitch_index_shift2 = 0;

	_super_count = 1;
	_super_spread = 0;
	_super_stereo_spread = 0;
	for (int i = 0; i < MAX_SUPER_VOICES; i++) {
		_super_phases[i] = 0;
		_super_phase_steps[i] = 0;
		_super_pan_values[i] = 64; // Center
	}

	_pcm_channel_num = 0;
	_pcm_start_point = 0;
	_pcm_end_point = 0;
	_pcm_loop_point = -1;

	// Reset PG and EG states.
	reset();

	// // Debug: log EG state at operator (re)initialization to verify that
	// // voices start from a fully silent state before any new note_on.
	// UtilityFunctions::print(
	// 	"FM_OP_INIT",
	// 	" ptr=", (int64_t)this,
	// 	" eg_state=", (int)_eg_state,
	// 	" eg_level=", _eg_level,
	// 	" eg_output=", _eg_output,
	// 	" phase=", _phase
	// );
}

void SiOPMOperator::reset() {
	_shift_eg_state(EG_OFF);
	update_eg_output();
	_eg_timer = SiOPMRefTable::ENV_TIMER_INITIAL;
	_eg_counter = 0;
	_eg_ssgec_state = 0;

	// Clear voice-stealing state.
	_deferred_attack_target = EG_OFF;
	_is_voice_steal_hint = false;

	_phase = 0;
}

String SiOPMOperator::_to_string() const {
	String params = "";

	params += "pg=" + itos(_pg_type) + ", ";
	params += "pt=" + itos(_pt_type) + ", ";

	params += "ar=" + itos(_attack_rate) + ", ";
	params += "dr=" + itos(_decay_rate) + ", ";
	params += "sr=" + itos(_sustain_rate) + ", ";
	params += "rr=" + itos(_release_rate) + ", ";
	params += "sl=" + itos(_sustain_level) + ", ";
	params += "tl=" + itos(_total_level) + ", ";

	params += "keyscale=(" + itos(get_key_scaling_rate()) + ", " + itos(get_key_scaling_level()) + "), ";
	params += "fmul=" + itos(get_fine_multiple()) + ", ";
	params += "detune=(" + itos(get_detune1()) + ", " + itos(get_ptss_detune()) + "), ";

	params += "amp=" + itos(get_amplitude_modulation_shift()) + ", ";
	params += "phase=" + itos(get_key_on_phase()) + ", ";
	params += "note=" + String(is_pitch_fixed() ? "yes" : "no") + ", ";

	params += "ssgec=" + itos(_ssg_type) + ", ";
	params += "mute=" + itos(_mute) + ", ";
	params += "reset=" + String(_envelope_reset_on_attack ? "yes" : "no");

	return "SiOPMOperator: " + params;
}

SiOPMOperator::SiOPMOperator(SiOPMSoundChip *p_chip) {
	_table = SiOPMRefTable::get_instance();
	_sound_chip = p_chip;

	_feed_pipe = memnew(SinglyLinkedList<int>(1, 0, true));
	_eg_increment_table = make_vector<int>(_table->eg_increment_tables[17]);
	_eg_level_table = make_vector<int>(_table->eg_level_tables[0]);
}
