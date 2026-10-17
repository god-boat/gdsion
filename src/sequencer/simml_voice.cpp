/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "simml_voice.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>

#include "sion_enums.h"
#include "chip/channels/siopm_channel_strata.h"
#include "chip/channels/siopm_channel_monolith.h"
#include "chip/channels/siopm_channel_iron.h"
#include "chip/channels/siopm_channel_talus.h"
#include "chip/channels/siopm_channel_guitar6.h"
#include "chip/channels/siopm_channel_ks.h"
#include "chip/wave/siopm_wave_sampler_data.h"
#include "chip/channels/siopm_channel_base.h"
#include "chip/siopm_operator_params.h"
#include "chip/wave/siopm_wave_pcm_data.h"
#include "chip/wave/siopm_wave_pcm_table.h"
#include "chip/wave/siopm_wave_sampler_table.h"
#include "chip/wave/siopm_wave_table.h"
#include "sequencer/simml_envelope_table.h"
#include "sequencer/simml_ref_table.h"
#include "sequencer/simml_track.h"
#include "sequencer/simml_channel_settings.h"
#include "utils/godot_util.h"
#include <iterator>

using namespace godot;

const EngineParamSchema &SiMMLVoice::get_shared_engine_param_schema() {
	static const EngineParamDescriptor parameters[] = {
		{AMPLITUDE_MOD_DEPTH, "voice.amplitude_modulation.depth", EngineParamType::INT, -128, 127, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.amplitude_modulation_depth = int(value.integer); voice.channel_params->set_amplitude_modulation_depth(int(value.integer)); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.amplitude_modulation_depth); }},
		{AMPLITUDE_MOD_END_DEPTH, "voice.amplitude_modulation.end_depth", EngineParamType::INT, -128, 127, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.amplitude_modulation_depth_end = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.amplitude_modulation_depth_end); }},
		{AMPLITUDE_MOD_DELAY, "voice.amplitude_modulation.delay", EngineParamType::INT, 0, 255, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.amplitude_modulation_delay = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.amplitude_modulation_delay); }},
		{AMPLITUDE_MOD_DURATION, "voice.amplitude_modulation.duration", EngineParamType::INT, 0, 255, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.amplitude_modulation_term = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.amplitude_modulation_term); }},
		{PITCH_MOD_DEPTH, "voice.pitch_modulation.depth", EngineParamType::INT, -128, 127, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.pitch_modulation_depth = int(value.integer); voice.channel_params->set_pitch_modulation_depth(int(value.integer)); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.pitch_modulation_depth); }},
		{PITCH_MOD_END_DEPTH, "voice.pitch_modulation.end_depth", EngineParamType::INT, -128, 127, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.pitch_modulation_depth_end = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.pitch_modulation_depth_end); }},
		{PITCH_MOD_DELAY, "voice.pitch_modulation.delay", EngineParamType::INT, 0, 255, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.pitch_modulation_delay = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.pitch_modulation_delay); }},
		{PITCH_MOD_DURATION, "voice.pitch_modulation.duration", EngineParamType::INT, 0, 255, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.pitch_modulation_term = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.pitch_modulation_term); }},

		{PITCH_BEND, "voice.pitch_bend", EngineParamType::INT, -8192, 8191, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.pitch_bend = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.pitch_bend); }},
		{GLIDE_TIME, "voice.glide.time", EngineParamType::INT, 0, 2147483647, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.portament_ms = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.portament_ms); }},
		{MONO_ENABLED, "voice.mono.enabled", EngineParamType::BOOL, 0, 1, EngineParamValue::from_bool(false), 1, false,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.mono = value.boolean; },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_bool(voice.mono); }},
		{MONO_PRIORITY, "voice.mono.priority", EngineParamType::INT, 0, 2, EngineParamValue::from_int(0), 1, false,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.mono_priority = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.mono_priority); }},
		{MONO_LEGATO, "voice.mono.legato", EngineParamType::BOOL, 0, 1, EngineParamValue::from_bool(true), 1, false,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.mono_legato = value.boolean; },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_bool(voice.mono_legato); }},
		{GLIDE_MODE, "voice.glide.mode", EngineParamType::INT, 0, 1, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.portament_time_mode = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.portament_time_mode); }},
		{GLIDE_DIVISION, "voice.glide.sync_division", EngineParamType::INT, 0, 5, EngineParamValue::from_int(4), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.portament_sync_division = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.portament_sync_division); }},
		{GLIDE_ALWAYS, "voice.glide.always", EngineParamType::BOOL, 0, 1, EngineParamValue::from_bool(false), 1, false,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.glide_always = value.boolean; },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_bool(voice.glide_always); }},
		{PITCH_OFFSET, "voice.pitch.offset", EngineParamType::INT, -8192, 8191, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.pitch_shift = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.pitch_shift); }},
		{EXPRESSION, "voice.expression", EngineParamType::INT, 0, 128, EngineParamValue::from_int(128), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.expression = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.expression); }},
		{NOTE_OFFSET, "voice.note.offset", EngineParamType::INT, -127, 127, EngineParamValue::from_int(0), 1, true,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.note_shift = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.note_shift); }},
		{TONE_INDEX, "voice.tone.index", EngineParamType::INT, -1, 127, EngineParamValue::from_int(-1), 1, false,
			[](SiMMLVoice &voice, uint32_t, int, EngineParamValue value) { voice.tone_num = int(value.integer); },
			[](const SiMMLVoice &voice, uint32_t, int) { return EngineParamValue::from_int(voice.tone_num); }},
	};
	static const EngineParamSchema schema = {-1, parameters, int(std::size(parameters))};
	return schema;
}

bool SiMMLVoice::is_fm_voice() const {
	return module_type == SiONModuleType::MODULE_FM;
}

bool SiMMLVoice::is_pcm_voice() const {
	Ref<SiOPMWavePCMTable> pcm_table = wave_data;
	if (pcm_table.is_valid()) {
		return true;
	}

	Ref<SiOPMWavePCMData> pcm_data = wave_data;
	if (pcm_data.is_valid()) {
		return true;
	}

	return false;
}

bool SiMMLVoice::is_sampler_voice() const {
	Ref<SiOPMWaveSamplerTable> sampler_table = wave_data;

	return sampler_table.is_valid();
}

Ref<SiOPMWaveSamplerData> SiMMLVoice::get_sampler_data(int p_note_number) const {
	if (get_module_type() != SiONModuleType::MODULE_SAMPLE) {
		return Ref<SiOPMWaveSamplerData>();
	}

	if (wave_data.is_null()) {
		return Ref<SiOPMWaveSamplerData>();
	}

	// Case 1: wave_data is a direct sampler data object
	Ref<SiOPMWaveSamplerData> sd = wave_data;
	if (sd.is_valid()) {
		return sd;
	}

	// Case 2: wave_data is a sampler table
	Ref<SiOPMWaveSamplerTable> tbl = wave_data;
	if (tbl.is_valid()) {
		int idx = p_note_number & (SiOPMRefTable::NOTE_TABLE_SIZE - 1);
		return tbl->get_sample(idx);
	}

	return Ref<SiOPMWaveSamplerData>();
}

bool SiMMLVoice::is_wave_table_voice() const {
	Ref<SiOPMWaveTable> wave_table = wave_data;

	return wave_table.is_valid();
}

bool SiMMLVoice::is_suitable_for_fm_voice() const {
	return update_track_parameters || (SiMMLRefTable::get_instance()->is_suitable_for_fm_voice(module_type) && wave_data == nullptr);
}

void SiMMLVoice::_set_engine_type(SiONModuleType p_module_type) {
	if (module_type != p_module_type) {
		module_type = p_module_type;
		engine_params.reset(EngineParamSchema::find_engine(module_type));
	}
}

void SiMMLVoice::set_module_type(SiONModuleType p_module_type, int p_channel_num, int p_tone_num) {
	_set_engine_type(p_module_type);
	channel_num = p_channel_num;
	tone_num = p_tone_num;

	int pg_type = -1;
	// SUS: This is needed because this method can be indirectly called by Godot editor's docgen. But this smells...
	if (SiMMLRefTable::get_instance()) {
		pg_type = SiMMLRefTable::get_instance()->get_pulse_generator_type(module_type, channel_num, tone_num);
	}
	if (pg_type != -1) {
		channel_params->get_operator_params(0)->set_pulse_generator_type(pg_type);
	}
}

bool SiMMLVoice::has_amplitude_modulation() const {
	return amplitude_modulation_depth != 0 || amplitude_modulation_depth_end != 0;
}

bool SiMMLVoice::has_pitch_modulation() const {
	return pitch_modulation_depth != 0 || pitch_modulation_depth_end != 0;
}

void SiMMLVoice::update_track_voice(SiMMLTrack *p_track) {
	// Fast-path: check if existing channel type matches what we need.
	// This avoids reinitializing the channel, which preserves DSP state
	// (envelopes, filters, fade) and helps prevent clicks during rapid note triggering.
	SiOPMChannelBase *existing_ch = p_track->get_channel();
	bool needs_rebind = false;

	switch (module_type) {
		case SiONModuleType::MODULE_FM: {
			// Check if already an FM channel
			SiOPMChannelFM *fm_ch = existing_ch ? dynamic_cast<SiOPMChannelFM *>(existing_ch) : nullptr;
			if (!fm_ch) {
				p_track->set_channel_module_type(SiONModuleType::MODULE_FM, channel_num);
			}
			p_track->get_channel()->set_channel_params(channel_params, update_volumes, true);
			p_track->reset_volume_offset();
		} break;

		case SiONModuleType::MODULE_KS: {
			SiOPMChannelKS *ks_ch = existing_ch ? dynamic_cast<SiOPMChannelKS *>(existing_ch) : nullptr;
			if (!ks_ch) {
				p_track->set_channel_module_type(SiONModuleType::MODULE_KS, 1);
				ks_ch = static_cast<SiOPMChannelKS *>(p_track->get_channel());
			}
			if (ks_ch) {
				ks_ch->set_all_release_rate(int(engine_params.get(SiOPMChannelKS::PMS_TENSION, 0).integer));
				ks_ch->set_channel_params(channel_params, update_volumes, true);
				p_track->reset_volume_offset();
				if (is_pcm_voice() && wave_data.is_valid()) {
					ks_ch->set_wave_data(wave_data);
				}

			}
		} break;

		case SiONModuleType::MODULE_GUITAR6: {
			SiOPMChannelGuitar6 *g6_ch = existing_ch ? dynamic_cast<SiOPMChannelGuitar6 *>(existing_ch) : nullptr;
			if (!g6_ch) {
				p_track->set_channel_module_type(SiONModuleType::MODULE_GUITAR6, 0);
				g6_ch = dynamic_cast<SiOPMChannelGuitar6 *>(p_track->get_channel());
			}
			if (g6_ch) {
				g6_ch->set_channel_params(channel_params, update_volumes, true);
				p_track->reset_volume_offset();
			}
		} break;

		case SiONModuleType::MODULE_STRATA: {
			SiOPMChannelStrata *strata_ch = existing_ch ? dynamic_cast<SiOPMChannelStrata *>(existing_ch) : nullptr;
			if (!strata_ch) {
				p_track->set_channel_module_type(SiONModuleType::MODULE_STRATA, 0);
				strata_ch = dynamic_cast<SiOPMChannelStrata *>(p_track->get_channel());
			}
			if (strata_ch) {
				strata_ch->set_channel_params(channel_params, update_volumes, true);
				p_track->reset_volume_offset();
			}
		} break;

		case SiONModuleType::MODULE_MONOLITH: {
			SiOPMChannelMonolith *mono_ch = existing_ch ? dynamic_cast<SiOPMChannelMonolith *>(existing_ch) : nullptr;
			if (!mono_ch) {
				p_track->set_channel_module_type(SiONModuleType::MODULE_MONOLITH, 0);
				mono_ch = dynamic_cast<SiOPMChannelMonolith *>(p_track->get_channel());
			}
			if (mono_ch) {

				mono_ch->set_channel_params(channel_params, update_volumes, true);
				p_track->reset_volume_offset();
			}
		} break;

		case SiONModuleType::MODULE_IRON: {
			SiOPMChannelIron *iron_ch = dynamic_cast<SiOPMChannelIron *>(existing_ch);
			if (!iron_ch) {
				p_track->set_channel_module_type(SiONModuleType::MODULE_IRON, 0);
				iron_ch = dynamic_cast<SiOPMChannelIron *>(p_track->get_channel());
			}

			iron_ch->set_channel_params(channel_params, update_volumes, true);
			p_track->reset_volume_offset();
		} break;

		case SiONModuleType::MODULE_TALUS: {
			SiOPMChannelTalus *talus_ch = dynamic_cast<SiOPMChannelTalus *>(existing_ch);
			if (!talus_ch) {
				p_track->set_channel_module_type(SiONModuleType::MODULE_TALUS, 0);
				talus_ch = dynamic_cast<SiOPMChannelTalus *>(p_track->get_channel());
			}

			talus_ch->set_channel_params(channel_params, update_volumes, true);
			p_track->reset_volume_offset();
		} break;

		default: { // Other sound modules.
			// For wave data, check if the wave's module type matches
			if (wave_data.is_valid()) {
				SiMMLChannelSettings *wave_settings = SiMMLRefTable::get_instance()->channel_settings_map[wave_data->get_module_type()];
				needs_rebind = !existing_ch || !wave_settings ||
					(existing_ch->get_channel_type() != wave_settings->get_channel_type());
				
				if (needs_rebind) {
					p_track->set_channel_module_type(wave_data->get_module_type(), -1);
				}
				p_track->get_channel()->set_channel_params(channel_params, update_volumes);
				p_track->get_channel()->set_wave_data(wave_data);
			} else {
				// Check if channel type matches the module type
				SiMMLChannelSettings *settings = SiMMLRefTable::get_instance()->channel_settings_map[module_type];
				needs_rebind = !existing_ch || !settings ||
					(existing_ch->get_channel_type() != settings->get_channel_type());
				
				if (needs_rebind) {
					p_track->set_channel_module_type(module_type, channel_num, tone_num);
				}
				p_track->get_channel()->set_channel_params(channel_params, update_volumes);
				if (!needs_rebind) {
					p_track->reset_volume_offset();
				}
			}
		} break;
	}

	// Published preparation state is consumed through the same scalar operation
	// as live updates. Initialization does not introduce a second bulk protocol.
	const EngineParamSchema *schema = engine_params.schema();
	p_track->set_engine_kind_id(module_type);
	if (schema != nullptr) {
		for (const EngineParamDescriptor *d : schema->local_parameters) {
			if (!d->realtime || !d->applies_on_voice) continue;
			for (int index = 0; index < d->instance_count; index++) {
				p_track->get_channel()->apply_engine_param(d->id, index, engine_params.get(d->id, index));
			}
		}
	}

	if (!Math::is_nan(default_gate_time)) {
		p_track->set_quantize_ratio(default_gate_time);
	}

	p_track->set_pitch_shift(pitch_shift);
	p_track->set_pitch_bend(pitch_bend);
	p_track->set_note_shift(note_shift);
	p_track->set_velocity_shift(velocity_shift);

	p_track->set_velocity_mode(velocity_mode);
	p_track->set_expression_mode(expression_mode);
	if (update_volumes) {
		p_track->set_velocity(velocity);
		p_track->set_expression(expression);
	}

	p_track->set_portament_ms(portament_ms);
	p_track->set_portament_time_mode(portament_time_mode);
	p_track->set_portament_sync_division(portament_sync_division);
	p_track->set_release_sweep(release_sweep);
	p_track->set_modulation_envelope(false, amplitude_modulation_depth, amplitude_modulation_depth_end, amplitude_modulation_delay, amplitude_modulation_term);
	p_track->set_modulation_envelope(true,  pitch_modulation_depth, pitch_modulation_depth_end, pitch_modulation_delay, pitch_modulation_term);
	{
		p_track->set_tone_envelope(1, note_on_tone_envelope, note_on_tone_envelope_step);
		p_track->set_amplitude_envelope(1, note_on_amplitude_envelope, note_on_amplitude_envelope_step);
		p_track->set_filter_envelope(1, note_on_filter_envelope, note_on_filter_envelope_step);
		p_track->set_pitch_envelope(1, note_on_pitch_envelope, note_on_pitch_envelope_step);
		p_track->set_note_envelope(1, note_on_note_envelope, note_on_note_envelope_step);
		p_track->set_tone_envelope(0, note_off_tone_envelope, note_off_tone_envelope_step);
		p_track->set_amplitude_envelope(0, note_off_amplitude_envelope, note_off_amplitude_envelope_step);
		p_track->set_filter_envelope(0, note_off_filter_envelope, note_off_filter_envelope_step);
		p_track->set_pitch_envelope(0, note_off_pitch_envelope, note_off_pitch_envelope_step);
		p_track->set_note_envelope(0, note_off_note_envelope, note_off_note_envelope_step);
	}
}

Ref<SiMMLVoice> SiMMLVoice::create_blank_pcm_voice(int p_channel_num) {
	Ref<SiMMLVoice> instance;
	instance.instantiate();
	instance->_set_engine_type(SiONModuleType::MODULE_PCM);
	instance->channel_num = p_channel_num;
	instance->wave_data = Ref<SiOPMWavePCMTable>(memnew(SiOPMWavePCMTable));

	return instance;
}

void SiMMLVoice::reset() {
	update_track_parameters = false;
	update_volumes = false;

	chip_type = SiONChipType::CHIP_SIOPM;
	_set_engine_type(SiONModuleType::MODULE_GENERIC_PG);
	channel_num = -1;
	tone_num = -1;
	preferable_note = -1;

	channel_params->initialize();
	wave_data = Ref<SiOPMWaveBase>();

	engine_params.reset(EngineParamSchema::find_engine(module_type));

	default_gate_time = NAN;
	default_gate_ticks = -1;
	default_key_on_delay_ticks = -1;
	pitch_shift = 0;
	pitch_bend = 0;
	note_shift = 0;
	portament_ms = 0;
	portament_time_mode = 0;
	portament_sync_division = 4;
	mono = false;
	mono_priority = 0;
	mono_legato = true;
	glide_always = false;
	release_sweep = 0;

	velocity = 256;
	expression = 128;
	velocity_mode = 0;
	velocity_shift = 4;
	expression_mode = 0;

	amplitude_modulation_depth = 0;
	amplitude_modulation_depth_end = 0;
	amplitude_modulation_delay = 0;
	amplitude_modulation_term = 0;
	pitch_modulation_depth = 0;
	pitch_modulation_depth_end = 0;
	pitch_modulation_delay = 0;
	pitch_modulation_term = 0;

	note_on_tone_envelope = Ref<SiMMLEnvelopeTable>();
	note_on_amplitude_envelope = Ref<SiMMLEnvelopeTable>();
	note_on_filter_envelope = Ref<SiMMLEnvelopeTable>();
	note_on_pitch_envelope = Ref<SiMMLEnvelopeTable>();
	note_on_note_envelope = Ref<SiMMLEnvelopeTable>();
	note_off_tone_envelope = Ref<SiMMLEnvelopeTable>();
	note_off_amplitude_envelope = Ref<SiMMLEnvelopeTable>();
	note_off_filter_envelope = Ref<SiMMLEnvelopeTable>();
	note_off_pitch_envelope = Ref<SiMMLEnvelopeTable>();
	note_off_note_envelope = Ref<SiMMLEnvelopeTable>();

	note_on_tone_envelope_step = 1;
	note_on_amplitude_envelope_step = 1;
	note_on_filter_envelope_step = 1;
	note_on_pitch_envelope_step = 1;
	note_on_note_envelope_step = 1;
	note_off_tone_envelope_step = 1;
	note_off_amplitude_envelope_step = 1;
	note_off_filter_envelope_step = 1;
	note_off_pitch_envelope_step = 1;
	note_off_note_envelope_step = 1;
}

void SiMMLVoice::copy_from(const Ref<SiMMLVoice> &p_source) {
	chip_type = p_source->chip_type;

	update_track_parameters = p_source->update_track_parameters;
	update_volumes = p_source->update_volumes;

	_set_engine_type(p_source->module_type);
	channel_num = p_source->channel_num;
	tone_num = p_source->tone_num;
	preferable_note = p_source->preferable_note;

	channel_params->copy_from(p_source->channel_params);
	wave_data = p_source->wave_data;

	engine_params = p_source->engine_params;

	default_gate_time = p_source->default_gate_time;
	default_gate_ticks = p_source->default_gate_ticks;
	default_key_on_delay_ticks = p_source->default_key_on_delay_ticks;
	pitch_shift = p_source->pitch_shift;
	pitch_bend = p_source->pitch_bend;
	note_shift = p_source->note_shift;
	portament_ms = p_source->portament_ms;
	portament_time_mode = p_source->portament_time_mode;
	portament_sync_division = p_source->portament_sync_division;
	mono = p_source->mono;
	mono_priority = p_source->mono_priority;
	mono_legato = p_source->mono_legato;
	glide_always = p_source->glide_always;
	release_sweep = p_source->release_sweep;

	velocity = p_source->velocity;
	expression = p_source->expression;
	velocity_mode = p_source->velocity_mode;
	velocity_shift = p_source->velocity_shift;
	expression_mode = p_source->expression_mode;

	amplitude_modulation_depth = p_source->amplitude_modulation_depth;
	amplitude_modulation_depth_end = p_source->amplitude_modulation_depth_end;
	amplitude_modulation_delay = p_source->amplitude_modulation_delay;
	amplitude_modulation_term = p_source->amplitude_modulation_term;
	pitch_modulation_depth = p_source->pitch_modulation_depth;
	pitch_modulation_depth_end = p_source->pitch_modulation_depth_end;
	pitch_modulation_delay = p_source->pitch_modulation_delay;
	pitch_modulation_term = p_source->pitch_modulation_term;

#define COPY_NOTE_ENVELOPE(m_prop)            \
	m_prop = Ref<SiMMLEnvelopeTable>();       \
	if (p_source->m_prop.is_valid()) {        \
		m_prop.instantiate();                 \
		m_prop->copy_from(p_source->m_prop);  \
	}

	COPY_NOTE_ENVELOPE(note_on_tone_envelope);
	COPY_NOTE_ENVELOPE(note_on_amplitude_envelope);
	COPY_NOTE_ENVELOPE(note_on_filter_envelope);
	COPY_NOTE_ENVELOPE(note_on_pitch_envelope);
	COPY_NOTE_ENVELOPE(note_on_note_envelope);
	COPY_NOTE_ENVELOPE(note_off_tone_envelope);
	COPY_NOTE_ENVELOPE(note_off_amplitude_envelope);
	COPY_NOTE_ENVELOPE(note_off_filter_envelope);
	COPY_NOTE_ENVELOPE(note_off_pitch_envelope);
	COPY_NOTE_ENVELOPE(note_off_note_envelope);
#undef COPY_NOTE_ENVELOPE

	note_on_tone_envelope_step = p_source->note_on_tone_envelope_step;
	note_on_amplitude_envelope_step = p_source->note_on_amplitude_envelope_step;
	note_on_filter_envelope_step = p_source->note_on_filter_envelope_step;
	note_on_pitch_envelope_step = p_source->note_on_pitch_envelope_step;
	note_on_note_envelope_step = p_source->note_on_note_envelope_step;
	note_off_tone_envelope_step = p_source->note_off_tone_envelope_step;
	note_off_amplitude_envelope_step = p_source->note_off_amplitude_envelope_step;
	note_off_filter_envelope_step = p_source->note_off_filter_envelope_step;
	note_off_pitch_envelope_step = p_source->note_off_pitch_envelope_step;
	note_off_note_envelope_step = p_source->note_off_note_envelope_step;
}

void SiMMLVoice::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_chip_type"), &SiMMLVoice::get_chip_type);
	ClassDB::bind_method(D_METHOD("set_chip_type", "type"), &SiMMLVoice::set_chip_type);
	ClassDB::bind_method(D_METHOD("get_module_type"), &SiMMLVoice::get_module_type);
	ClassDB::bind_method(D_METHOD("set_module_type", "module_type", "channel_num", "tone_num"), &SiMMLVoice::set_module_type, DEFVAL(0), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_channel_num"), &SiMMLVoice::get_channel_num);
	ClassDB::bind_method(D_METHOD("set_channel_num", "num"), &SiMMLVoice::set_channel_num);
	ClassDB::bind_method(D_METHOD("get_tone_num"), &SiMMLVoice::get_tone_num);
	ClassDB::bind_method(D_METHOD("set_tone_num", "num"), &SiMMLVoice::set_tone_num);
	ClassDB::bind_method(D_METHOD("get_note_shift"), &SiMMLVoice::get_note_shift);
	ClassDB::bind_method(D_METHOD("set_note_shift", "value"), &SiMMLVoice::set_note_shift);
	ClassDB::bind_method(D_METHOD("get_portament_ms"), &SiMMLVoice::get_portament_ms);
	ClassDB::bind_method(D_METHOD("set_portament_ms", "ms"), &SiMMLVoice::set_portament_ms);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::INT, "portament_ms"), "set_portament_ms", "get_portament_ms");
	ClassDB::bind_method(D_METHOD("get_portament_time_mode"), &SiMMLVoice::get_portament_time_mode);
	ClassDB::bind_method(D_METHOD("set_portament_time_mode", "mode"), &SiMMLVoice::set_portament_time_mode);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::INT, "portament_time_mode"), "set_portament_time_mode", "get_portament_time_mode");
	ClassDB::bind_method(D_METHOD("get_portament_sync_division"), &SiMMLVoice::get_portament_sync_division);
	ClassDB::bind_method(D_METHOD("set_portament_sync_division", "division"), &SiMMLVoice::set_portament_sync_division);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::INT, "portament_sync_division"), "set_portament_sync_division", "get_portament_sync_division");
	ClassDB::bind_method(D_METHOD("get_mono"), &SiMMLVoice::get_mono);
	ClassDB::bind_method(D_METHOD("set_mono", "mono"), &SiMMLVoice::set_mono);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::BOOL, "mono"), "set_mono", "get_mono");
	ClassDB::bind_method(D_METHOD("get_mono_priority"), &SiMMLVoice::get_mono_priority);
	ClassDB::bind_method(D_METHOD("set_mono_priority", "priority"), &SiMMLVoice::set_mono_priority);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::INT, "mono_priority"), "set_mono_priority", "get_mono_priority");
	ClassDB::bind_method(D_METHOD("get_mono_legato"), &SiMMLVoice::get_mono_legato);
	ClassDB::bind_method(D_METHOD("set_mono_legato", "legato"), &SiMMLVoice::set_mono_legato);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::BOOL, "mono_legato"), "set_mono_legato", "get_mono_legato");
	ClassDB::bind_method(D_METHOD("get_glide_always"), &SiMMLVoice::get_glide_always);
	ClassDB::bind_method(D_METHOD("set_glide_always", "always"), &SiMMLVoice::set_glide_always);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::BOOL, "glide_always"), "set_glide_always", "get_glide_always");

	ClassDB::bind_method(D_METHOD("is_fm_voice"), &SiMMLVoice::is_fm_voice);
	ClassDB::bind_method(D_METHOD("is_pcm_voice"), &SiMMLVoice::is_pcm_voice);
	ClassDB::bind_method(D_METHOD("is_sampler_voice"), &SiMMLVoice::is_sampler_voice);
	ClassDB::bind_method(D_METHOD("is_wave_table_voice"), &SiMMLVoice::is_wave_table_voice);

	ClassDB::bind_method(D_METHOD("get_channel_params"), &SiMMLVoice::get_channel_params);

	ClassDB::bind_method(D_METHOD("get_velocity"), &SiMMLVoice::get_velocity);
	ClassDB::bind_method(D_METHOD("set_velocity", "value"), &SiMMLVoice::set_velocity);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::INT, "velocity"), "set_velocity", "get_velocity");
	ClassDB::bind_method(D_METHOD("get_expression"), &SiMMLVoice::get_expression);
	ClassDB::bind_method(D_METHOD("set_expression", "value"), &SiMMLVoice::set_expression);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::INT, "expression"), "set_expression", "get_expression");
	ClassDB::bind_method(D_METHOD("get_velocity_mode"), &SiMMLVoice::get_velocity_mode);
	ClassDB::bind_method(D_METHOD("set_velocity_mode", "value"), &SiMMLVoice::set_velocity_mode);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::INT, "velocity_mode"), "set_velocity_mode", "get_velocity_mode");
	ClassDB::bind_method(D_METHOD("get_velocity_shift"), &SiMMLVoice::get_velocity_shift);
	ClassDB::bind_method(D_METHOD("set_velocity_shift", "value"), &SiMMLVoice::set_velocity_shift);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::INT, "velocity_shift"), "set_velocity_shift", "get_velocity_shift");
	ClassDB::bind_method(D_METHOD("get_expression_mode"), &SiMMLVoice::get_expression_mode);
	ClassDB::bind_method(D_METHOD("set_expression_mode", "value"), &SiMMLVoice::set_expression_mode);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::INT, "expression_mode"), "set_expression_mode", "get_expression_mode");

	ClassDB::bind_method(D_METHOD("should_update_track_parameters"), &SiMMLVoice::should_update_track_parameters);
	ClassDB::bind_method(D_METHOD("set_update_track_parameters", "enabled"), &SiMMLVoice::set_update_track_parameters);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::BOOL, "update_track_parameters"), "set_update_track_parameters", "should_update_track_parameters");

	ClassDB::bind_method(D_METHOD("should_update_volumes"), &SiMMLVoice::should_update_volumes);
	ClassDB::bind_method(D_METHOD("set_update_volumes", "enabled"), &SiMMLVoice::set_update_volumes);
	ClassDB::add_property("SiMMLVoice", PropertyInfo(Variant::BOOL, "update_volumes"), "set_update_volumes", "should_update_volumes");

	ClassDB::bind_method(D_METHOD("get_amplitude_modulation_depth"), &SiMMLVoice::get_amplitude_modulation_depth);
	ClassDB::bind_method(D_METHOD("get_amplitude_modulation_depth_end"), &SiMMLVoice::get_amplitude_modulation_depth_end);
	ClassDB::bind_method(D_METHOD("get_amplitude_modulation_delay"), &SiMMLVoice::get_amplitude_modulation_delay);
	ClassDB::bind_method(D_METHOD("get_amplitude_modulation_term"), &SiMMLVoice::get_amplitude_modulation_term);
	ClassDB::bind_method(D_METHOD("get_pitch_modulation_depth"), &SiMMLVoice::get_pitch_modulation_depth);
	ClassDB::bind_method(D_METHOD("get_pitch_modulation_depth_end"), &SiMMLVoice::get_pitch_modulation_depth_end);
	ClassDB::bind_method(D_METHOD("get_pitch_modulation_delay"), &SiMMLVoice::get_pitch_modulation_delay);
	ClassDB::bind_method(D_METHOD("get_pitch_modulation_term"), &SiMMLVoice::get_pitch_modulation_term);
}

SiMMLVoice::SiMMLVoice() {
	channel_params.instantiate();
	reset();
}
