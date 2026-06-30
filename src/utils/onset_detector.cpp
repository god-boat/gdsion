#include "onset_detector.h"

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <cmath>
#include <algorithm>

#include "chip/wave/siopm_wave_base.h"

using namespace godot;

static bool debug_print_enabled = true;

static void debug_print(const String &p_msg) {
	if (debug_print_enabled) {
		UtilityFunctions::print(p_msg);
	}
}

namespace {

// ---------------------------------------------------------------------------
// Tunable parameters
// ---------------------------------------------------------------------------

struct OnsetProfile {
	const char *debug_prefix;
	double window_time_seconds;
	double hop_time_seconds;
	double min_interval_seconds;
	double threshold_floor_ratio;
	double threshold_k_min;
	double threshold_k_range;
	bool use_channel_power_envelope;
	bool use_strength_release;
	double strength_release_ratio;
	double strength_release_floor_ratio;
	bool use_envelope_retrigger_guard;
	double envelope_retrigger_ratio;
	double envelope_retrigger_floor_ratio;
	bool use_start_boundary_as_onset;
};

static constexpr double BPM_WINDOW_TIME_SECONDS = 0.010666666666666666;
static constexpr double BPM_HOP_TIME_SECONDS = 0.005333333333333333;
static constexpr double BPM_MIN_ONSET_INTERVAL_SECONDS = 0.05;

static constexpr double SLICE_WINDOW_TIME_SECONDS = 0.005333333333333333;
static constexpr double SLICE_HOP_TIME_SECONDS = 0.0026666666666666666;
static constexpr double SLICE_MIN_ONSET_INTERVAL_SECONDS = 0.085;

static constexpr OnsetProfile BPM_ONSET_PROFILE = {
	"BPM",
	BPM_WINDOW_TIME_SECONDS,
	BPM_HOP_TIME_SECONDS,
	BPM_MIN_ONSET_INTERVAL_SECONDS,
	0.02,
	1.0,
	3.0,
	false,
	false,
	0.0,
	0.0,
	false,
	0.0,
	0.0,
	false
};

static constexpr OnsetProfile SLICE_ONSET_PROFILE = {
	"SLICE",
	SLICE_WINDOW_TIME_SECONDS,
	SLICE_HOP_TIME_SECONDS,
	SLICE_MIN_ONSET_INTERVAL_SECONDS,
	0.008,
	0.35,
	1.75,
	true,
	true,
	0.08,
	0.01,
	true,
	0.35,
	0.03,
	true
};

// BPM working range. Tempi outside are folded by halving/doubling. Wider than a
// single octave so common slow (ambient) and fast (DnB/footwork) tempi are kept
// intact rather than silently doubled or halved.
static constexpr double BPM_RANGE_LO = 60.0;
static constexpr double BPM_RANGE_HI = 200.0;

// Onsets within this fraction of a grid spacing count toward the grid-fit score.
static constexpr double GRID_FIT_TOLERANCE = 0.15;

// Candidates within this many BPM of each other are merged into one cluster.
static constexpr double CANDIDATE_CLUSTER_BPM = 1.0;

// Cap on candidates evaluated by the (expensive) grid-fit scorer.
static constexpr int CANDIDATE_SCORE_CAP = 30;

// Strongest K onsets are tried as candidate phase origins during scoring.
static constexpr int PHASE_ORIGIN_COUNT = 8;

// All-pairs candidate generation is O(N^2). For huge onset lists, restrict to
// the strongest hits to keep work bounded.
static constexpr int ALL_PAIRS_ONSET_CAP = 120;

// Beat-count interpretations applied to each pairwise onset distance. A given
// distance might represent a half beat, one beat, four beats, etc.
static const double PAIR_BEAT_INTERPRETATIONS[] = {0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0};
static constexpr int PAIR_BEAT_INTERPRETATION_COUNT =
	sizeof(PAIR_BEAT_INTERPRETATIONS) / sizeof(double);

// Grid subdivisions tested against each candidate BPM. Each onset's best fit
// across these grids is used in scoring. Weights bias toward coarser (beat-level)
// alignments so that a BPM fitting onsets on the beat outscores one that only
// fits via fine subdivisions.
static const double GRID_SUBDIVISIONS[] = {1.0, 0.75, 0.5, 1.0 / 3.0, 0.25};
static const double GRID_SUBDIVISION_WEIGHTS[] = {1.0, 0.7, 0.8, 0.5, 0.5};
static constexpr int GRID_SUBDIVISION_COUNT =
	sizeof(GRID_SUBDIVISIONS) / sizeof(double);

// Two candidate BPMs must differ by at least this much to count as "distinct"
// when selecting the second-best for the confidence margin.
static constexpr double DISTINCT_BPM_MARGIN = 1.5;

// Fine refinement span and step around the coarse winner.
static constexpr double REFINE_SPAN_BPM = 2.0;
static constexpr double REFINE_STEP_BPM = 0.05;

// Bar-alignment scoring: rewards candidates where the total sample length is a
// clean number of bars (assumes 4/4 time). Loops exported from DAWs are almost
// always exact bar multiples, so this is a strong prior.
static constexpr double BAR_ALIGNMENT_WEIGHT = 0.35;
static constexpr double BAR_ALIGNMENT_TOLERANCE = 0.03; // max fractional-bar error to receive any bonus

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

struct OnsetAnalysis {
	Vector<int> positions;
	Vector<double> strengths;
	Vector<double> rms_envelope;
	Vector<double> onset_strength_envelope;
	int hop_size = 0;
	int window_size = 0;
	int sample_rate = 0;
	int frame_count = 0;
};

struct BpmCandidate {
	double bpm = 0.0;
	double seed_weight = 0.0; // weight at generation time (autocorr value or pair strength sum)
	double score = 0.0;       // grid-fit score, filled in by _score_candidate
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

int _resolve_stream_sample_rate(const Ref<AudioStream> &p_stream) {
	Ref<AudioStreamWAV> wav_stream = p_stream;
	ERR_FAIL_COND_V_MSG(wav_stream.is_null(), 0, "OnsetDetector: stream analysis only supports AudioStreamWAV with an explicit mix rate.");

	const int sample_rate = wav_stream->get_mix_rate();
	ERR_FAIL_COND_V_MSG(sample_rate <= 0, 0, vformat("OnsetDetector: AudioStreamWAV returned an invalid mix rate (%d).", sample_rate));

	return sample_rate;
}

// Returns offset in [-0.5, 0.5] of the parabolic peak through (y1, y2, y3).
double _parabolic_peak_offset(double y1, double y2, double y3) {
	double denom = (y1 - 2.0 * y2 + y3);
	if (std::abs(denom) < 1e-12) {
		return 0.0;
	}
	double off = 0.5 * (y1 - y3) / denom;
	if (off < -0.5) off = -0.5;
	if (off > 0.5) off = 0.5;
	return off;
}

double _fold_bpm(double p_bpm, double p_lo, double p_hi) {
	if (p_bpm <= 0.0) return 0.0;
	double v = p_bpm;
	int guard = 64;
	while (v > p_hi && guard-- > 0) v *= 0.5;
	guard = 64;
	while (v < p_lo && guard-- > 0) v *= 2.0;
	return v;
}

// Score in [0, 1] measuring how well the sample length aligns to a whole number
// of bars at the given BPM (4/4 time). Only awards a score for power-of-2 bar
// counts (1, 2, 4, 8, 16) which account for virtually all real-world loops.
double _loop_length_score(double p_bpm, int p_frame_count, int p_sample_rate) {
	if (p_bpm <= 0.0 || p_frame_count <= 0 || p_sample_rate <= 0) return 0.0;

	double duration = double(p_frame_count) / double(p_sample_rate);
	double bar_duration = 240.0 / p_bpm;
	double total_bars = duration / bar_duration;

	double nearest = std::round(total_bars);
	if (nearest < 1.0) nearest = 1.0;

	int int_bars = static_cast<int>(nearest);
	if (int_bars != 1 && int_bars != 2 && int_bars != 4 && int_bars != 8 && int_bars != 16) {
		return 0.0;
	}

	double error = std::abs(total_bars - nearest);
	if (error >= BAR_ALIGNMENT_TOLERANCE) return 0.0;

	return 1.0 - error / BAR_ALIGNMENT_TOLERANCE;
}

Dictionary _build_empty_result() {
	Dictionary d;
	d["bpm"] = 0.0;
	d["confidence"] = 0.0;
	d["score"] = 0.0;
	d["onset_count"] = 0;
	d["alternatives"] = Array();
	return d;
}

PackedInt32Array _pack_onset_positions(const Vector<int> &p_onsets) {
	PackedInt32Array result;
	result.resize(p_onsets.size());
	for (int i = 0; i < p_onsets.size(); i++) {
		result.set(i, p_onsets[i]);
	}
	return result;
}

// ---------------------------------------------------------------------------
// Onset analysis pipeline
// ---------------------------------------------------------------------------

// Compute RMS envelope, then onset strength as positive first derivative.
Vector<double> _compute_onset_strength_envelope(
	const Vector<double> &p_wave_data,
	int p_channel_count,
	int p_sample_rate,
	const OnsetProfile &p_profile,
	Vector<double> *r_rms_envelope,
	int *r_window_size,
	int *r_hop_size,
	int *r_frame_count
) {
	*r_window_size = 0;
	*r_hop_size = 0;
	*r_frame_count = 0;
	if (r_rms_envelope != nullptr) {
		r_rms_envelope->clear();
	}

	if (p_wave_data.is_empty() || p_channel_count < 1 || p_sample_rate <= 0) {
		return Vector<double>();
	}

	int window_size = MAX(static_cast<int>(p_sample_rate * p_profile.window_time_seconds), 64);
	int hop_size = MAX(static_cast<int>(p_sample_rate * p_profile.hop_time_seconds), 32);
	int frame_count = p_wave_data.size() / p_channel_count;

	*r_window_size = window_size;
	*r_hop_size = hop_size;
	*r_frame_count = frame_count;

	if (frame_count < window_size * 2) {
		return Vector<double>();
	}
	int num_windows = (frame_count - window_size) / hop_size + 1;
	if (num_windows < 2) {
		return Vector<double>();
	}

	Vector<double> envelope;
	envelope.resize_zeroed(num_windows);
	const double *data = p_wave_data.ptr();

	for (int w = 0; w < num_windows; w++) {
		int start = w * hop_size;
		double sum_sq = 0.0;
		for (int f = 0; f < window_size; f++) {
			int fi = start + f;
			if (fi >= frame_count) break;
			if (p_profile.use_channel_power_envelope) {
				double frame_sum_sq = 0.0;
				for (int ch = 0; ch < p_channel_count; ch++) {
					double sample = data[fi * p_channel_count + ch];
					frame_sum_sq += sample * sample;
				}
				sum_sq += frame_sum_sq / p_channel_count;
			} else {
				double sample = 0.0;
				for (int ch = 0; ch < p_channel_count; ch++) {
					sample += data[fi * p_channel_count + ch];
				}
				sample /= p_channel_count;
				sum_sq += sample * sample;
			}
		}
		envelope.write[w] = std::sqrt(sum_sq / window_size);
	}

	double env_min = envelope[0], env_max = envelope[0], env_sum = 0.0;
	for (int w = 0; w < num_windows; w++) {
		if (envelope[w] < env_min) env_min = envelope[w];
		if (envelope[w] > env_max) env_max = envelope[w];
		env_sum += envelope[w];
	}
	debug_print(vformat("[%s] _compute_onset_strength_envelope: %d windows, RMS envelope min=%.6f max=%.6f mean=%.6f", String(p_profile.debug_prefix), num_windows, env_min, env_max, env_sum / num_windows));
	if (r_rms_envelope != nullptr) {
		*r_rms_envelope = envelope;
	}

	Vector<double> onset_strength;
	onset_strength.resize_zeroed(num_windows);
	for (int w = 1; w < num_windows; w++) {
		double diff = envelope[w] - envelope[w - 1];
		onset_strength.write[w] = MAX(0.0, diff);
	}
	return onset_strength;
}

// Peak-pick the onset-strength envelope. Uses parabolic interpolation on each
// local maximum for sub-hop sample precision.
void _peak_pick_onsets(
	const Vector<double> &p_onset_strength,
	const Vector<double> &p_rms_envelope,
	int p_hop_size,
	int p_min_onset_interval_samples,
	double p_threshold,
	const OnsetProfile &p_profile,
	Vector<int> *r_positions,
	Vector<double> *r_strengths
) {
	int num_windows = p_onset_strength.size();
	if (num_windows < 3) return;

	bool use_release = p_profile.use_strength_release;
	bool use_envelope_guard = p_profile.use_envelope_retrigger_guard && p_rms_envelope.size() == num_windows;
	double max_release_strength = 0.0;
	if (use_release) {
		for (int w = 0; w < num_windows; w++) {
			if (p_onset_strength[w] > max_release_strength) {
				max_release_strength = p_onset_strength[w];
			}
		}
	}
	double max_guard_envelope = 0.0;
	if (use_envelope_guard) {
		for (int w = 0; w < num_windows; w++) {
			if (p_rms_envelope[w] > max_guard_envelope) {
				max_guard_envelope = p_rms_envelope[w];
			}
		}
	}

	int last_pos = p_profile.use_start_boundary_as_onset ? 0 : -p_min_onset_interval_samples;
	bool start_boundary_pending = p_profile.use_start_boundary_as_onset;
	bool strength_released = true;
	double release_threshold = 0.0;
	bool have_guard_origin = false;
	double last_guard_envelope = 0.0;
	double min_guard_envelope_since_last = 0.0;
	for (int w = 1; w < num_windows - 1; w++) {
		if (use_release && !strength_released && p_onset_strength[w] <= release_threshold) {
			strength_released = true;
		}
		if (use_envelope_guard && have_guard_origin && p_rms_envelope[w] < min_guard_envelope_since_last) {
			min_guard_envelope_since_last = p_rms_envelope[w];
		}

		double s = p_onset_strength[w];
		if (s < p_threshold) continue;
		double sp = p_onset_strength[w - 1];
		double sn = p_onset_strength[w + 1];
		if (s < sp || s < sn) continue;

		double off = _parabolic_peak_offset(sp, s, sn);
		int sample_pos = static_cast<int>(std::round((static_cast<double>(w) + off) * p_hop_size));
		int samples_since_last = sample_pos - last_pos;
		if (use_release) {
			if (start_boundary_pending && samples_since_last < p_min_onset_interval_samples) {
				last_pos = sample_pos;
				strength_released = false;
				release_threshold = MAX(s * p_profile.strength_release_ratio, max_release_strength * p_profile.strength_release_floor_ratio);
				if (use_envelope_guard) {
					last_guard_envelope = p_rms_envelope[w];
					min_guard_envelope_since_last = last_guard_envelope;
					have_guard_origin = true;
				}
				start_boundary_pending = false;
				continue;
			}
			if (!strength_released || samples_since_last < p_min_onset_interval_samples) continue;
		} else if (samples_since_last < p_min_onset_interval_samples) {
			continue;
		}
		if (use_envelope_guard && have_guard_origin) {
			double guard_threshold = MAX(last_guard_envelope * p_profile.envelope_retrigger_ratio, max_guard_envelope * p_profile.envelope_retrigger_floor_ratio);
			if (min_guard_envelope_since_last > guard_threshold) continue;
		}

		r_positions->push_back(sample_pos);
		r_strengths->push_back(s);
		last_pos = sample_pos;
		start_boundary_pending = false;
		if (use_envelope_guard) {
			last_guard_envelope = p_rms_envelope[w];
			min_guard_envelope_since_last = last_guard_envelope;
			have_guard_origin = true;
		}
		if (use_release) {
			strength_released = false;
			release_threshold = MAX(s * p_profile.strength_release_ratio, max_release_strength * p_profile.strength_release_floor_ratio);
		}
	}
}

OnsetAnalysis _analyze_onsets(
	const Vector<double> &p_wave_data,
	int p_channel_count,
	int p_sensitivity,
	int p_sample_rate,
	const OnsetProfile &p_profile
) {
	OnsetAnalysis result;
	String debug_prefix = String(p_profile.debug_prefix);
	if (p_wave_data.is_empty() || p_channel_count < 1 || p_sample_rate <= 0) {
		return result;
	}

	int window_size = 0, hop_size = 0, frame_count = 0;
	Vector<double> rms_envelope;
	Vector<double> onset_strength = _compute_onset_strength_envelope(
		p_wave_data, p_channel_count, p_sample_rate, p_profile,
		&rms_envelope,
		&window_size, &hop_size, &frame_count
	);
	if (onset_strength.is_empty()) return result;

	result.rms_envelope = rms_envelope;
	result.onset_strength_envelope = onset_strength;
	result.window_size = window_size;
	result.hop_size = hop_size;
	result.sample_rate = p_sample_rate;
	result.frame_count = frame_count;

	int min_onset_interval = MAX(static_cast<int>(p_sample_rate * p_profile.min_interval_seconds), hop_size * 2);
	int clamped_sens = CLAMP(p_sensitivity, 1, 100);
	double sens = (clamped_sens - 1) / 99.0;

	int n = onset_strength.size();

	double max_strength = 0.0;
	double nz_sum = 0.0;
	int nz_count = 0;
	for (int i = 0; i < n; i++) {
		double v = onset_strength[i];
		if (v > max_strength) max_strength = v;
		if (v > 1e-10) {
			nz_sum += v;
			nz_count++;
		}
	}

	if (max_strength <= 0.0) {
		debug_print(vformat("[%s] _analyze_onsets: sample_rate=%d channels=%d sensitivity=%d", debug_prefix, p_sample_rate, p_channel_count, p_sensitivity));
		debug_print(vformat("[%s]   frame_count=%d window_size=%d hop_size=%d envelope_frames=%d", debug_prefix, frame_count, window_size, hop_size, n));
		debug_print(vformat("[%s]   no positive onset strength", debug_prefix));
		return result;
	}

	double threshold = max_strength * 0.05;
	double nz_mean = 0.0;
	double nz_stddev = 0.0;
	if (nz_count > 1) {
		nz_mean = nz_sum / nz_count;
		double var_sum = 0.0;
		for (int i = 0; i < n; i++) {
			double v = onset_strength[i];
			if (v > 1e-10) {
				double d = v - nz_mean;
				var_sum += d * d;
			}
		}
		nz_stddev = std::sqrt(var_sum / (nz_count - 1));

		double k = p_profile.threshold_k_min + (1.0 - sens) * p_profile.threshold_k_range;
		threshold = nz_mean + k * nz_stddev;
		threshold = MAX(threshold, max_strength * p_profile.threshold_floor_ratio);
	}

	double k = p_profile.threshold_k_min + (1.0 - sens) * p_profile.threshold_k_range;
	debug_print(vformat("[%s] _analyze_onsets: sample_rate=%d channels=%d sensitivity=%d", debug_prefix, p_sample_rate, p_channel_count, p_sensitivity));
	debug_print(vformat("[%s]   frame_count=%d window_size=%d hop_size=%d envelope_frames=%d", debug_prefix, frame_count, window_size, hop_size, n));
	debug_print(vformat("[%s]   duration=%.2fs", debug_prefix, double(frame_count) / double(p_sample_rate)));
	debug_print(vformat("[%s]   nonzero_count=%d/%d mean=%.6f stddev=%.6f max=%.6f", debug_prefix, nz_count, n, nz_mean, nz_stddev, max_strength));
	debug_print(vformat("[%s]   k=%.2f threshold=%.6f (%.1f%% of max)", debug_prefix, k, threshold, 100.0 * threshold / max_strength));
	if (p_profile.use_strength_release) {
		debug_print(vformat("[%s]   release: strength_reset_ratio=%.2f reset_floor=%.1f%% of max_strength", debug_prefix, p_profile.strength_release_ratio, p_profile.strength_release_floor_ratio * 100.0));
	}
	if (p_profile.use_envelope_retrigger_guard) {
		debug_print(vformat("[%s]   retrigger_guard: rms_dip_ratio=%.2f rms_floor=%.1f%% of max_rms", debug_prefix, p_profile.envelope_retrigger_ratio, p_profile.envelope_retrigger_floor_ratio * 100.0));
	}

	_peak_pick_onsets(onset_strength, rms_envelope, hop_size, min_onset_interval, threshold, p_profile,
		&result.positions, &result.strengths);

	debug_print(vformat("[%s]   detected %d onsets (min_interval=%d samples = %.3fs)", debug_prefix, result.positions.size(), min_onset_interval, double(min_onset_interval) / double(p_sample_rate)));
	if (result.positions.size() > 0) {
		double min_str = result.strengths[0], max_str = result.strengths[0];
		for (int i = 1; i < result.strengths.size(); i++) {
			if (result.strengths[i] < min_str) min_str = result.strengths[i];
			if (result.strengths[i] > max_str) max_str = result.strengths[i];
		}
		debug_print(vformat("[%s]   onset strengths: min=%.6f max=%.6f", debug_prefix, min_str, max_str));
		int show = MIN(result.positions.size(), 10);
		for (int i = 0; i < show; i++) {
			debug_print(vformat("[%s]   onset[%d] pos=%d (%.3fs) strength=%.6f", debug_prefix, i, result.positions[i], double(result.positions[i]) / double(p_sample_rate), result.strengths[i]));
		}
		if (result.positions.size() > 10) {
			debug_print(vformat("[%s]   ... (%d more onsets)", debug_prefix, result.positions.size() - 10));
		}
	}

	return result;
}

// ---------------------------------------------------------------------------
// Candidate generation
// ---------------------------------------------------------------------------

Vector<BpmCandidate> _autocorr_candidates(
	const Vector<double> &p_onset_strength,
	int p_sample_rate,
	int p_hop_size,
	double p_bpm_lo,
	double p_bpm_hi
) {
	Vector<BpmCandidate> out;
	int n = p_onset_strength.size();
	if (n < 8 || p_sample_rate <= 0 || p_hop_size <= 0) return out;

	double env_rate = double(p_sample_rate) / double(p_hop_size);
	int lag_min = MAX(2, int(std::floor(env_rate * 60.0 / p_bpm_hi)));
	int lag_max = MIN(n / 2, int(std::ceil(env_rate * 60.0 / p_bpm_lo)));

	debug_print(vformat("[BPM] _autocorr_candidates: envelope_size=%d env_rate=%.2f Hz lag_range=[%d, %d]", n, env_rate, lag_min, lag_max));

	if (lag_max <= lag_min + 2) {
		debug_print("[BPM]   lag range too narrow, skipping autocorrelation");
		return out;
	}

	Vector<double> ac;
	ac.resize_zeroed(lag_max + 2);
	const double *env = p_onset_strength.ptr();

	for (int lag = lag_min; lag <= lag_max + 1 && lag < n; lag++) {
		double s = 0.0;
		int limit = n - lag;
		for (int t = 0; t < limit; t++) {
			s += env[t] * env[t + lag];
		}
		ac.write[lag] = s;
	}

	for (int lag = lag_min + 1; lag <= lag_max; lag++) {
		double v = ac[lag];
		if (v <= 0.0) continue;
		if (v < ac[lag - 1] || v < ac[lag + 1]) continue;
		double off = _parabolic_peak_offset(ac[lag - 1], v, ac[lag + 1]);
		double refined_lag = double(lag) + off;
		double bpm = 60.0 * env_rate / refined_lag;
		double folded = _fold_bpm(bpm, p_bpm_lo, p_bpm_hi);
		if (folded <= 0.0) continue;
		BpmCandidate c;
		c.bpm = folded;
		c.seed_weight = v;
		out.push_back(c);
	}

	debug_print(vformat("[BPM]   autocorr produced %d candidates", out.size()));
	int show = MIN(out.size(), 8);
	for (int i = 0; i < show; i++) {
		debug_print(vformat("[BPM]   autocorr[%d] bpm=%.2f weight=%.6f", i, out[i].bpm, out[i].seed_weight));
	}

	return out;
}

Vector<BpmCandidate> _all_pairs_candidates(
	const Vector<int> &p_onsets,
	const Vector<double> &p_strengths,
	int p_sample_rate,
	double p_bpm_lo,
	double p_bpm_hi
) {
	Vector<BpmCandidate> out;
	int n = p_onsets.size();
	if (n < 3 || p_sample_rate <= 0) return out;

	Vector<int> pos = p_onsets;
	Vector<double> str = p_strengths;

	debug_print(vformat("[BPM] _all_pairs_candidates: %d onsets, sample_rate=%d, range=[%.1f, %.1f]", n, p_sample_rate, p_bpm_lo, p_bpm_hi));

	if (n > ALL_PAIRS_ONSET_CAP) {
		debug_print(vformat("[BPM]   capping onset list from %d to %d strongest", n, ALL_PAIRS_ONSET_CAP));
		struct Pair { int pos; double strength; };
		Vector<Pair> pairs;
		pairs.resize(n);
		Pair *pp = pairs.ptrw();
		for (int i = 0; i < n; i++) {
			pp[i].pos = p_onsets[i];
			pp[i].strength = p_strengths[i];
		}
		std::sort(pp, pp + n, [](const Pair &a, const Pair &b) {
			return a.strength > b.strength;
		});
		int keep = ALL_PAIRS_ONSET_CAP;
		std::sort(pp, pp + keep, [](const Pair &a, const Pair &b) {
			return a.pos < b.pos;
		});
		pos.clear();
		str.clear();
		pos.resize(keep);
		str.resize(keep);
		for (int i = 0; i < keep; i++) {
			pos.write[i] = pp[i].pos;
			str.write[i] = pp[i].strength;
		}
		n = keep;
	}

	for (int i = 0; i < n; i++) {
		for (int j = i + 1; j < n; j++) {
			int delta = pos[j] - pos[i];
			if (delta <= 0) continue;
			double weight = str[i] + str[j];
			for (int k = 0; k < PAIR_BEAT_INTERPRETATION_COUNT; k++) {
				double beats = PAIR_BEAT_INTERPRETATIONS[k];
				double bpm = 60.0 * beats * p_sample_rate / double(delta);
				double folded = _fold_bpm(bpm, p_bpm_lo, p_bpm_hi);
				if (folded < p_bpm_lo * 0.9 || folded > p_bpm_hi * 1.1) continue;
				BpmCandidate c;
				c.bpm = folded;
				c.seed_weight = weight;
				out.push_back(c);
			}
		}
	}

	debug_print(vformat("[BPM]   all_pairs produced %d raw candidates", out.size()));

	return out;
}

// Merge nearby candidates (within p_tolerance_bpm) into single weight-averaged
// representatives, then sort the result by descending seed_weight.
Vector<BpmCandidate> _cluster_and_rank(Vector<BpmCandidate> p_candidates, double p_tolerance_bpm) {
	Vector<BpmCandidate> clusters;
	if (p_candidates.is_empty()) return clusters;

	BpmCandidate *cp = p_candidates.ptrw();
	std::sort(cp, cp + p_candidates.size(), [](const BpmCandidate &a, const BpmCandidate &b) {
		return a.bpm < b.bpm;
	});

	for (int i = 0; i < p_candidates.size(); i++) {
		if (!clusters.is_empty() &&
			p_candidates[i].bpm - clusters[clusters.size() - 1].bpm < p_tolerance_bpm) {
			BpmCandidate &last = clusters.write[clusters.size() - 1];
			double total = last.seed_weight + p_candidates[i].seed_weight;
			if (total > 0.0) {
				last.bpm = (last.bpm * last.seed_weight + p_candidates[i].bpm * p_candidates[i].seed_weight) / total;
				last.seed_weight = total;
			}
		} else {
			clusters.push_back(p_candidates[i]);
		}
	}

	BpmCandidate *cl = clusters.ptrw();
	std::sort(cl, cl + clusters.size(), [](const BpmCandidate &a, const BpmCandidate &b) {
		return a.seed_weight > b.seed_weight;
	});
	return clusters;
}

// ---------------------------------------------------------------------------
// Candidate scoring (the actual tempo-hypothesis test)
// ---------------------------------------------------------------------------

// Score in [0, 1] measuring how well the onsets land on a rhythmic grid at p_bpm.
// We try the top-K strongest onsets as the phase origin, test multiple grid
// subdivisions, and keep the best.
double _score_candidate(
	double p_bpm,
	const Vector<int> &p_onsets,
	const Vector<double> &p_strengths,
	int p_sample_rate
) {
	int n = p_onsets.size();
	if (n < 3 || p_bpm <= 0.0 || p_sample_rate <= 0) return 0.0;

	double beat_samples = 60.0 * p_sample_rate / p_bpm;
	double grids[GRID_SUBDIVISION_COUNT];
	for (int g = 0; g < GRID_SUBDIVISION_COUNT; g++) {
		grids[g] = beat_samples * GRID_SUBDIVISIONS[g];
	}

	struct StrengthIdx { int idx; double strength; };
	Vector<StrengthIdx> sidx;
	sidx.resize(n);
	StrengthIdx *sp = sidx.ptrw();
	for (int i = 0; i < n; i++) {
		sp[i].idx = i;
		sp[i].strength = p_strengths[i];
	}
	std::sort(sp, sp + n, [](const StrengthIdx &a, const StrengthIdx &b) {
		return a.strength > b.strength;
	});
	int num_origins = MIN(PHASE_ORIGIN_COUNT, n);

	double total_strength = 0.0;
	for (int i = 0; i < n; i++) total_strength += p_strengths[i];
	if (total_strength <= 0.0) return 0.0;

	const double tol = GRID_FIT_TOLERANCE;
	double best_score = 0.0;
	for (int oi = 0; oi < num_origins; oi++) {
		double origin = static_cast<double>(p_onsets[sp[oi].idx]);
		double sum = 0.0;
		for (int i = 0; i < n; i++) {
			double op = static_cast<double>(p_onsets[i]);
			double best_fit = 0.0;
			for (int g = 0; g < GRID_SUBDIVISION_COUNT; g++) {
				double gs = grids[g];
				if (gs <= 0.0) continue;
				double x = (op - origin) / gs;
				double rx = std::round(x);
				double err = std::abs(x - rx);
				if (err < tol) {
					double fit = (1.0 - err / tol) * GRID_SUBDIVISION_WEIGHTS[g];
					if (fit > best_fit) best_fit = fit;
				}
			}
			sum += best_fit * p_strengths[i];
		}
		double s = sum / total_strength;
		if (s > best_score) best_score = s;
	}
	return best_score;
}

// Fine search around the coarse winner.
double _refine_bpm(
	double p_coarse_bpm,
	const Vector<int> &p_onsets,
	const Vector<double> &p_strengths,
	int p_sample_rate,
	double p_span_bpm,
	double p_step_bpm,
	double *r_best_score
) {
	double best_bpm = p_coarse_bpm;
	double best_score = _score_candidate(p_coarse_bpm, p_onsets, p_strengths, p_sample_rate);

	for (double offset = -p_span_bpm; offset <= p_span_bpm; offset += p_step_bpm) {
		if (std::abs(offset) < 1e-9) continue;
		double cand = p_coarse_bpm + offset;
		if (cand <= 0.0) continue;
		double s = _score_candidate(cand, p_onsets, p_strengths, p_sample_rate);
		if (s > best_score) {
			best_score = s;
			best_bpm = cand;
		}
	}
	if (r_best_score) *r_best_score = best_score;
	return best_bpm;
}

// ---------------------------------------------------------------------------
// Detailed BPM pipeline
// ---------------------------------------------------------------------------

Dictionary _estimate_bpm_detailed_internal(
	const Vector<double> &p_wave_data,
	int p_channel_count,
	int p_sample_rate
) {
	debug_print("=======================================================");
	debug_print("[BPM] estimate_bpm_detailed_internal START");
	debug_print(vformat("[BPM]   input: %d samples, %d channels, sample_rate=%d", p_wave_data.size(), p_channel_count, p_sample_rate));

	if (p_wave_data.is_empty() || p_channel_count < 1 || p_sample_rate <= 0) {
		debug_print("[BPM]   EARLY EXIT: invalid input parameters");
		return _build_empty_result();
	}

	int frame_count = p_wave_data.size() / p_channel_count;
	debug_print(vformat("[BPM]   total duration: %.3fs (%d frames)", double(frame_count) / double(p_sample_rate), frame_count));

	OnsetAnalysis a = _analyze_onsets(p_wave_data, p_channel_count, 50, p_sample_rate, BPM_ONSET_PROFILE);
	if (a.positions.size() < 3) {
		debug_print(vformat("[BPM]   EARLY EXIT: too few onsets (%d < 3)", a.positions.size()));
		return _build_empty_result();
	}

	int bpm_min_spacing = static_cast<int>(p_sample_rate * 60.0 / (BPM_RANGE_HI * 2.0));
	int pre_consolidation_count = a.positions.size();
	{
		Vector<int> consolidated_pos;
		Vector<double> consolidated_str;
		consolidated_pos.push_back(a.positions[0]);
		consolidated_str.push_back(a.strengths[0]);
		for (int i = 1; i < a.positions.size(); i++) {
			int last_idx = consolidated_pos.size() - 1;
			if (a.positions[i] - consolidated_pos[last_idx] < bpm_min_spacing) {
				if (a.strengths[i] > consolidated_str[last_idx]) {
					consolidated_pos.write[last_idx] = a.positions[i];
					consolidated_str.write[last_idx] = a.strengths[i];
				}
			} else {
				consolidated_pos.push_back(a.positions[i]);
				consolidated_str.push_back(a.strengths[i]);
			}
		}
		a.positions = consolidated_pos;
		a.strengths = consolidated_str;
	}
	debug_print(vformat("[BPM] Onset consolidation: %d -> %d (min_spacing=%d samples = %.3fs)",
		pre_consolidation_count, a.positions.size(), bpm_min_spacing, double(bpm_min_spacing) / double(p_sample_rate)));

	if (a.positions.size() < 3) {
		debug_print(vformat("[BPM]   EARLY EXIT: too few onsets after consolidation (%d < 3)", a.positions.size()));
		return _build_empty_result();
	}

	Vector<BpmCandidate> autocorr = _autocorr_candidates(
		a.onset_strength_envelope, a.sample_rate, a.hop_size, BPM_RANGE_LO, BPM_RANGE_HI
	);
	Vector<BpmCandidate> all_pairs = _all_pairs_candidates(
		a.positions, a.strengths, a.sample_rate, BPM_RANGE_LO, BPM_RANGE_HI
	);

	Vector<BpmCandidate> combined;
	for (int i = 0; i < autocorr.size(); i++) combined.push_back(autocorr[i]);
	for (int i = 0; i < all_pairs.size(); i++) combined.push_back(all_pairs[i]);

	debug_print(vformat("[BPM] Combined candidates: %d (autocorr=%d + all_pairs=%d)", combined.size(), autocorr.size(), all_pairs.size()));

	if (combined.is_empty()) {
		debug_print("[BPM]   EARLY EXIT: no candidates generated");
		return _build_empty_result();
	}

	Vector<BpmCandidate> ranked = _cluster_and_rank(combined, CANDIDATE_CLUSTER_BPM);

	debug_print(vformat("[BPM] After clustering: %d clusters (from %d raw)", ranked.size(), combined.size()));
	int show_ranked = MIN(ranked.size(), 10);
	for (int i = 0; i < show_ranked; i++) {
		debug_print(vformat("[BPM]   cluster[%d] bpm=%.2f seed_weight=%.4f", i, ranked[i].bpm, ranked[i].seed_weight));
	}

	if (ranked.is_empty()) {
		debug_print("[BPM]   EARLY EXIT: no clusters after merging");
		return _build_empty_result();
	}

	int score_cap = MIN(static_cast<int>(ranked.size()), CANDIDATE_SCORE_CAP);
	Vector<BpmCandidate> scored;
	scored.resize(score_cap);

	debug_print(vformat("[BPM] Scoring top %d candidates against %d onsets...", score_cap, a.positions.size()));

	double max_seed_weight = ranked[0].seed_weight;
	for (int i = 1; i < score_cap; i++) {
		if (ranked[i].seed_weight > max_seed_weight) {
			max_seed_weight = ranked[i].seed_weight;
		}
	}

	for (int i = 0; i < score_cap; i++) {
		BpmCandidate c = ranked[i];
		double grid_score = _score_candidate(c.bpm, a.positions, a.strengths, a.sample_rate);
		double seed_factor = (max_seed_weight > 0.0) ? c.seed_weight / max_seed_weight : 0.0;
		double bar_align = _loop_length_score(c.bpm, frame_count, p_sample_rate);
		c.score = grid_score * (1.0 + 0.2 * seed_factor + BAR_ALIGNMENT_WEIGHT * bar_align);
		scored.write[i] = c;
	}

	BpmCandidate *scp = scored.ptrw();
	std::sort(scp, scp + scored.size(), [](const BpmCandidate &a, const BpmCandidate &b) {
		return a.score > b.score;
	});

	debug_print(vformat("[BPM] Top scored candidates (sorted by combined score, seed_boost max_seed=%.4f):", max_seed_weight));
	int show_scored = MIN(scored.size(), 10);
	for (int i = 0; i < show_scored; i++) {
		double seed_factor = (max_seed_weight > 0.0) ? scored[i].seed_weight / max_seed_weight : 0.0;
		double bar_align = _loop_length_score(scored[i].bpm, frame_count, p_sample_rate);
		double total_bars = (double(frame_count) / double(p_sample_rate)) * scored[i].bpm / 240.0;
		debug_print(vformat("[BPM]   scored[%d] bpm=%.2f combined=%.4f seed=%.3f bar_align=%.3f (%.3f bars)", i, scored[i].bpm, scored[i].score, seed_factor, bar_align, total_bars));
	}

	if (scored[0].score <= 0.0) {
		debug_print("[BPM]   EARLY EXIT: best score is 0");
		return _build_empty_result();
	}

	BpmCandidate best = scored[0];
	double refined_grid_score = 0.0;
	double refined_bpm = _refine_bpm(
		best.bpm, a.positions, a.strengths, a.sample_rate,
		REFINE_SPAN_BPM, REFINE_STEP_BPM, &refined_grid_score
	);

	double best_seed_factor = (max_seed_weight > 0.0) ? best.seed_weight / max_seed_weight : 0.0;
	double refined_bar_align = _loop_length_score(refined_bpm, frame_count, p_sample_rate);
	double refined_combined = refined_grid_score * (1.0 + 0.2 * best_seed_factor + BAR_ALIGNMENT_WEIGHT * refined_bar_align);

	debug_print(vformat("[BPM] Refinement: coarse=%.2f (score=%.4f) -> refined=%.2f (score=%.4f, bar_align=%.3f)", best.bpm, best.score, refined_bpm, refined_combined, refined_bar_align));

	if (refined_combined > best.score) {
		best.bpm = refined_bpm;
		best.score = refined_combined;
	}

	double second_score = 0.0;
	double second_bpm = 0.0;
	for (int i = 1; i < scored.size(); i++) {
		if (std::abs(scored[i].bpm - best.bpm) < DISTINCT_BPM_MARGIN) continue;
		second_score = scored[i].score;
		second_bpm = scored[i].bpm;
		break;
	}
	double confidence = 0.0;
	if (best.score > 0.0) {
		confidence = 1.0 - (second_score / best.score);
		if (confidence < 0.0) confidence = 0.0;
		if (confidence > 1.0) confidence = 1.0;
	}

	double bpm_rounded = std::round(best.bpm * 10.0) / 10.0;
	double bpm_frac = bpm_rounded - std::floor(bpm_rounded);
	if (bpm_frac <= 0.2 || bpm_frac >= 0.8) {
		bpm_rounded = std::round(bpm_rounded);
	}

	debug_print(vformat("[BPM] RESULT: bpm=%.1f confidence=%.3f score=%.4f", bpm_rounded, confidence, best.score));
	debug_print(vformat("[BPM]   second-best: bpm=%.2f score=%.4f (margin=%.1f BPM required)", second_bpm, second_score, DISTINCT_BPM_MARGIN));
	debug_print("=======================================================");

	Dictionary result;
	result["bpm"] = bpm_rounded;
	result["confidence"] = confidence;
	result["score"] = best.score;
	result["onset_count"] = a.positions.size();

	Array alternatives;
	int alt_added = 0;
	for (int i = 1; i < scored.size() && alt_added < 3; i++) {
		if (std::abs(scored[i].bpm - best.bpm) < DISTINCT_BPM_MARGIN) continue;
		Dictionary alt;
		alt["bpm"] = std::round(scored[i].bpm * 10.0) / 10.0;
		alt["score"] = scored[i].score;
		alt["kind"] = "nearby";
		alternatives.push_back(alt);
		alt_added++;
	}
	{
		Dictionary half;
		half["bpm"] = std::round(bpm_rounded * 0.5 * 10.0) / 10.0;
		half["score"] = best.score;
		half["kind"] = "half";
		alternatives.push_back(half);
	}
	{
		Dictionary dbl;
		dbl["bpm"] = std::round(bpm_rounded * 2.0 * 10.0) / 10.0;
		dbl["score"] = best.score;
		dbl["kind"] = "double";
		alternatives.push_back(dbl);
	}
	result["alternatives"] = alternatives;
	return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void OnsetDetector::_bind_methods() {
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("detect_onsets", "wave_data", "channel_count", "sensitivity", "sample_rate"),
		&OnsetDetector::detect_onsets, DEFVAL(2), DEFVAL(50), DEFVAL(0));
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("detect_onsets_from_stream", "stream", "sensitivity"),
		&OnsetDetector::detect_onsets_from_stream, DEFVAL(50));
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("detect_slice_onsets", "wave_data", "channel_count", "sensitivity", "sample_rate"),
		&OnsetDetector::detect_slice_onsets, DEFVAL(2), DEFVAL(75), DEFVAL(0));
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("detect_slice_onsets_from_stream", "stream", "sensitivity"),
		&OnsetDetector::detect_slice_onsets_from_stream, DEFVAL(75));
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("estimate_bpm", "wave_data", "channel_count", "sample_rate"),
		&OnsetDetector::estimate_bpm, DEFVAL(2), DEFVAL(0));
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("estimate_bpm_from_stream", "stream"),
		&OnsetDetector::estimate_bpm_from_stream);
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("estimate_bpm_detailed", "wave_data", "channel_count", "sample_rate"),
		&OnsetDetector::estimate_bpm_detailed, DEFVAL(2), DEFVAL(0));
	ClassDB::bind_static_method("OnsetDetector", D_METHOD("estimate_bpm_detailed_from_stream", "stream"),
		&OnsetDetector::estimate_bpm_detailed_from_stream);
}

PackedInt32Array OnsetDetector::detect_onsets(
	const PackedFloat32Array &p_wave_data,
	int p_channel_count,
	int p_sensitivity,
	int p_sample_rate
) {
	ERR_FAIL_COND_V_MSG(p_sample_rate <= 0, PackedInt32Array(), vformat("OnsetDetector: detect_onsets() requires an explicit positive sample rate, got %d.", p_sample_rate));

	Vector<double> wave_data;
	wave_data.resize(p_wave_data.size());
	for (int i = 0; i < p_wave_data.size(); i++) {
		wave_data.write[i] = static_cast<double>(p_wave_data[i]);
	}

	Vector<int> onsets = detect_onsets_internal(wave_data, p_channel_count, p_sensitivity, p_sample_rate);

	return _pack_onset_positions(onsets);
}

PackedInt32Array OnsetDetector::detect_onsets_from_stream(
	const Ref<AudioStream> &p_stream,
	int p_sensitivity
) {
	PackedInt32Array result;
	if (p_stream.is_null()) return result;

	const int sample_rate = _resolve_stream_sample_rate(p_stream);
	if (sample_rate <= 0) return result;

	int channel_count = 2;
	Vector<double> wave_data = SiOPMWaveBase::extract_wave_data(p_stream, &channel_count);
	if (wave_data.is_empty() || channel_count < 1) return result;

	Vector<int> onsets = detect_onsets_internal(wave_data, channel_count, p_sensitivity, sample_rate);

	return _pack_onset_positions(onsets);
}

PackedInt32Array OnsetDetector::detect_slice_onsets(
	const PackedFloat32Array &p_wave_data,
	int p_channel_count,
	int p_sensitivity,
	int p_sample_rate
) {
	ERR_FAIL_COND_V_MSG(p_sample_rate <= 0, PackedInt32Array(), vformat("OnsetDetector: detect_slice_onsets() requires an explicit positive sample rate, got %d.", p_sample_rate));

	Vector<double> wave_data;
	wave_data.resize(p_wave_data.size());
	for (int i = 0; i < p_wave_data.size(); i++) {
		wave_data.write[i] = static_cast<double>(p_wave_data[i]);
	}

	Vector<int> onsets = detect_slice_onsets_internal(wave_data, p_channel_count, p_sensitivity, p_sample_rate);
	return _pack_onset_positions(onsets);
}

PackedInt32Array OnsetDetector::detect_slice_onsets_from_stream(
	const Ref<AudioStream> &p_stream,
	int p_sensitivity
) {
	PackedInt32Array result;
	if (p_stream.is_null()) return result;

	const int sample_rate = _resolve_stream_sample_rate(p_stream);
	if (sample_rate <= 0) return result;

	int channel_count = 2;
	Vector<double> wave_data = SiOPMWaveBase::extract_wave_data(p_stream, &channel_count);
	if (wave_data.is_empty() || channel_count < 1) return result;

	Vector<int> onsets = detect_slice_onsets_internal(wave_data, channel_count, p_sensitivity, sample_rate);
	return _pack_onset_positions(onsets);
}

Vector<int> OnsetDetector::detect_onsets_internal(
	const Vector<double> &p_wave_data,
	int p_channel_count,
	int p_sensitivity,
	int p_sample_rate
) {
	Vector<int> onsets;
	if (p_wave_data.is_empty() || p_channel_count < 1) return onsets;
	ERR_FAIL_COND_V_MSG(p_sample_rate <= 0, onsets, vformat("OnsetDetector: detect_onsets_internal() requires an explicit positive sample rate, got %d.", p_sample_rate));

	OnsetAnalysis a = _analyze_onsets(p_wave_data, p_channel_count, p_sensitivity, p_sample_rate, BPM_ONSET_PROFILE);
	return a.positions;
}

Vector<int> OnsetDetector::detect_slice_onsets_internal(
	const Vector<double> &p_wave_data,
	int p_channel_count,
	int p_sensitivity,
	int p_sample_rate
) {
	Vector<int> onsets;
	if (p_wave_data.is_empty() || p_channel_count < 1) return onsets;
	ERR_FAIL_COND_V_MSG(p_sample_rate <= 0, onsets, vformat("OnsetDetector: detect_slice_onsets_internal() requires an explicit positive sample rate, got %d.", p_sample_rate));

	OnsetAnalysis a = _analyze_onsets(p_wave_data, p_channel_count, p_sensitivity, p_sample_rate, SLICE_ONSET_PROFILE);
	return a.positions;
}

double OnsetDetector::estimate_bpm(
	const PackedFloat32Array &p_wave_data,
	int p_channel_count,
	int p_sample_rate
) {
	ERR_FAIL_COND_V_MSG(p_sample_rate <= 0, 0.0, vformat("OnsetDetector: estimate_bpm() requires an explicit positive sample rate, got %d.", p_sample_rate));
	Dictionary d = estimate_bpm_detailed(p_wave_data, p_channel_count, p_sample_rate);
	return double(d["bpm"]);
}

double OnsetDetector::estimate_bpm_from_stream(const Ref<AudioStream> &p_stream) {
	Dictionary d = estimate_bpm_detailed_from_stream(p_stream);
	return double(d["bpm"]);
}

Dictionary OnsetDetector::estimate_bpm_detailed(
	const PackedFloat32Array &p_wave_data,
	int p_channel_count,
	int p_sample_rate
) {
	ERR_FAIL_COND_V_MSG(p_sample_rate <= 0, _build_empty_result(), vformat("OnsetDetector: estimate_bpm_detailed() requires an explicit positive sample rate, got %d.", p_sample_rate));

	Vector<double> wave_data;
	wave_data.resize(p_wave_data.size());
	for (int i = 0; i < p_wave_data.size(); i++) {
		wave_data.write[i] = static_cast<double>(p_wave_data[i]);
	}
	return _estimate_bpm_detailed_internal(wave_data, p_channel_count, p_sample_rate);
}

Dictionary OnsetDetector::estimate_bpm_detailed_from_stream(const Ref<AudioStream> &p_stream) {
	if (p_stream.is_null()) return _build_empty_result();

	const int sample_rate = _resolve_stream_sample_rate(p_stream);
	if (sample_rate <= 0) return _build_empty_result();

	int channel_count = 2;
	Vector<double> wave_data = SiOPMWaveBase::extract_wave_data(p_stream, &channel_count);
	if (wave_data.is_empty() || channel_count < 1) return _build_empty_result();

	return _estimate_bpm_detailed_internal(wave_data, channel_count, sample_rate);
}
