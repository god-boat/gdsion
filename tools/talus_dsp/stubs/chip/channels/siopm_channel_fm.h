// Prepared voices and FM schema inheritance are covered by the native suite.
// This harness exercises channel-local scalar application and DSP only.
#pragma once

#include "engine_params.h"
#include <cstdlib>

inline void store_engine_param(SiMMLVoice &, uint32_t, int, EngineParamValue) { std::abort(); }
inline EngineParamValue load_engine_param(const SiMMLVoice &, uint32_t, int) { std::abort(); }

class SiOPMChannelFM {
public:
	static const EngineParamSchema &get_prepared_engine_param_schema() { std::abort(); }
};
