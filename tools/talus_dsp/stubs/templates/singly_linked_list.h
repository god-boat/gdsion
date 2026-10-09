// The pipe list, reduced to the cursor API the channels use. The harness builds
// one chain per buffer call.
#pragma once

#include <vector>

template <class T>
class SinglyLinkedList {
public:
	class Element {
		friend class SinglyLinkedList<T>;
		Element *_next_ptr = nullptr;

	public:
		T value = 0;
		Element *next() const { return _next_ptr; }
	};

private:
	std::vector<Element> _storage;
	Element *_current = nullptr;

public:
	Element *get() const { return _current; }
	void set(Element *p_element) { _current = p_element; }

	// A chain of p_length zeros plus one spare, which the last next() lands on.
	void harness_reset(int p_length) {
		_storage.assign(p_length + 1, Element());
		for (int i = 0; i < p_length; i++) {
			_storage[i]._next_ptr = &_storage[i + 1];
		}
		_storage[p_length]._next_ptr = &_storage[p_length];
		_current = _storage.data();
	}

	const T &harness_value(int p_index) const { return _storage[p_index].value; }
};
