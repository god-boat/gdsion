/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SIOPM_TALUS_PARAMS_H
#define SIOPM_TALUS_PARAMS_H

// The Talus channel's grouped params, in slot order. SiONVoice::set_talus,
// the driver mailbox and MML all carry them as this positional tuple, so the
// defaults below are the engine's one copy of them.
struct TalusParams {
	enum Slot {
		STRIKE_LEVEL, // Percent.
		STRIKE_HARDNESS, // Percent: contact time 6 ms to 0.15 ms.
		STRIKE_VELOCITY, // Percent of the force that key velocity scales.
		NOISE_LEVEL, // Percent.
		NOISE_DECAY_MS,
		NOISE_COLOR_HZ,
		TONE_LEVEL, // Percent.
		TONE_PITCH_CENTS, // Relative to the fundamental.
		TONE_SHAPE, // Percent, sine to square.
		TONE_DECAY_MS,
		VARIATION, // Percent.
		MODEL, // 0 Membrane, 1 Loop.
		TUNE_SEMITONES,
		FINE_CENTS,
		KEYTRACK, // Percent.
		DECAY_MS, // T60 of the fundamental.
		DAMPING, // Percent.
		POSITION, // Percent, center to edge.
		STIFFNESS, // Percent.
		LOW_CUT_HZ,
		CUT_HZ,
		CUT_DB,
		TENSION_CENTS,
		DRIVE, // Percent.
		WIRES_LEVEL, // Percent.
		WIRES_TENSION, // Percent, loose to tight.
		WIRES_TONE_HZ,
		WIRES_DECAY_MS,
		HEAD, // Percent.
		DIRECT, // Percent.
		RELEASE_MODE, // 0 Ring, 1 Mute.
		RELEASE_MS,
		SLOT_COUNT,
	};

	int values[SLOT_COUNT] = {
		100, 60, 80, // Strike.
		30, 8, 3000, // Noise.
		0, 0, 50, 10, // Tone.
		20, // Variation.
		0, 0, 0, 100, 800, 40, 30, 0, 1, 2000, 0, 0, 0, // Resonator.
		0, 50, 5000, 120, // Wires.
		100, 20, // Output.
		0, 80, // Release.
	};
};

#endif // SIOPM_TALUS_PARAMS_H
