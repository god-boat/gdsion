/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_BASE_H
#define SI_EFFECT_BASE_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <cmath>
#include "chip/siopm_ref_table.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace godot;

// Base class for all effects. Doesn't implement any behavior by default.
// Extending classes must be used instead.
class SiEffectBase : public RefCounted {
	GDCLASS(SiEffectBase, RefCounted)

	bool _is_free = true;
	double _sampling_rate = 48000.0;

protected:
	static void _bind_methods() {}

	_FORCE_INLINE_ void _refresh_sampling_rate_cache() {
		_sampling_rate = 48000.0;

		SiOPMRefTable *ref_table = SiOPMRefTable::get_instance();
		if (ref_table && ref_table->sampling_rate > 0) {
			_sampling_rate = ref_table->sampling_rate;
		}
	}

	// Helper for set_by_mml implementations.
	_FORCE_INLINE_ double _get_mml_arg(Vector<double> p_args, int p_index, double p_default) const {
		if (p_index < 0 || p_index >= p_args.size()) {
			return p_default;
		}

		const double value = p_args[p_index];
		return Math::is_nan(value) ? p_default : value;
	}

	// Helper for constant-power crossfade (equal-power mixing).
	// Maintains consistent volume across the wet/dry range using trigonometric functions.
	_FORCE_INLINE_ void _calculate_constant_power_gains(double p_wet, double &r_dry_gain, double &r_wet_gain) const {
		r_dry_gain = cos(p_wet * M_PI * 0.5);
		r_wet_gain = sin(p_wet * M_PI * 0.5);
	}

	_FORCE_INLINE_ double _get_sampling_rate() const { return _sampling_rate; }
	_FORCE_INLINE_ double _get_samples_per_ms() const { return _sampling_rate / 1000.0; }
	_FORCE_INLINE_ double _get_angular_frequency(double p_frequency) const { return (2.0 * M_PI * p_frequency) / _sampling_rate; }

public:
	bool is_free() const { return _is_free; }
	void set_free(bool p_free) { _is_free = p_free; }
	void refresh_sampling_rate() { _refresh_sampling_rate_cache(); }

	// Returns the requested channel count.
	virtual int prepare_process() { return 1; }

	// Process the effect to the stream buffer.
	// When called as mono, same data is expected in buffer[i*2] and buffer[i*2+1]. I.e. the buffer
	// is always in stereo. The order in the buffer is the same as wave format ([L0,R0,L1,R1,L2,R2 ... ]).
	// Start index and length must be adjusted internally to account for the stereo nature of the buffer.
	// Returns the output channel count.
	virtual int process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) { return p_channels; }

	virtual void set_by_mml(Vector<double> p_args) {}
	virtual void reset() {}

	SiEffectBase() { _refresh_sampling_rate_cache(); }
};

#endif // SI_EFFECT_BASE_H
