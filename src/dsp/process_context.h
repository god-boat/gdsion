/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DSP_PROCESS_CONTEXT_H
#define SION_DSP_PROCESS_CONTEXT_H

namespace sion::dsp {

// What every block processor knows about the block it is rendering.
// SiOPMSoundChip owns it and refreshes it at the start of each block.
struct ProcessContext {
	double sample_rate = 48000.0;
	int block_length = 0;
	double bpm = 120.0;
};

} // namespace sion::dsp

#endif // SION_DSP_PROCESS_CONTEXT_H
