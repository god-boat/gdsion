#include "engine_param_api.h"
#include <cmath>
#include <cstdlib>
#include <godot_cpp/core/error_macros.hpp>

bool decode_engine_param(const EngineParamDescriptor &p_descriptor, const godot::Variant &p_value, EngineParamValue &r_value) {
	switch (p_descriptor.type) {
		case EngineParamType::INT:
			ERR_FAIL_COND_V_MSG(p_value.get_type() != godot::Variant::INT, false, "Engine parameter requires an integer.");
			r_value = EngineParamValue::from_int(int64_t(p_value));
			break;
		case EngineParamType::FLOAT:
			ERR_FAIL_COND_V_MSG(p_value.get_type() != godot::Variant::FLOAT, false, "Engine parameter requires a float.");
			r_value = EngineParamValue::from_float(double(p_value));
			break;
		case EngineParamType::BOOL:
			ERR_FAIL_COND_V_MSG(p_value.get_type() != godot::Variant::BOOL, false, "Engine parameter requires a boolean.");
			r_value = EngineParamValue::from_bool(bool(p_value));
			break;
	}
	ERR_FAIL_COND_V_MSG(!std::isfinite(r_value.number()) || r_value.number() < p_descriptor.minimum || r_value.number() > p_descriptor.maximum, false, "Engine parameter value is outside its registered range.");
	return true;
}

godot::Variant encode_engine_param(EngineParamValue p_value) {
	switch (p_value.type) {
		case EngineParamType::INT: return p_value.integer;
		case EngineParamType::FLOAT: return p_value.real;
		case EngineParamType::BOOL: return p_value.boolean;
	}
	CRASH_NOW_MSG("Invalid registered scalar type.");
	std::abort();
}

godot::Variant::Type engine_param_variant_type(EngineParamType p_type) {
	switch (p_type) {
		case EngineParamType::INT: return godot::Variant::INT;
		case EngineParamType::FLOAT: return godot::Variant::FLOAT;
		case EngineParamType::BOOL: return godot::Variant::BOOL;
	}
	CRASH_NOW_MSG("Invalid registered scalar type.");
	std::abort();
}
