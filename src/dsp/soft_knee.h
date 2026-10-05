/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_SOFT_KNEE_H
#define SION_DSP_SOFT_KNEE_H

#include <godot_cpp/core/defs.hpp>

namespace sion::dsp {

// Excess past a threshold in dB, softened by a quadratic knee. The caller
// supplies the direction and amount of gain applied to this excess.
_FORCE_INLINE_ double soft_knee_excess_db(double p_excess_db, double p_knee_db) {
	if (p_knee_db <= 0.0) {
		return p_excess_db > 0.0 ? p_excess_db : 0.0;
	}

	double half_knee = p_knee_db * 0.5;
	if (p_excess_db <= -half_knee) {
		return 0.0;
	}
	if (p_excess_db >= half_knee) {
		return p_excess_db;
	}

	double y = p_excess_db + half_knee;
	return y * y / (2.0 * p_knee_db);
}

} // namespace sion::dsp

#endif // SION_DSP_SOFT_KNEE_H
