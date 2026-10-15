#include "engine_params.h"

#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <godot_cpp/core/error_macros.hpp>
#include "sion_enums.h"
#include "sequencer/simml_voice.h"

static std::array<const EngineParamSchema *, SiONModuleType::MODULE_MAX> schemas = {};
static std::array<EngineParamSchema, SiONModuleType::MODULE_MAX> registered_schemas = {};

void store_engine_param(SiMMLVoice &p_voice, uint32_t p_id, int p_index, EngineParamValue p_value) {
	p_voice.engine_params.set(p_id, p_index, p_value);
}

EngineParamValue load_engine_param(const SiMMLVoice &p_voice, uint32_t p_id, int p_index) {
	return p_voice.engine_params.get(p_id, p_index);
}

int EngineParamSchema::parameter_count() const {
	return count + (parent != nullptr ? parent->parameter_count() : 0);
}

const EngineParamDescriptor &EngineParamSchema::parameter_at(int p_position) const {
	return p_position < count ? parameters[p_position] : parent->parameter_at(p_position - count);
}

const EngineParamDescriptor *EngineParamSchema::find(uint32_t p_id) const {
	for (int i = 0; i < count; i++) {
		if (parameters[i].id == p_id) return &parameters[i];
	}
	return parent != nullptr ? parent->find(p_id) : nullptr;
}

int EngineParamSchema::state_index(uint32_t p_id, int p_index) const {
	int offset = 0;
	for (const EngineParamDescriptor *d : local_parameters) {
		if (d->id == p_id) return offset + p_index;
		offset += d->instance_count;
	}
	CRASH_NOW_MSG("Unregistered engine parameter in prepared state.");
	std::abort();
}

void EngineParamSchema::register_schema(const EngineParamSchema &p_schema) {
	ERR_FAIL_COND_MSG(p_schema.engine_kind < 0 || p_schema.engine_kind >= int(schemas.size()), "Unknown engine kind in parameter registration.");
	ERR_FAIL_COND_MSG(schemas[p_schema.engine_kind] != nullptr, "Duplicate engine parameter registration.");
	ERR_FAIL_COND_MSG(p_schema.parameter_count() <= 0, "An engine parameter schema must declare parameters.");
	std::vector<const EngineParamDescriptor *> local_parameters;
	for (int i = 0; i < p_schema.parameter_count(); i++) {
		const EngineParamDescriptor &d = p_schema.parameter_at(i);
		ERR_FAIL_COND_MSG(d.type > EngineParamType::BOOL, "Invalid engine parameter scalar type.");
		ERR_FAIL_COND_MSG(d.id == 0 || std::strlen(d.name) == 0, "Engine parameters require stable numeric and string identities.");
		ERR_FAIL_COND_MSG(d.reset_value.type != d.type || !std::isfinite(d.minimum) || !std::isfinite(d.maximum) || d.minimum > d.maximum || !std::isfinite(d.reset_value.number()) || d.reset_value.number() < d.minimum || d.reset_value.number() > d.maximum, "Invalid engine parameter type, range or reset value.");
		ERR_FAIL_COND_MSG(d.instance_count <= 0, "An engine parameter must register at least one instance.");
		ERR_FAIL_NULL_MSG(d.set_prepared, "An engine parameter requires prepared application.");
		ERR_FAIL_NULL_MSG(d.get_prepared, "An engine parameter requires prepared readback.");
		for (int j = 0; j < i; j++) {
			const EngineParamDescriptor &previous = p_schema.parameter_at(j);
			ERR_FAIL_COND_MSG(d.id == previous.id || std::strcmp(d.name, previous.name) == 0, "Duplicate engine parameter identity.");
		}
		if (d.uses_local_state()) local_parameters.push_back(&d);
	}
	registered_schemas[p_schema.engine_kind] = p_schema;
	registered_schemas[p_schema.engine_kind].local_parameters = std::move(local_parameters);
	schemas[p_schema.engine_kind] = &registered_schemas[p_schema.engine_kind];
}

const EngineParamSchema *EngineParamSchema::find_engine(int p_engine_kind) {
	if (p_engine_kind < 0 || p_engine_kind >= int(schemas.size())) return nullptr;
	return schemas[p_engine_kind];
}

void EngineParamState::reset(const EngineParamSchema *p_schema) {
	_schema = p_schema;
	_values.clear();
	// Reference-table bootstrap constructs voices before schema registration.
	if (_schema == nullptr) return;
	for (const EngineParamDescriptor *d : _schema->local_parameters) {
		_values.insert(_values.end(), d->instance_count, d->reset_value);
	}
}
