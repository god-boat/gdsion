/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef RESAMPLE_UTIL_H
#define RESAMPLE_UTIL_H

#include <godot_cpp/templates/vector.hpp>
#include <cmath>

using namespace godot;

// Simple linear resampler for mono/stereo interleaved PCM in the range [-1, 1].
// Shared between SiOPMWaveSamplerData and SiOPMWaveStreamData.
// Result is written into r_dst; p_src is not modified.
inline void resample_linear(const Vector<double> &p_src, int p_channels, int p_src_rate, int p_dst_rate, Vector<double> &r_dst) {
	if (p_src_rate == p_dst_rate || p_src_rate <= 0 || p_dst_rate <= 0) {
		r_dst = p_src;
		return;
	}

	int src_frame_count = p_src.size() / p_channels;
	if (src_frame_count < 2) {
		r_dst = p_src;
		return;
	}

	double ratio = static_cast<double>(p_dst_rate) / static_cast<double>(p_src_rate);
	double inv_ratio = 1.0 / ratio;
	int dst_frame_count = static_cast<int>(std::ceil(src_frame_count * ratio));

	r_dst.resize_zeroed(dst_frame_count * p_channels);

	for (int ch = 0; ch < p_channels; ch++) {
		double src_pos = 0.0;
		for (int i = 0; i < dst_frame_count; ++i, src_pos += inv_ratio) {
			int idx = static_cast<int>(std::floor(src_pos));
			double frac = src_pos - idx;
			if (idx >= src_frame_count - 1) {
				idx = src_frame_count - 2;
				frac = 1.0;
			}
			double s0 = p_src[(idx * p_channels) + ch];
			double s1 = p_src[((idx + 1) * p_channels) + ch];
			r_dst.write[(i * p_channels) + ch] = s0 + (s1 - s0) * frac;
		}
	}
}

#endif // RESAMPLE_UTIL_H
