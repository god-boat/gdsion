/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SIOPM_IRON_PARAMS_H
#define SIOPM_IRON_PARAMS_H

// Iron channel raw inputs and template content. Scalar transport carries IDs.
struct IronParams {
	enum Slot {
		VOICING, // 0 single, 1 power (root + fifth), 2 power + octave, 3 octaves.
		PICK_ATTACK, // Percent.
		PICK_POSITION, // Percent, from the bridge toward mid-string.
		PALM_MUTE, // Percent.
		MUTE_VELOCITY, // Percent of full mute a zero-velocity pick adds.
		SUSTAIN_MS,
		RELEASE_MS,
		STIFFNESS, // Percent.
		PICKUP_POSITION, // Percent, from the bridge toward the neck.
		GAIN, // Percent.
		TIGHT_HZ,
		BASS_DB,
		MID_DB,
		TREBLE_DB,
		CABINET, // 0 modern 4x12, 1 vintage 4x12, 2 open-back 2x12, 3 direct.
		MIC, // Percent, from on-axis toward off-axis.
		HUMANIZE, // Percent.
		TIMING_MS,
		PICK_SCRAPE, // Percent: how long and how hard the pick drags over the windings before it lets go.
		PICK_BEND_CENTS, // How sharp a full-velocity pick stretches the string.
		FRET_NOISE, // Percent: fret buzz and finger squeaks from the fretting hand.
		SLOT_COUNT,
	};

	int values[SLOT_COUNT] = { 0, 60, 20, 0, 0, 4000, 40, 30, 0, 70, 400, 0, 0, 0, 0, 30, 35, 4, 50, 20, 40 };
};

#endif // SIOPM_IRON_PARAMS_H
