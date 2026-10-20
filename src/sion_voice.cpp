/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "sion_voice.h"
#include "chip/channels/siopm_channel_ks.h"
#include "engine_param_api.h"

#include <godot_cpp/classes/reg_ex.hpp>
#include <godot_cpp/classes/reg_ex_match.hpp>
#include <godot_cpp/core/memory.hpp>

#include "sion_enums.h"
#include "chip/siopm_channel_params.h"
#include "chip/siopm_operator_params.h"
#include "chip/siopm_ref_table.h"
#include "chip/wave/siopm_wave_pcm_data.h"
#include "chip/wave/siopm_wave_pcm_table.h"
#include "chip/wave/siopm_wave_sampler_data.h"
#include "chip/wave/siopm_wave_sampler_table.h"
#include "chip/wave/siopm_wave_table.h"
#include "utils/godot_util.h"
#include "utils/translator_util.h"
#include "sequencer/simml_track.h"

void SiONVoice::set_params(TypedArray<int> p_args) {
	Vector<int> data = make_vector_from_typed_array<int>(p_args);
	::TranslatorUtil::set_siopm_params(channel_params, data);
	chip_type = SiONChipType::CHIP_SIOPM;
}

void SiONVoice::set_params_opl(TypedArray<int> p_args) {
	Vector<int> data = make_vector_from_typed_array<int>(p_args);
	::TranslatorUtil::set_opl_params(channel_params, data);
	chip_type = SiONChipType::CHIP_OPL;
}

void SiONVoice::set_params_opm(TypedArray<int> p_args) {
	Vector<int> data = make_vector_from_typed_array<int>(p_args);
	::TranslatorUtil::set_opm_params(channel_params, data);
	chip_type = SiONChipType::CHIP_OPM;
}

void SiONVoice::set_params_opn(TypedArray<int> p_args) {
	Vector<int> data = make_vector_from_typed_array<int>(p_args);
	::TranslatorUtil::set_opn_params(channel_params, data);
	chip_type = SiONChipType::CHIP_OPN;
}

void SiONVoice::set_params_opx(TypedArray<int> p_args) {
	Vector<int> data = make_vector_from_typed_array<int>(p_args);
	::TranslatorUtil::set_opx_params(channel_params, data);
	chip_type = SiONChipType::CHIP_OPX;
}

void SiONVoice::set_params_ma3(TypedArray<int> p_args) {
	Vector<int> data = make_vector_from_typed_array<int>(p_args);
	::TranslatorUtil::set_ma3_params(channel_params, data);
	chip_type = SiONChipType::CHIP_MA3;

}

void SiONVoice::set_params_al(TypedArray<int> p_args) {
	Vector<int> data = make_vector_from_typed_array<int>(p_args);
	::TranslatorUtil::set_al_params(channel_params, data);
	chip_type = SiONChipType::CHIP_ANALOG_LIKE;

}

TypedArray<int> SiONVoice::get_params() const {
	Vector<int> data = ::TranslatorUtil::get_siopm_params(channel_params);
	return make_typed_array_from_vector<int>(data);
}

TypedArray<int> SiONVoice::get_params_opl() const {
	Vector<int> data = ::TranslatorUtil::get_opl_params(channel_params);
	return make_typed_array_from_vector<int>(data);
}

TypedArray<int> SiONVoice::get_params_opm() const {
	Vector<int> data = ::TranslatorUtil::get_opm_params(channel_params);
	return make_typed_array_from_vector<int>(data);
}

TypedArray<int> SiONVoice::get_params_opn() const {
	Vector<int> data = ::TranslatorUtil::get_opn_params(channel_params);
	return make_typed_array_from_vector<int>(data);
}

TypedArray<int> SiONVoice::get_params_opx() const {
	Vector<int> data = ::TranslatorUtil::get_opx_params(channel_params);
	return make_typed_array_from_vector<int>(data);
}

TypedArray<int> SiONVoice::get_params_ma3() const {
	Vector<int> data = ::TranslatorUtil::get_ma3_params(channel_params);
	return make_typed_array_from_vector<int>(data);
}

TypedArray<int> SiONVoice::get_params_al() const {
	Vector<int> data = ::TranslatorUtil::get_al_params(channel_params);
	return make_typed_array_from_vector<int>(data);
}

String SiONVoice::get_mml(int p_index, SiONChipType p_chip_type, bool p_append_postfix) const {
	SiONChipType type = p_chip_type;
	if (type == SiONChipType::CHIP_AUTO) {
		type = chip_type;
	}

	String mml;
	switch (type) {
		case SiONChipType::CHIP_SIOPM:
			mml = "#@"    + itos(p_index) + ::TranslatorUtil::get_siopm_params_as_mml(channel_params, " ", "\n", _name);
			break;
		case SiONChipType::CHIP_PMS_GUITAR: {
			String data = ::TranslatorUtil::get_ks_params_as_mml(channel_params,
				int(engine_params.get(SiOPMChannelKS::PMS_TENSION, 0).integer), ",", "", "").trim_suffix("}");
			const EngineParamSchema &schema = *engine_params.schema();
			for (int i = 0; i < schema.count; i++) {
				const EngineParamDescriptor &d = schema.parameters[i];
				if (d.uses_local_state() && d.id != SiOPMChannelKS::PMS_TENSION) data += "," + itos(engine_params.get(d.id, 0).integer);
			}
			mml = "#KS@" + itos(p_index) + data + "}";
		} break;
		case SiONChipType::CHIP_OPL:
			mml = "#OPL@" + itos(p_index) + ::TranslatorUtil::get_opl_params_as_mml(channel_params, " ", "\n", _name);
			break;
		case SiONChipType::CHIP_OPM:
			mml = "#OPM@" + itos(p_index) + ::TranslatorUtil::get_opm_params_as_mml(channel_params, " ", "\n", _name);
			break;
		case SiONChipType::CHIP_OPN:
			mml = "#OPN@" + itos(p_index) + ::TranslatorUtil::get_opn_params_as_mml(channel_params, " ", "\n", _name);
			break;
		case SiONChipType::CHIP_OPX:
			mml = "#OPX@" + itos(p_index) + ::TranslatorUtil::get_opx_params_as_mml(channel_params, " ", "\n", _name);
			break;
		case SiONChipType::CHIP_MA3:
			mml = "#MA@"  + itos(p_index) + ::TranslatorUtil::get_ma3_params_as_mml(channel_params, " ", "\n", _name);
			break;
		case SiONChipType::CHIP_ANALOG_LIKE:
			mml = "#AL@"  + itos(p_index) + ::TranslatorUtil::get_al_params_as_mml(channel_params, " ", "\n", _name);
			break;
		case SiONChipType::CHIP_GUITAR6: {
			PackedStringArray values;
			const EngineParamSchema &schema = *engine_params.schema();
			for (int i = 0; i < schema.count; i++) {
				const EngineParamDescriptor &d = schema.parameters[i];
				EngineParamValue value = engine_params.get(d.id, 0);
				if (value.type == EngineParamType::BOOL) values.push_back(value.boolean ? "1" : "0");
				else values.push_back(::TranslatorUtil::format_mml_float(i == 0 ? value.real : value.real * 0.01));
			}
			mml = "#G6@" + itos(p_index) + "{" + String(",").join(values) + "}";
		} break;
		case SiONChipType::CHIP_STRATA:
			mml = "#BR@" + itos(p_index) + ::TranslatorUtil::get_engine_params_as_mml(engine_params);
			break;
		case SiONChipType::CHIP_MONOLITH: {
			mml = "#MO@" + itos(p_index) + ::TranslatorUtil::get_engine_params_as_mml(engine_params);
		} break;
		case SiONChipType::CHIP_IRON: {
			mml = "#IR@" + itos(p_index) + ::TranslatorUtil::get_engine_params_as_mml(engine_params);
		} break;
		case SiONChipType::CHIP_TALUS: {
			mml = "#TL@" + itos(p_index) + ::TranslatorUtil::get_engine_params_as_mml(engine_params);
		} break;
		default:
			ERR_FAIL_V_MSG("", vformat("SiONVoice: Chip type %d is unsupported for MML strings.", type));
	}

	if (p_append_postfix) {
		Ref<SiONVoice> this_voice = const_cast<SiONVoice *>(this);
		String postfix = ::TranslatorUtil::get_voice_setting_as_mml(this_voice);
		if (!postfix.is_empty()) {
			mml += "\n" + postfix;
		}
	}

	return mml + ";";
}

int SiONVoice::set_by_mml(String p_mml) {
	reset();

	// Godot's RegEx implementation doesn't support passing global flags, but PCRE2 allows local flags, which we can abuse.
	// (?s) enables single line mode (dot matches newline) for the entire expression.
	Ref<RegEx> re_command = RegEx::create_from_string("(?s)(#[A-Z0-9]*@)\\s*(\\d+)\\s*{(.*?)}(.*?);");
	Ref<RegExMatch> res = re_command->search(p_mml);
	if (res.is_null()) {
		return -1;
	}

	String command = res->get_string(1);
	String data = res->get_string(3);

	if (command == "#@") {
		::TranslatorUtil::parse_siopm_params(channel_params, data);
		chip_type = SiONChipType::CHIP_SIOPM;
	} else if (command == "#OPL@") {
		::TranslatorUtil::parse_opl_params(channel_params, data);
		chip_type = SiONChipType::CHIP_OPL;
	} else if (command == "#OPM@") {
		::TranslatorUtil::parse_opm_params(channel_params, data);
		chip_type = SiONChipType::CHIP_OPM;
	} else if (command == "#OPN@") {
		::TranslatorUtil::parse_opn_params(channel_params, data);
		chip_type = SiONChipType::CHIP_OPN;
	} else if (command == "#OPX@") {
		::TranslatorUtil::parse_opx_params(channel_params, data);
		chip_type = SiONChipType::CHIP_OPX;
	} else if (command == "#MA@") {
		::TranslatorUtil::parse_ma3_params(channel_params, data);
		chip_type = SiONChipType::CHIP_MA3;
	} else if (command == "#AL@") {
		::TranslatorUtil::parse_al_params(channel_params, data);
		chip_type = SiONChipType::CHIP_ANALOG_LIKE;
	} else if (command == "#KS@") {
		int ws, ar, dr, tl, fn, tn;
		::TranslatorUtil::parse_ks_params(channel_params, data, ws, ar, dr, tl, fn, tn);
		_set_engine_type(SiONModuleType::MODULE_KS);
		channel_num = 1;
		chip_type = SiONChipType::CHIP_PMS_GUITAR;
		PackedStringArray parts = data.split(",");
		String local = itos(tn);
		if (parts.size() > 6) local += "," + String(",").join(parts.slice(6));
		if (!::TranslatorUtil::parse_engine_params(engine_params, local)) return -1;
	} else if (command == "#G6@") {
		_set_engine_type(SiONModuleType::MODULE_GUITAR6);
		channel_num = 0;
		chip_type = SiONChipType::CHIP_GUITAR6;
		PackedStringArray parts = data.split(",");
		for (int i = 1; i < MIN(int(parts.size()), 8); i++) {
			ERR_FAIL_COND_V_MSG(!parts[i].strip_edges().is_valid_float(), -1, "Guitar6 MML parameter requires a float.");
			parts.set(i, ::TranslatorUtil::format_mml_float(parts[i].to_float() * 100.0));
		}

		if (!::TranslatorUtil::parse_engine_params(engine_params, String(",").join(parts))) return -1;
	} else if (command == "#BR@") {
		_set_engine_type(SiONModuleType::MODULE_STRATA);
		channel_num = 0;
		chip_type = SiONChipType::CHIP_STRATA;
		if (!::TranslatorUtil::parse_engine_params(engine_params, data)) return -1;
	} else if (command == "#MO@") {
		_set_engine_type(SiONModuleType::MODULE_MONOLITH);
		channel_num = 0;
		chip_type = SiONChipType::CHIP_MONOLITH;
		if (!::TranslatorUtil::parse_engine_params(engine_params, data)) return -1;
	} else if (command == "#IR@") {
		_set_engine_type(SiONModuleType::MODULE_IRON);
		channel_num = 0;
		chip_type = SiONChipType::CHIP_IRON;
		if (!::TranslatorUtil::parse_engine_params(engine_params, data)) return -1;
	} else if (command == "#TL@") {
		_set_engine_type(SiONModuleType::MODULE_TALUS);
		channel_num = 0;
		chip_type = SiONChipType::CHIP_TALUS;
		if (!::TranslatorUtil::parse_engine_params(engine_params, data)) return -1;
	} else {
		return -1;
	}

	String postfix = res->get_string(4);
	int voice_index = res->get_string(2).to_int();
	::TranslatorUtil::parse_voice_setting(this, postfix);

	Ref<RegEx> re_name = RegEx::create_from_string("^.*?(//\\s*(.+?))?[\\n\\r]");
	res = re_name->search(data);
	if (res.is_valid()) {
		_name = res->get_string(2);
	} else {
		_name = "";
	}

	return voice_index;
}

Ref<SiOPMWaveTable> SiONVoice::set_wave_table(Vector<double> *p_data) {
	_set_engine_type(SiONModuleType::MODULE_SCC);

	Vector<int> table;
	for (int i = 0; i < p_data->size(); i++) {
		int table_index = SiOPMRefTable::calculate_log_table_index((*p_data)[i]);
		table.append(table_index);
	}

	Ref<SiOPMWaveTable> wave_table = memnew(SiOPMWaveTable(table));
	wave_data = wave_table;

	return wave_data;
}

Ref<SiOPMWavePCMData> SiONVoice::set_pcm_voice(const Variant &p_data, int p_sampling_note, int p_src_channel_count, int p_channel_count) {
	_set_engine_type(SiONModuleType::MODULE_PCM);

	Ref<SiOPMWavePCMData> pcm_data = memnew(SiOPMWavePCMData(p_data, p_sampling_note * 64, p_src_channel_count, p_channel_count));
	wave_data = pcm_data;

	return wave_data;
}

Ref<SiOPMWavePCMData> SiONVoice::set_pcm_wave(int p_index, const Variant &p_data, int p_sampling_note, int p_key_range_from, int p_key_range_to, int p_src_channel_count, int p_channel_count) {
	if (get_module_type() != SiONModuleType::MODULE_PCM || channel_num != p_index) {
		wave_data = Ref<SiOPMWaveBase>();
	}

	_set_engine_type(SiONModuleType::MODULE_PCM);
	channel_num = p_index;

	Ref<SiOPMWavePCMTable> pcm_table = wave_data;
	if (pcm_table.is_null()) {
		pcm_table = Ref<SiOPMWavePCMTable>(memnew(SiOPMWavePCMTable));
		wave_data = pcm_table;
	}

	Ref<SiOPMWavePCMData> pcm_data = memnew(SiOPMWavePCMData(p_data, int(p_sampling_note * 64), p_src_channel_count, p_channel_count));
	pcm_table->set_key_range_data(pcm_data, p_key_range_from, p_key_range_to);

	return pcm_data;
}

Ref<SiOPMWaveSamplerData> SiONVoice::set_sampler_voice(const Variant &p_data, bool p_ignore_note_off, int p_channel_count) {
	_set_engine_type(SiONModuleType::MODULE_SAMPLE);

	Ref<SiOPMWaveSamplerData> sampler_data = memnew(SiOPMWaveSamplerData(p_data, p_ignore_note_off, 0, 2, p_channel_count));
	wave_data = sampler_data;

	return wave_data;
}

Ref<SiOPMWaveSamplerData> SiONVoice::set_sampler_wave(int p_index, const Variant &p_data, bool p_ignore_note_off, int p_pan, int p_src_channel_count, int p_channel_count, bool p_fixed_pitch) {
	_set_engine_type(SiONModuleType::MODULE_SAMPLE);

	Ref<SiOPMWaveSamplerTable> sampler_table = wave_data;
	if (sampler_table.is_null()) {
		sampler_table = Ref<SiOPMWaveSamplerTable>(memnew(SiOPMWaveSamplerTable));
		wave_data = sampler_table;
	}

	Ref<SiOPMWaveSamplerData> sampler_data = memnew(SiOPMWaveSamplerData(p_data, p_ignore_note_off, p_pan, p_src_channel_count, p_channel_count, p_fixed_pitch));
	sampler_table->set_sample(sampler_data, p_index & (SiOPMRefTable::NOTE_TABLE_SIZE - 1));

	return sampler_data;
}

void SiONVoice::set_pitch_envelope(TypedArray<int> p_envelope, int p_loop_point, int p_step) {
	Vector<int> envelope_vector = make_vector_from_typed_array<int>(p_envelope);
	UtilityFunctions::print("SiONVoice::set_pitch_envelope - envelope size: ", envelope_vector.size(), " first value: ", (envelope_vector.size() > 0 ? envelope_vector[0] : -999));
	Ref<SiMMLEnvelopeTable> envelope = memnew(SiMMLEnvelopeTable(envelope_vector, p_loop_point));
	UtilityFunctions::print("SiONVoice::set_pitch_envelope - envelope table created, is_null: ", envelope.is_null());
	set_note_on_pitch_envelope(envelope, p_step);
}

Ref<SiONVoice> SiONVoice::clone() {
	Ref<SiONVoice> new_voice;
	new_voice.instantiate();
	new_voice->copy_from(this);
	new_voice->_name = _name;

	return new_voice;
}

void SiONVoice::reset() {
	SiMMLVoice::reset();

	_name = "";
	set_update_track_parameters(true);
}

Ref<SiONVoice> SiONVoice::create(SiONModuleType p_module_type, int p_channel_num, int p_attack_rate, int p_release_rate, int p_pitch_shift, int p_connection_type, int p_wave_shape2, int p_pitch_shift2) {
	return memnew(SiONVoice(p_module_type, p_channel_num, p_attack_rate, p_release_rate, p_pitch_shift, p_connection_type, p_wave_shape2, p_pitch_shift2));
}

Ref<SiONVoice> SiONVoice::create_engine(SiONModuleType p_module_type, SiONChipType p_chip_type) {
	ERR_FAIL_COND_V_MSG(p_module_type >= SiONModuleType::MODULE_MAX, Ref<SiONVoice>(), "Unknown engine module type.");
	ERR_FAIL_COND_V_MSG(p_chip_type < SiONChipType::CHIP_AUTO || p_chip_type >= SiONChipType::CHIP_MAX, Ref<SiONVoice>(), "Unknown engine chip type.");

	const int channel = p_module_type == SiONModuleType::MODULE_KS ? 1 : 0;
	Ref<SiONVoice> voice = memnew(SiONVoice(p_module_type, channel));
	if (p_chip_type != SiONChipType::CHIP_AUTO) {
		voice->chip_type = p_chip_type;
	}
	if (p_chip_type == SiONChipType::CHIP_ANALOG_LIKE) {
		voice->channel_params->set_operator_count(2);
		voice->channel_params->set_analog_like(true);
		voice->channel_params->set_analog_balance(0);
		voice->channel_params->get_operator_params(0)->set_detune2(0);
		voice->channel_params->get_operator_params(1)->set_detune2(0);
	}
	return voice;
}

TypedArray<Dictionary> SiONVoice::get_engine_param_descriptors(int p_engine_kind) {
	const EngineParamSchema *schema = EngineParamSchema::find_engine(p_engine_kind);
	TypedArray<Dictionary> result;
	ERR_FAIL_NULL_V_MSG(schema, result, "Engine kind has no registered scalar parameter contract.");
	for (int i = 0; i < schema->parameter_count(); i++) {
		const EngineParamDescriptor &d = schema->parameter_at(i);
		Dictionary descriptor;
		descriptor["engine_kind_id"] = schema->engine_kind;
		descriptor["param_id"] = int64_t(d.id);
		descriptor["id"] = d.name;
		descriptor["scalar_type"] = engine_param_variant_type(d.type);
		descriptor["min_value"] = d.minimum;
		descriptor["max_value"] = d.maximum;
		descriptor["reset_value"] = encode_engine_param(d.reset_value);
		descriptor["instance_count"] = d.instance_count;
		descriptor["realtime"] = d.realtime;
		result.push_back(descriptor);
	}
	return result;
}

void SiONVoice::set_engine_param(int p_param_id, const Variant &p_value, int p_index) {
	const EngineParamSchema *schema = EngineParamSchema::find_engine(get_module_type());
	ERR_FAIL_NULL_MSG(schema, "Voice engine has no scalar parameter contract.");
	const EngineParamDescriptor *d = schema->find(p_param_id);
	ERR_FAIL_NULL_MSG(d, "Unknown engine parameter ID.");
	ERR_FAIL_COND_MSG(!d->has_instance(p_index), "Engine parameter index is outside its registered instances.");
	EngineParamValue value;
	if (!decode_engine_param(*d, p_value, value)) return;
	d->set_prepared(*this, p_param_id, p_index, value);
}

Variant SiONVoice::get_engine_param(int p_param_id, int p_index) const {
	const EngineParamSchema *schema = EngineParamSchema::find_engine(get_module_type());
	ERR_FAIL_NULL_V_MSG(schema, Variant(), "Voice engine has no scalar parameter contract.");
	const EngineParamDescriptor *d = schema->find(p_param_id);
	ERR_FAIL_NULL_V_MSG(d, Variant(), "Unknown engine parameter ID.");
	ERR_FAIL_COND_V_MSG(!d->has_instance(p_index), Variant(), "Engine parameter index is outside its registered instances.");
	return encode_engine_param(d->get_prepared(*this, p_param_id, p_index));
}

void SiONVoice::_bind_methods() {
	// Factory.

	ClassDB::bind_static_method("SiONVoice", D_METHOD("create", "module_type", "channel_num", "attack_rate", "release_rate", "pitch_shift", "connection_type", "wave_shape2", "pitch_shift2"), &SiONVoice::create, DEFVAL(SiONModuleType::MODULE_GENERIC_PG), DEFVAL(0), DEFVAL(58), DEFVAL(63), DEFVAL(0), DEFVAL(-1), DEFVAL(0), DEFVAL(0));
	ClassDB::bind_static_method("SiONVoice", D_METHOD("create_engine", "module_type", "chip_type"), &SiONVoice::create_engine);
	ClassDB::bind_static_method("SiONVoice", D_METHOD("get_engine_param_descriptors", "engine_kind_id"), &SiONVoice::get_engine_param_descriptors);
	ClassDB::bind_method(D_METHOD("set_engine_param", "param_id", "value", "index"), &SiONVoice::set_engine_param, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_engine_param", "param_id", "index"), &SiONVoice::get_engine_param, DEFVAL(0));

	// Public API.

	// Apply voice parameters to an existing SiMMLTrack (real-time).
	ClassDB::bind_method(D_METHOD("update_track_voice", "track"), &SiONVoice::update_track_voice);

	ClassDB::bind_method(D_METHOD("get_name"), &SiONVoice::get_name);
	ClassDB::bind_method(D_METHOD("set_name", "value"), &SiONVoice::set_name);

	ClassDB::bind_method(D_METHOD("get_params"), &SiONVoice::get_params);
	ClassDB::bind_method(D_METHOD("get_params_opl"), &SiONVoice::get_params_opl);
	ClassDB::bind_method(D_METHOD("get_params_opm"), &SiONVoice::get_params_opm);
	ClassDB::bind_method(D_METHOD("get_params_opn"), &SiONVoice::get_params_opn);
	ClassDB::bind_method(D_METHOD("get_params_opx"), &SiONVoice::get_params_opx);
	ClassDB::bind_method(D_METHOD("get_params_ma3"), &SiONVoice::get_params_ma3);
	ClassDB::bind_method(D_METHOD("get_params_al"), &SiONVoice::get_params_al);

	ClassDB::bind_method(D_METHOD("set_params", "args"), &SiONVoice::set_params);
	ClassDB::bind_method(D_METHOD("set_params_opl", "args"), &SiONVoice::set_params_opl);
	ClassDB::bind_method(D_METHOD("set_params_opm", "args"), &SiONVoice::set_params_opm);
	ClassDB::bind_method(D_METHOD("set_params_opn", "args"), &SiONVoice::set_params_opn);
	ClassDB::bind_method(D_METHOD("set_params_opx", "args"), &SiONVoice::set_params_opx);
	ClassDB::bind_method(D_METHOD("set_params_ma3", "args"), &SiONVoice::set_params_ma3);
	ClassDB::bind_method(D_METHOD("set_params_al", "args"), &SiONVoice::set_params_al);

	ClassDB::bind_method(D_METHOD("get_mml", "index", "chip_type", "append_postfix"), &SiONVoice::get_mml, DEFVAL(SiONChipType::CHIP_AUTO), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("set_by_mml", "mml"), &SiONVoice::set_by_mml);
	const IronParams iron_defaults;
	const int *d = iron_defaults.values;
	const TalusParams talus_defaults;
	const int *t = talus_defaults.values;
	ClassDB::bind_method(D_METHOD("set_pitch_envelope", "envelope", "loop_point", "step"), &SiONVoice::set_pitch_envelope, DEFVAL(-1), DEFVAL(1));

	ClassDB::bind_method(D_METHOD("clone"), &SiONVoice::clone);

	ClassDB::bind_method(D_METHOD("set_sampler_wave", "index", "data", "ignore_note_off", "pan", "src_channel_count", "channel_count", "fixed_pitch"), &SiONVoice::set_sampler_wave, DEFVAL(false), DEFVAL(0), DEFVAL(2), DEFVAL(0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("set_sampler_voice", "data", "ignore_note_off", "channel_count"), &SiONVoice::set_sampler_voice, DEFVAL(false), DEFVAL(2));
	ClassDB::bind_method(D_METHOD("get_sampler_data", "note_number"), &SiONVoice::get_sampler_data);

	ClassDB::bind_method(D_METHOD("set_pitch_shift", "shift"), &SiONVoice::set_pitch_shift);
	ClassDB::bind_method(D_METHOD("get_pitch_shift"), &SiONVoice::get_pitch_shift);
	ClassDB::bind_method(D_METHOD("get_wave_data"), &SiONVoice::get_wave_data);
	ClassDB::bind_method(D_METHOD("set_wave_data", "data"), &SiONVoice::set_wave_data);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "pitch_shift"), "set_pitch_shift", "get_pitch_shift");
	ClassDB::bind_method(D_METHOD("set_pitch_bend", "bend"), &SiONVoice::set_pitch_bend);
	ClassDB::bind_method(D_METHOD("get_pitch_bend"), &SiONVoice::get_pitch_bend);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "pitch_bend"), "set_pitch_bend", "get_pitch_bend");

	ClassDB::add_property("SiONVoice", PropertyInfo(Variant::STRING, "name"), "set_name", "get_name");
}

SiONVoice::SiONVoice(SiONModuleType p_module_type, int p_channel_num, int p_attack_rate, int p_release_rate, int p_pitch_shift, int p_connection_type, int p_wave_shape2, int p_pitch_shift2) :
		SiMMLVoice() {
	set_update_track_parameters(true);

	set_module_type(p_module_type, p_channel_num);
	channel_params->get_operator_params(0)->set_attack_rate(p_attack_rate);
	channel_params->get_operator_params(0)->set_release_rate(p_release_rate);
	pitch_shift = p_pitch_shift;

	if (p_connection_type >= 0) {
		channel_params->set_operator_count(2);
		channel_params->set_analog_like(true);
		channel_params->set_algorithm(p_connection_type <= 2 ? p_connection_type : 0);
		channel_params->get_operator_params(0)->set_pulse_generator_type(p_channel_num);
		channel_params->get_operator_params(1)->set_pulse_generator_type(p_wave_shape2);
		channel_params->get_operator_params(1)->set_detune2(p_pitch_shift2);
	}
}
