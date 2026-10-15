#ifndef ENGINE_PARAM_API_H
#define ENGINE_PARAM_API_H

#include "engine_params.h"
#include <godot_cpp/variant/variant.hpp>

// Main-thread GDExtension boundary. Only plain EngineParamValue crosses the queue.
// Callers supply the registered Variant type; the descriptor alone owns it.
bool decode_engine_param(const EngineParamDescriptor &p_descriptor, const godot::Variant &p_value, EngineParamValue &r_value);
godot::Variant encode_engine_param(EngineParamValue p_value);
godot::Variant::Type engine_param_variant_type(EngineParamType p_type);

#endif
