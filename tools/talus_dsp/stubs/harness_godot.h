// Just enough of godot-cpp for the real channel sources to compile standalone.
// ClassDB::bind_method records each bound process function, and Callable calls
// it back, so a channel's _process_function works as it does in the engine.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/core/math_defs.hpp>

namespace godot {

class Object {
public:
	virtual ~Object() {}
};

class String {
	std::string _text;

public:
	String() {}
	String(const char *p_text) :
			_text(p_text) {}
	String(const std::string &p_text) :
			_text(p_text) {}
	String &operator+=(const String &p_other) {
		_text += p_other._text;
		return *this;
	}
	String operator+(const String &p_other) const { return String(_text + p_other._text); }
	friend String operator+(const char *p_left, const String &p_right) { return String(std::string(p_left) + p_right._text); }
	const std::string &text() const { return _text; }
};

inline String itos(long long p_value) { return String(std::to_string(p_value)); }
inline String rtos(double p_value) { return String(std::to_string(p_value)); }

template <class T>
class Ref {
	T *_pointer = nullptr;

public:
	Ref() {}
	Ref(T *p_pointer) :
			_pointer(p_pointer) {}
	T *operator->() const { return _pointer; }
};

template <class T>
class Vector {
	std::vector<T> _data;

public:
	struct Write {
		std::vector<T> *data;
		T &operator[](int p_index) { return (*data)[p_index]; }
	} write;

	Vector() :
			write{ &_data } {}
	Vector(const Vector &p_other) :
			_data(p_other._data), write{ &_data } {}
	Vector &operator=(const Vector &p_other) {
		_data = p_other._data;
		return *this;
	}
	void resize(int p_size) { _data.resize(p_size); }
	int size() const { return (int)_data.size(); }
	const T &operator[](int p_index) const { return _data[p_index]; }
};

struct MethodDefinition {
	const char *name;
};

template <typename... Args>
MethodDefinition D_METHOD(const char *p_name, Args...) {
	return { p_name };
}

inline std::map<std::string, std::function<void(Object *, int)>> &harness_bound_methods() {
	static std::map<std::string, std::function<void(Object *, int)>> methods;
	return methods;
}

struct ClassDB {
	template <class T>
	static void bind_method(MethodDefinition p_definition, void (T::*p_method)(int)) {
		harness_bound_methods()[p_definition.name] = [p_method](Object *p_object, int p_length) {
			(static_cast<T *>(p_object)->*p_method)(p_length);
		};
	}
};

class Callable {
	Object *_object = nullptr;
	std::string _method;

public:
	Callable() {}
	Callable(Object *p_object, const char *p_method) :
			_object(p_object), _method(p_method) {}
	void call(int p_length) const { harness_bound_methods().at(_method)(_object, p_length); }
};

} // namespace godot

#define GDCLASS(m_class, m_inherits)                             \
public:                                                          \
	static void initialize_class() { m_class::_bind_methods(); } \
                                                                 \
private:
