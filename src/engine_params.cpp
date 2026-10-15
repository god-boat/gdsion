#include "engine_params.h"

#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <godot_cpp/core/error_macros.hpp>
#include "sion_enums.h"

static std::array<const EngineParamSchema *, SiONModuleType::MODULE_MAX> schemas = {};

const EngineParamDescriptor *EngineParamSchema::find(uint32_t p_id) const {
	for (int i = 0; i < count; i++) {
		if (parameters[i].id == p_id) return &parameters[i];
	}
	return nullptr;
}

int EngineParamSchema::state_index(uint32_t p_id, int p_index) const {
	int offset = 0;
	for (int i = 0; i < count; i++) {
		if (parameters[i].id == p_id) return offset + p_index;
		offset += parameters[i].instance_count;
	}
	CRASH_NOW_MSG("Unregistered engine parameter in prepared state.");
	std::abort();
}

void EngineParamSchema::register_schema(const EngineParamSchema &p_schema) {
	ERR_FAIL_COND_MSG(p_schema.engine_kind < 0 || p_schema.engine_kind >= int(schemas.size()), "Unknown engine kind in parameter registration.");
	ERR_FAIL_COND_MSG(schemas[p_schema.engine_kind] != nullptr, "Duplicate engine parameter registration.");
	ERR_FAIL_COND_MSG(p_schema.count <= 0, "An engine parameter schema must declare parameters.");
	for (int i = 0; i < p_schema.count; i++) {
		const EngineParamDescriptor &d = p_schema.parameters[i];
		ERR_FAIL_COND_MSG(d.type > EngineParamType::BOOL, "Invalid engine parameter scalar type.");
		ERR_FAIL_COND_MSG(d.id == 0 || std::strlen(d.name) == 0, "Engine parameters require stable numeric and string identities.");
		ERR_FAIL_COND_MSG(d.reset_value.type != d.type || !std::isfinite(d.minimum) || !std::isfinite(d.maximum) || d.minimum > d.maximum || !std::isfinite(d.reset_value.number()) || d.reset_value.number() < d.minimum || d.reset_value.number() > d.maximum, "Invalid engine parameter type, range or reset value.");
		ERR_FAIL_COND_MSG(d.instance_count <= 0, "An engine parameter must register at least one instance.");
		for (int j = 0; j < i; j++) {
			ERR_FAIL_COND_MSG(d.id == p_schema.parameters[j].id || std::strcmp(d.name, p_schema.parameters[j].name) == 0, "Duplicate engine parameter identity.");
		}
	}
	schemas[p_schema.engine_kind] = &p_schema;
}

const EngineParamSchema *EngineParamSchema::find_engine(int p_engine_kind) {
	if (p_engine_kind < 0 || p_engine_kind >= int(schemas.size())) return nullptr;
	return schemas[p_engine_kind];
}

void EngineParamState::reset(const EngineParamSchema *p_schema) {
	_schema = p_schema;
	_values.clear();
	// Unmigrated engines have no scalar schema yet.
	if (_schema == nullptr) return;
	for (int i = 0; i < _schema->count; i++) {
		const EngineParamDescriptor &d = _schema->parameters[i];
		_values.insert(_values.end(), d.instance_count, d.reset_value);
	}
}
