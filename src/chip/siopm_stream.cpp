/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_stream.h"

#include "chip/siopm_ref_table.h"

void SiOPMStream::resize(int p_length) {
	// Guard against invalid sizes that could trigger an internal assert in Godot
	// (CowData::resize expects a non-negative count). If the caller passes an
	// illegal value, clamp to zero instead of letting the engine abort.
	if (unlikely(p_length < 0)) {
		p_length = 0;
	} else if (unlikely(p_length > 1 << 24)) {
		// Arbitrary safety cap (~16 million samples) to avoid runaway allocations
		// in case of integer overflow elsewhere that produced a huge positive
		// number. Adjust as needed if legitimate buffers ever require more.
		p_length = 1 << 24;
	}
	buffer.resize_zeroed(p_length);
}

void SiOPMStream::clear() {
	for (int i = 0; i < buffer.size(); i++) {
		buffer.write[i] = 0;
	}
}

//unused :(
void SiOPMStream::limit() {
	for (int i = 0; i < buffer.size(); i++) {
		buffer.write[i] = CLAMP(buffer[i], -1, 1);
	}
}

void SiOPMStream::quantize(int p_bitrate) {
	double r = 1 << p_bitrate;
	double ir = 2.0 / r;
	for (int i = 0; i < buffer.size(); i++) {
		int n = buffer[i] * r; // Truncate the double-precision value before shifting.
		buffer.write[i] = (n >> 1) * ir;
	}
}

void SiOPMStream::write(SinglyLinkedList<int>::Element *p_data_start, int p_offset, int p_length, double p_volume, int p_pan) {
	if (p_offset < 0) {
		p_offset = 0; // safety – avoid negative writes
	}
	double volume = p_volume * SiOPMRefTable::get_instance()->i2n;
	int buffer_size = (p_offset + p_length) << 1;
	if (buffer_size > buffer.size()) {
		buffer_size = buffer.size(); // clamp to avoid overflow
	}

	if (channels == 2) { // stereo
		double (&pan_table)[129] = SiOPMRefTable::get_instance()->pan_table;
		double volume_left = pan_table[128 - p_pan] * volume;
		double volume_right = pan_table[p_pan] * volume;

		SinglyLinkedList<int>::Element *current = p_data_start;
		for (int i = p_offset << 1; i < buffer_size;) {
			buffer.write[i] += current->value * volume_left;
			i++;
			buffer.write[i] += current->value * volume_right;
			i++;

			current = current->next();
		}
	} else if (channels == 1) { // mono
		SinglyLinkedList<int>::Element *current = p_data_start;
		for (int i = p_offset << 1; i < buffer_size;) {
			buffer.write[i] += current->value * volume;
			i++;
			buffer.write[i] += current->value * volume;
			i++;

			current = current->next();
		}
	}
}

void SiOPMStream::write_stereo(SinglyLinkedList<int>::Element *p_left_start, SinglyLinkedList<int>::Element *p_right_start, int p_offset, int p_length, double p_volume, int p_pan) {
	if (p_offset < 0) {
		p_offset = 0;
	}
	double volume = p_volume * SiOPMRefTable::get_instance()->i2n;
	int buffer_size = (p_offset + p_length) << 1;
	if (buffer_size > buffer.size()) {
		buffer_size = buffer.size();
	}

	if (channels == 2) { // stereo
		double (&pan_table)[129] = SiOPMRefTable::get_instance()->pan_table;
		double volume_left = pan_table[128 - p_pan] * volume;
		double volume_right = pan_table[p_pan] * volume;

		SinglyLinkedList<int>::Element *current_left = p_left_start;
		SinglyLinkedList<int>::Element *current_right = p_right_start;

		for (int i = p_offset << 1; i < buffer_size;) {
			buffer.write[i] += current_left->value * volume_left;
			i++;
			buffer.write[i] += current_right->value * volume_right;
			i++;

			current_left = current_left->next();
			current_right = current_right->next();
		}
	} else if (channels == 1) { // mono
		volume *= 0.5;

		SinglyLinkedList<int>::Element *current_left = p_left_start;
		SinglyLinkedList<int>::Element *current_right = p_right_start;

		for (int i = p_offset << 1; i < buffer_size;) {
			buffer.write[i] += (current_left->value + current_right->value) * volume;
			i++;
			buffer.write[i] += (current_left->value + current_right->value) * volume;
			i++;

			current_left = current_left->next();
			current_right = current_right->next();
		}
	}
}

void SiOPMStream::write_from_vector(Vector<double> *p_data, int p_start_data, int p_start_buffer, int p_length, double p_volume, int p_pan, int p_sample_channel_count) {
	// Guard against invalid ranges to avoid CRASH_BAD_INDEX from Godot's Vector.
	if (p_start_buffer < 0) {
		p_start_buffer = 0;
	}

	// Cap the amount we can write so we never exceed the backing buffer size.
	int max_frames_in_buffer = buffer.size() >> 1; // each frame has 2 samples (LR)
	if (p_start_buffer >= max_frames_in_buffer || p_length <= 0) {
		return; // Nothing we can write safely.
	}
	if (p_start_buffer + p_length > max_frames_in_buffer) {
		p_length = max_frames_in_buffer - p_start_buffer;
	}

	double volume = p_volume;

	int channels = this->channels;
	if (channels == 2) {
		double (&pan_table)[129] = SiOPMRefTable::get_instance()->pan_table;

		if (p_sample_channel_count == 2) { // stereo data to stereo buffer
			double volume_left = pan_table[128 - p_pan] * volume;
			double volume_right = pan_table[p_pan] * volume;
			int buffer_size = (p_start_buffer + p_length) << 1;

			for (int j = p_start_data << 1, i = p_start_buffer << 1; j < (p_start_data + p_length) << 1;) {
				buffer.write[i] += (*p_data)[j] * volume_left;
				j++;
				i++;
				buffer.write[i] += (*p_data)[j] * volume_right;
				j++;
				i++;
			}
		} else { // mono data to stereo buffer
			double volume_left = pan_table[128 - p_pan] * volume * 0.707;
			double volume_right = pan_table[p_pan] * volume * 0.707;
			for (int j = p_start_data, i = p_start_buffer << 1; j < p_start_data + p_length; j++) {
				buffer.write[i] += (*p_data)[j] * volume_left;
				i++;
				buffer.write[i] += (*p_data)[j] * volume_right;
				i++;
			}
		}
	} else if (channels == 1) {
		if (p_sample_channel_count == 2) { // stereo data to mono buffer
			volume *= 0.5;
			for (int j = p_start_data << 1, i = p_start_buffer << 1; j < (p_start_data + p_length) << 1;) {
				buffer.write[i] += ((*p_data)[j] + (*p_data)[j + 1]) * volume;
				i++;
				buffer.write[i] += ((*p_data)[j] + (*p_data)[j + 1]) * volume;
				i++;
				j += 2;
			}
		} else { // mono data to mono buffer
			for (int j = p_start_data, i = p_start_buffer << 1; j < p_start_data + p_length; j++) {
				buffer.write[i] += (*p_data)[j] * volume;
				i++;
				buffer.write[i] += (*p_data)[j] * volume;
				i++;
			}
		}
	}
}
