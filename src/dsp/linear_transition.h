/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_LINEAR_TRANSITION_H
#define SION_DSP_LINEAR_TRANSITION_H

namespace sion::dsp {

// A sample-domain linear transition into a potentially moving target.
// begin() holds the last emitted value as the start while process() advances
// the mix toward each new target. The first target after reset() is adopted
// directly. begin() takes at least two samples so the transition has both
// endpoints.
class LinearTransition {
	bool _initialized = false;
	int _sample_count = 0;
	int _remaining = 0;
	double _start = 0.0;
	double _value = 0.0;

public:
	bool is_initialized() const { return _initialized; }

	void begin(int p_sample_count) {
		_start = _value;
		_sample_count = p_sample_count;
		_remaining = p_sample_count;
	}

	inline double process(double p_target) {
		if (!_initialized) {
			_initialized = true;
			_value = p_target;
			return _value;
		}
		if (_remaining == 0) {
			_value = p_target;
			return _value;
		}

		const double mix = (double)(_sample_count - _remaining) / (_sample_count - 1);
		_value = _start + (p_target - _start) * mix;
		_remaining--;
		return _value;
	}

	void reset() {
		_initialized = false;
		_sample_count = 0;
		_remaining = 0;
		_start = 0.0;
		_value = 0.0;
	}
};

} // namespace sion::dsp

#endif // SION_DSP_LINEAR_TRANSITION_H
