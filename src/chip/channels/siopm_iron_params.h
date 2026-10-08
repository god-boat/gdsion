/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SIOPM_IRON_PARAMS_H
#define SIOPM_IRON_PARAMS_H

// The Iron channel's grouped params, in slot order. SiONVoice::set_iron,
// the driver mailbox and MML all carry them as this positional tuple, so the
// defaults below are the engine's one copy of them.
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
		SLOT_COUNT,
	};

	int values[SLOT_COUNT] = { 0, 60, 20, 0, 0, 4000, 40, 30, 0, 70, 400, 0, 0, 0, 0, 30, 35, 4 };
};

#endif // SIOPM_IRON_PARAMS_H
