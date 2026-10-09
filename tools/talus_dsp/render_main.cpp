// Drives the real Talus and Iron channels, compiled against the stubs, from a
// script on stdin. One command per line:
//
//   out <path>               Binary output: per job, an int32 count then the samples.
//   rate <hz>                Sample rate for the next `channel`.
//   channel talus|iron       Selects and initializes a channel; starts a job.
//   params <ints...>         The grouped param tuple.
//   pitch <int>              set_pitch, 64 units per semitone.
//   expression <int>         offset_volume; 128 is full velocity.
//   on | off                 note_on / note_off.
//   render <n> [blocks...]   Renders n samples, split into the given blocks, then the rest.
//   end                      Writes the job.
//   until_idle <n> <block>   Renders blocks until the channel idles, at most n samples,
//                            and prints "IDLE <samples rendered>" (-1 if it never idled).
//   time <label> <n> <block> Renders n samples in blocks and prints "TIME <label> <ns per sample>".
//
// Each channel type is constructed once, so every job of a type has the same
// random seed (the first channel of its type gets seed 0).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <xmmintrin.h>

#ifndef HARNESS_NO_TALUS
#include "chip/channels/siopm_channel_talus.h"
#endif
#ifdef HARNESS_IRON
#include "chip/channels/siopm_channel_iron.h"
#endif

struct Pipes {
	SinglyLinkedList<int> in;
	SinglyLinkedList<int> base;
	SinglyLinkedList<int> out;
};

static void render(SiOPMChannelBase *p_channel, Pipes &r_pipes, int p_length, std::vector<int32_t> &r_output) {
	p_channel->harness_buffer(p_length);
	for (int i = 0; i < p_length; i++) {
		r_output.push_back(r_pipes.out.harness_value(i));
	}
}

int main() {
	// Flush denormals to zero, as SiONDriver::render_interleaved arms it.
	_mm_setcsr(_mm_getcsr() | 0x8040);

	Pipes pipes;
#ifndef HARNESS_NO_TALUS
	SiOPMChannelTalus::initialize_class();
	SiOPMChannelTalus talus;
	talus.harness_attach(&pipes.in, &pipes.base, &pipes.out);
#endif
#ifdef HARNESS_IRON
	SiOPMChannelIron::initialize_class();
	SiOPMChannelIron iron;
	iron.harness_attach(&pipes.in, &pipes.base, &pipes.out);
#endif

	SiOPMChannelBase *channel = nullptr;
	std::string kind;
	std::ofstream output;
	std::vector<int32_t> rendered;
	std::string line;
	while (std::getline(std::cin, line)) {
		std::istringstream words(line);
		std::string command;
		if (!(words >> command)) {
			continue;
		}

		if (command == "out") {
			std::string path;
			words >> path;
			output.open(path, std::ios::binary);
		} else if (command == "rate") {
			words >> SiOPMRefTable::get_instance()->sampling_rate;
		} else if (command == "channel") {
			words >> kind;
#ifndef HARNESS_NO_TALUS
			if (kind == "talus") {
				channel = &talus;
			}
#endif
#ifdef HARNESS_IRON
			if (kind == "iron") {
				channel = &iron;
			}
#endif
			if (!channel) {
				std::cerr << "unknown channel " << kind << "\n";
				return 1;
			}
			channel->initialize(nullptr, 0);
			rendered.clear();
		} else if (command == "params") {
			std::vector<int> values;
			int value;
			while (words >> value) {
				values.push_back(value);
			}
#ifndef HARNESS_NO_TALUS
			if (kind == "talus") {
				TalusParams params;
				for (int i = 0; i < (int)values.size() && i < TalusParams::SLOT_COUNT; i++) {
					params.values[i] = values[i];
				}
				talus.set_talus_params(params);
			}
#endif
#ifdef HARNESS_IRON
			if (kind == "iron") {
				IronParams params;
				for (int i = 0; i < (int)values.size() && i < IronParams::SLOT_COUNT; i++) {
					params.values[i] = values[i];
				}
				iron.set_iron_params(params);
			}
#endif
		} else if (command == "pitch") {
			int pitch;
			words >> pitch;
			channel->set_pitch(pitch);
		} else if (command == "expression") {
			int expression;
			words >> expression;
			channel->offset_volume(expression, 0);
		} else if (command == "on") {
			channel->note_on();
		} else if (command == "off") {
			channel->note_off();
		} else if (command == "render") {
			int remaining;
			words >> remaining;
			int block;
			while (remaining > 0 && words >> block) {
				block = block < remaining ? block : remaining;
				render(channel, pipes, block, rendered);
				remaining -= block;
			}
			if (remaining > 0) {
				render(channel, pipes, remaining, rendered);
			}
		} else if (command == "until_idle") {
			int limit;
			int block;
			words >> limit >> block;
			int done = 0;
			while (done < limit && !channel->is_idling()) {
				render(channel, pipes, block, rendered);
				done += block;
			}
			std::cout << "IDLE " << (channel->is_idling() ? done : -1) << "\n";
		} else if (command == "end") {
			const int32_t count = (int32_t)rendered.size();
			output.write(reinterpret_cast<const char *>(&count), sizeof(count));
			output.write(reinterpret_cast<const char *>(rendered.data()), count * sizeof(int32_t));
			rendered.clear();
		} else if (command == "time") {
			std::string label;
			int samples;
			int block;
			words >> label >> samples >> block;
			const auto start = std::chrono::steady_clock::now();
			for (int done = 0; done < samples; done += block) {
				channel->harness_buffer(block);
			}
			const auto elapsed = std::chrono::steady_clock::now() - start;
			const double nanoseconds = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
			std::cout << "TIME " << label << " " << nanoseconds / samples << "\n";
		} else {
			std::cerr << "unknown command " << command << "\n";
			return 1;
		}
	}
	output.close();
	return 0;
}
