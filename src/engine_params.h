#ifndef ENGINE_PARAMS_H
#define ENGINE_PARAMS_H

#include <cstdint>
#include <type_traits>
#include <vector>

// Plain scalar data only. Neither prepared state nor the render command retains
// a Variant, object, string or method-specific argument list.
enum class EngineParamType : uint8_t { INT, FLOAT, BOOL };

struct EngineParamValue {
	EngineParamType type = EngineParamType::INT;
	union {
		int64_t integer = 0;
		double real;
		bool boolean;
	};
	static EngineParamValue from_int(int64_t p_value) { EngineParamValue v; v.integer = p_value; return v; }
	static EngineParamValue from_float(double p_value) { EngineParamValue v; v.type = EngineParamType::FLOAT; v.real = p_value; return v; }
	static EngineParamValue from_bool(bool p_value) { EngineParamValue v; v.type = EngineParamType::BOOL; v.boolean = p_value; return v; }
	double number() const {
		if (type == EngineParamType::INT) return double(integer);
		if (type == EngineParamType::FLOAT) return real;
		return boolean ? 1.0 : 0.0;
	}
};

static_assert(std::is_trivially_copyable<EngineParamValue>::value, "Realtime scalar payload must remain plain data.");

// A parameter holds instance_count independent values, selected by index. What
// an index denotes (an operator, a slot, a phase) belongs to the engine that
// applies it; the lane only carries it.
struct EngineParamDescriptor {
	uint32_t id;
	const char *name;
	EngineParamType type;
	double minimum;
	double maximum;
	EngineParamValue reset_value;
	int instance_count;
	bool realtime;

	bool has_instance(int p_index) const { return p_index >= 0 && p_index < instance_count; }
};

struct EngineParamSchema {
	int engine_kind;
	const EngineParamDescriptor *parameters;
	int count;
	const EngineParamDescriptor *find(uint32_t p_id) const;
	int state_index(uint32_t p_id, int p_index) const;

	// Registration is initialization-time only. The returned schema is immutable.
	static void register_schema(const EngineParamSchema &p_schema);
	static const EngineParamSchema *find_engine(int p_engine_kind);
};

// Engine-owned preparation storage. Allocated/reset/copied on the preparation
// thread; the render owner only reads it when applying a published voice.
class EngineParamState {
	const EngineParamSchema *_schema = nullptr;
	std::vector<EngineParamValue> _values;

public:
	const EngineParamSchema *schema() const { return _schema; }
	void reset(const EngineParamSchema *p_schema);
	void set(uint32_t p_id, int p_index, EngineParamValue p_value) { _values[_schema->state_index(p_id, p_index)] = p_value; }
	EngineParamValue get(uint32_t p_id, int p_index) const { return _values[_schema->state_index(p_id, p_index)]; }
};

#endif
