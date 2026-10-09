#pragma once

struct SiOPMRefTable {
	double sampling_rate = 48000.0;

	static SiOPMRefTable *get_instance() {
		static SiOPMRefTable table;
		return &table;
	}
};
