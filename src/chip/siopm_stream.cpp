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
	double *dst = buffer.ptrw();
	for (int i = 0; i < buffer.size(); i++) {
		dst[i] = 0;
	}
}

//unused :(
void SiOPMStream::limit() {
	double *dst = buffer.ptrw();
	for (int i = 0; i < buffer.size(); i++) {
		dst[i] = CLAMP(dst[i], -1, 1);
	}
}

void SiOPMStream::quantize(int p_bitrate) {
	double r = 1 << p_bitrate;
	double ir = 2.0 / r;
	double *dst = buffer.ptrw();
	for (int i = 0; i < buffer.size(); i++) {
		int n = dst[i] * r; // Truncate the double-precision value before shifting.
		dst[i] = (n >> 1) * ir;
	}
}

void SiOPMStream::write(SinglyLinkedList<int>::Element *p_data_start, int p_offset, int p_length, double p_volume, int p_pan) {
	if (p_data_start == nullptr || p_length <= 0) {
		return;
	}
	if (p_offset < 0) {
		p_offset = 0; // safety - avoid negative writes
	}
	double volume = p_volume * SiOPMRefTable::get_instance()->i2n;
	const int start_index = p_offset << 1;
	int buffer_size = (p_offset + p_length) << 1;
	if (buffer_size > buffer.size()) {
		buffer_size = buffer.size(); // clamp to avoid overflow
	}
	if (buffer_size <= start_index) {
		return;
	}

	double *dst = buffer.ptrw();

	if (channels == 2) { // stereo
		double (&pan_table)[129] = SiOPMRefTable::get_instance()->pan_table;
		double volume_left = volume;
		double volume_right = volume;
		if (p_pan != PAN_NONE) {
			const int pan = CLAMP(p_pan, 0, 128);
			volume_left = pan_table[128 - pan] * volume;
			volume_right = pan_table[pan] * volume;
		}

		SinglyLinkedList<int>::Element *current = p_data_start;
		for (int i = start_index; i < buffer_size;) {
			const double sample = current->value;
			dst[i++] += sample * volume_left;
			dst[i++] += sample * volume_right;

			current = current->next();
		}
	} else if (channels == 1) { // mono
		SinglyLinkedList<int>::Element *current = p_data_start;
		for (int i = start_index; i < buffer_size;) {
			const double sample = current->value * volume;
			dst[i++] += sample;
			dst[i++] += sample;

			current = current->next();
		}
	}
}

void SiOPMStream::write_stereo(SinglyLinkedList<int>::Element *p_left_start, SinglyLinkedList<int>::Element *p_right_start, int p_offset, int p_length, double p_volume, int p_pan) {
	if (p_left_start == nullptr || p_right_start == nullptr || p_length <= 0) {
		return;
	}
	if (p_offset < 0) {
		p_offset = 0;
	}
	double volume = p_volume * SiOPMRefTable::get_instance()->i2n;
	const int start_index = p_offset << 1;
	int buffer_size = (p_offset + p_length) << 1;
	if (buffer_size > buffer.size()) {
		buffer_size = buffer.size();
	}
	if (buffer_size <= start_index) {
		return;
	}

	double *dst = buffer.ptrw();

	if (channels == 2) { // stereo
		double (&pan_table)[129] = SiOPMRefTable::get_instance()->pan_table;
		double volume_left = volume;
		double volume_right = volume;
		if (p_pan != PAN_NONE) {
			const int pan = CLAMP(p_pan, 0, 128);
			volume_left = pan_table[128 - pan] * volume;
			volume_right = pan_table[pan] * volume;
		}

		SinglyLinkedList<int>::Element *current_left = p_left_start;
		SinglyLinkedList<int>::Element *current_right = p_right_start;

		for (int i = start_index; i < buffer_size;) {
			dst[i++] += current_left->value * volume_left;
			dst[i++] += current_right->value * volume_right;

			current_left = current_left->next();
			current_right = current_right->next();
		}
	} else if (channels == 1) { // mono
		volume *= 0.5;

		SinglyLinkedList<int>::Element *current_left = p_left_start;
		SinglyLinkedList<int>::Element *current_right = p_right_start;

		for (int i = start_index; i < buffer_size;) {
			const double sample = (current_left->value + current_right->value) * volume;
			dst[i++] += sample;
			dst[i++] += sample;

			current_left = current_left->next();
			current_right = current_right->next();
		}
	}
}

void SiOPMStream::write_from_vector(Vector<double> *p_data, int p_start_data, int p_start_buffer, int p_length, double p_volume, int p_pan, int p_sample_channel_count) {
	// Guard against invalid ranges to avoid CRASH_BAD_INDEX from Godot's Vector.
	if (p_data == nullptr) {
		return;
	}
	if (p_start_data < 0) {
		p_start_data = 0;
	}
	if (p_start_buffer < 0) {
		p_start_buffer = 0;
	}

	// Clamp the source range so we never read past the input vector.
	const int src_channels = (p_sample_channel_count == 2) ? 2 : 1;
	const int max_frames_in_source = p_data->size() / src_channels;
	if (p_start_data >= max_frames_in_source || p_length <= 0) {
		return;
	}
	if (p_start_data + p_length > max_frames_in_source) {
		p_length = max_frames_in_source - p_start_data;
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
	const int pan = (p_pan == PAN_NONE) ? 64 : CLAMP(p_pan, 0, 128);
	const double *src = p_data->ptr();
	double *dst = buffer.ptrw();

	int channels = this->channels;
	if (channels == 2) {
		double (&pan_table)[129] = SiOPMRefTable::get_instance()->pan_table;

		if (p_sample_channel_count == 2) { // stereo data to stereo buffer
			double volume_left = pan_table[128 - pan] * volume;
			double volume_right = pan_table[pan] * volume;

			for (int j = p_start_data << 1, i = p_start_buffer << 1; j < (p_start_data + p_length) << 1;) {
				dst[i++] += src[j++] * volume_left;
				dst[i++] += src[j++] * volume_right;
			}
		} else { // mono data to stereo buffer
			double volume_left = pan_table[128 - pan] * volume * 0.707;
			double volume_right = pan_table[pan] * volume * 0.707;
			for (int j = p_start_data, i = p_start_buffer << 1; j < p_start_data + p_length; j++) {
				const double sample = src[j];
				dst[i++] += sample * volume_left;
				dst[i++] += sample * volume_right;
			}
		}
	} else if (channels == 1) {
		if (p_sample_channel_count == 2) { // stereo data to mono buffer
			volume *= 0.5;
			for (int j = p_start_data << 1, i = p_start_buffer << 1; j < (p_start_data + p_length) << 1;) {
				const double sample = (src[j] + src[j + 1]) * volume;
				dst[i++] += sample;
				dst[i++] += sample;
				j += 2;
			}
		} else { // mono data to mono buffer
			for (int j = p_start_data, i = p_start_buffer << 1; j < p_start_data + p_length; j++) {
				const double sample = src[j] * volume;
				dst[i++] += sample;
				dst[i++] += sample;
			}
		}
	}
}
