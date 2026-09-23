#ifndef SI_EFFECT_MB_COMPRESSOR_H
#define SI_EFFECT_MB_COMPRESSOR_H

#include "dsp/linkwitz_riley.h"
#include "effector/si_effect_base.h"

#include <godot_cpp/templates/vector.hpp>
#include <atomic>
#include <cmath>

using namespace godot;

// Three-band upward/downward compressor in the OTT lineage. Every band runs a
// downward compressor above its "Above" threshold and an upward compressor
// below its "Below" threshold at the same time, both driven by one shared
// stereo-linked envelope whose attack and release belong to the band.
class SiEffectMultibandCompressor : public SiEffectBase {
	GDCLASS(SiEffectMultibandCompressor, SiEffectBase)

	static constexpr double EPSILON = 1e-12;

	// Metering ballistics.
	static constexpr double RMS_METER_MS = 25.0;

	// Per-band base envelope times, scaled by the normalized attack/release
	// controls. Both sections of a band run off these: the band owns the time
	// constants, not the upward/downward axis.
	static constexpr double LOW_ATTACK_MS = 2.8;
	static constexpr double LOW_RELEASE_MS = 40.0;
	static constexpr double MID_ATTACK_MS = 1.4;
	static constexpr double MID_RELEASE_MS = 28.0;
	static constexpr double HIGH_ATTACK_MS = 0.7;
	static constexpr double HIGH_RELEASE_MS = 15.0;

	// Normalized attack/release map exponentially onto the base times:
	//   ms = base_ms * exp(norm * TIME_EXP_SPAN - TIME_EXP_BIAS)
	// so 0.5 is the base time, 0.55 is roughly 1.5x it, and 1.0 is 55x it.
	static constexpr double TIME_EXP_SPAN = 8.0;
	static constexpr double TIME_EXP_BIAS = 4.0;
	static constexpr double MIN_ENVELOPE_MS = 0.05;

	static constexpr double MIN_GAIN = -30.0;
	static constexpr double MAX_GAIN = 30.0;
	static constexpr double MIN_THRESHOLD = -100.0;
	static constexpr double MAX_THRESHOLD = 12.0;
	static constexpr double MAX_KNEE = 24.0;

	// Upward compression of a near-silent band would otherwise run away.
	static constexpr double MAX_UPWARD_GAIN_DB = 30.0;
	static constexpr double MIN_TOTAL_GAIN_DB = -80.0;

	static constexpr double MIN_FREQUENCY = 20.0;
	static constexpr double MAX_FREQUENCY = 20000.0;
	// Keeps the mid band from collapsing to zero width when both splits are
	// dragged together, and puts split inversion out of reach.
	static constexpr double MIN_CROSSOVER_RATIO = 1.05;

	// SiONDriver refuses any buffer length above 8192 frames, so sizing every
	// working buffer for that up front is what makes the no-allocation-on-the-
	// audio-thread guarantee true rather than merely usual.
	static constexpr int MAX_BLOCK_FRAMES = 8192;

	// Shipped defaults. Named so the constructor, set_by_mml() and the
	// ClassDB bindings cannot drift apart from each other or from
	// objects/effect_specs/fx_mb_compressor_specs.tres.
	static constexpr double DEFAULT_LOW_UPPER_THRESHOLD = -22.0;
	static constexpr double DEFAULT_MID_UPPER_THRESHOLD = -22.0;
	static constexpr double DEFAULT_HIGH_UPPER_THRESHOLD = -25.0;
	static constexpr double DEFAULT_LOW_LOWER_THRESHOLD = -38.0;
	static constexpr double DEFAULT_MID_LOWER_THRESHOLD = -38.0;
	static constexpr double DEFAULT_HIGH_LOWER_THRESHOLD = -40.0;
	static constexpr double DEFAULT_LOW_UPPER_AMOUNT = 0.80;
	static constexpr double DEFAULT_MID_UPPER_AMOUNT = 0.88;
	static constexpr double DEFAULT_HIGH_UPPER_AMOUNT = 0.90;
	static constexpr double DEFAULT_LOW_LOWER_AMOUNT = 0.55;
	static constexpr double DEFAULT_MID_LOWER_AMOUNT = 0.65;
	static constexpr double DEFAULT_HIGH_LOWER_AMOUNT = 0.70;
	static constexpr double DEFAULT_LOW_OUTPUT_GAIN = 2.0;
	static constexpr double DEFAULT_MID_OUTPUT_GAIN = 3.0;
	static constexpr double DEFAULT_HIGH_OUTPUT_GAIN = 5.0;
	static constexpr double DEFAULT_ATTACK = 0.55;
	static constexpr double DEFAULT_RELEASE = 0.60;
	static constexpr double DEFAULT_MIX = 1.0;
	static constexpr double DEFAULT_LM_FREQUENCY = 88.0;
	static constexpr double DEFAULT_MH_FREQUENCY = 2500.0;
	static constexpr double DEFAULT_KNEE = 6.0;

	enum BandMode {
		BAND_MULTIBAND = 0,
		BAND_LOW = 1,
		BAND_HIGH = 2,
		BAND_FULL_RANGE = 3,
	};

	enum BandIndex {
		BAND_LOW_INDEX = 0,
		BAND_MID_INDEX = 1,
		BAND_HIGH_INDEX = 2,
		BAND_COUNT = 3,
	};

	// Controls shared by every band, loaded once at the top of a block.
	struct BlockSettings {
		double attack = DEFAULT_ATTACK;
		double release = DEFAULT_RELEASE;
		double knee_db = DEFAULT_KNEE;
		// Dry/wet is de-zippered across the block: the value in force at frame
		// i is mix_start + (i + 1) * mix_delta.
		double mix_start = DEFAULT_MIX;
		double mix_delta = 0.0;
	};

	// One band worth of settings, snapshotted out of the atomic store at the
	// top of a block. Plain doubles: only the audio thread ever reads these.
	struct BandSettings {
		double upper_threshold_db = DEFAULT_MID_UPPER_THRESHOLD;
		double lower_threshold_db = DEFAULT_MID_LOWER_THRESHOLD;
		double upper_amount = DEFAULT_MID_UPPER_AMOUNT;
		double lower_amount = DEFAULT_MID_LOWER_AMOUNT;
		double output_gain_db = DEFAULT_MID_OUTPUT_GAIN;
		double knee_db = DEFAULT_KNEE;
		double attack = DEFAULT_ATTACK;
		double release = DEFAULT_RELEASE;
		double mix_start = DEFAULT_MIX;
		double mix_delta = 0.0;
	};

	// Soft-kneed excess past a threshold, in dB: zero below the knee, the raw
	// excess above it, quadratic across it.
	static _FORCE_INLINE_ double _soft_excess_db(double p_excess_db, double p_knee_db) {
		if (p_knee_db <= 0.0) {
			return MAX(p_excess_db, 0.0);
		}

		double half_knee = p_knee_db * 0.5;
		if (p_excess_db <= -half_knee) {
			return 0.0;
		}
		if (p_excess_db >= half_knee) {
			return p_excess_db;
		}

		double y = p_excess_db + half_knee;
		return y * y / (2.0 * p_knee_db);
	}

	static _FORCE_INLINE_ double _db_to_linear(double p_db) {
		return std::pow(10.0, p_db / 20.0);
	}

	// Mean square to dBFS. The detector carries 0.5 * (L^2 + R^2), so a mono
	// signal present in both channels reads its own level rather than +3 dB.
	static _FORCE_INLINE_ double _mean_square_to_db(double p_mean_square) {
		return 10.0 * std::log10(p_mean_square + EPSILON);
	}

	static _FORCE_INLINE_ double _compute_coeff(double p_time_ms, double p_sample_rate) {
		if (p_time_ms <= 0.0 || p_sample_rate <= 0.0) {
			return 0.0;
		}
		return std::exp(-1.0 / (p_time_ms * 0.001 * p_sample_rate));
	}

	// A single band: one mean-square envelope feeding an upward and a downward
	// gain computer, plus input/output/gain metering for the UI.
	class BandCompressor {
		double _base_attack_ms;
		double _base_release_ms;

		// Audio-thread-owned state.
		double _envelope = 0.0;
		double _input_mean_squared = 0.0;
		double _output_mean_squared = 0.0;
		double _output_mult = 1.0;

		// Written on the audio thread, read from the main thread.
		std::atomic<float> _meter_input_db{ -120.0f };
		std::atomic<float> _meter_output_db{ -120.0f };
		std::atomic<float> _meter_downward_db{ 0.0f };
		std::atomic<float> _meter_upward_db{ 0.0f };

	public:
		// Operates in place on an interleaved stereo band buffer.
		void process(double *p_audio, int p_length, const BandSettings &p_settings, double p_sample_rate);
		void reset();

		double get_input_db() const { return _meter_input_db.load(std::memory_order_relaxed); }
		double get_output_db() const { return _meter_output_db.load(std::memory_order_relaxed); }
		double get_downward_gain_reduction_db() const { return _meter_downward_db.load(std::memory_order_relaxed); }
		double get_upward_gain_db() const { return _meter_upward_db.load(std::memory_order_relaxed); }

		BandCompressor(double p_base_attack_ms, double p_base_release_ms) :
				_base_attack_ms(p_base_attack_ms),
				_base_release_ms(p_base_release_ms) {}
	};

	// Thread-safe parameter storage: written from the main thread through
	// set_params()/set_arg(), snapshotted on the audio thread at block start.
	// Values are seeded by the constructor, so the in-class initializers only
	// need to be well-defined, not correct.
	struct BandParams {
		std::atomic<float> upper_threshold{ 0.0f };
		std::atomic<float> lower_threshold{ 0.0f };
		std::atomic<float> upper_amount{ 0.0f };
		std::atomic<float> lower_amount{ 0.0f };
		std::atomic<float> output_gain{ 0.0f };
	};

	struct Params {
		std::atomic<float> enabled_bands{ (float)BAND_MULTIBAND };
		BandParams bands[BAND_COUNT];
		std::atomic<float> attack{ 0.0f };
		std::atomic<float> release{ 0.0f };
		std::atomic<float> mix{ 1.0f };
		std::atomic<float> lm_frequency{ 0.0f };
		std::atomic<float> mh_frequency{ 0.0f };
		std::atomic<float> knee_db{ 0.0f };
	};

	Params _params;
	std::atomic<bool> _reset_state_requested{ false };

	// One Linkwitz-Riley splitter per crossover point, plus a compensator that
	// runs the low band through the same allpass the mid/high split imposes on
	// everything above it, so the three bands sum flat at unity. The compensator
	// shares the mid/high coefficients. Each splitter holds one state per
	// channel.
	sion::dsp::LinkwitzRiley4Coeffs _lm_coeffs;
	sion::dsp::LinkwitzRiley4Coeffs _mh_coeffs;
	sion::dsp::LinkwitzRiley4 _lm_filter[2];
	sion::dsp::LinkwitzRiley4 _mh_filter[2];
	sion::dsp::LinkwitzRiley4 _low_compensation_filter[2];

	BandCompressor _low_compressor;
	BandCompressor _mid_compressor;
	BandCompressor _high_compressor;

	// Audio-thread-owned record of what the filters are currently tuned to.
	double _active_lm_frequency = 0.0;
	double _active_mh_frequency = 0.0;
	double _active_sample_rate = 0.0;
	int _active_mode = -1;

	// Where the de-zippered dry/wet ramp left off. The bands do the blending,
	// but the ramp is owned here so all three run the identical curve.
	// _mix_primed is false until a block has run since the last reset, which is
	// what stops a pooled instance from gliding down from the previous patch's
	// mix.
	double _mix_current = DEFAULT_MIX;
	bool _mix_primed = false;

	// Sized once, in the constructor. One scratch buffer serves both the
	// intermediate band between the two splits and, once that has been split
	// again, the low band's phase compensation.
	Vector<double> _low_buffer;
	Vector<double> _mid_buffer;
	Vector<double> _high_buffer;
	Vector<double> _scratch_buffer;

	void _reset_dsp_state();
	void _snapshot_bands(const BlockSettings &p_block, BandSettings r_bands[BAND_COUNT]) const;

	static void _split(int p_channels, const sion::dsp::LinkwitzRiley4Coeffs &p_coeffs, sion::dsp::LinkwitzRiley4 *r_filter, const double *p_input, double *p_low, double *p_high, int p_length);
	void _process_multiband(int p_channels, double *p_audio, int p_length, double p_sample_rate, const BandSettings *p_bands);
	void _process_low_band(int p_channels, double *p_audio, int p_length, double p_sample_rate, const BandSettings &p_low);
	void _process_high_band(int p_channels, double *p_audio, int p_length, double p_sample_rate, const BandSettings &p_high);
	void _process_full_range(double *p_audio, int p_length, double p_sample_rate, const BandSettings &p_mid);

	const BandCompressor *_get_band_compressor(int p_band) const;

protected:
	static void _bind_methods();

public:
	void set_params(int p_enabled_bands = BAND_MULTIBAND,
			double p_low_upper_threshold = DEFAULT_LOW_UPPER_THRESHOLD,
			double p_mid_upper_threshold = DEFAULT_MID_UPPER_THRESHOLD,
			double p_high_upper_threshold = DEFAULT_HIGH_UPPER_THRESHOLD,
			double p_low_lower_threshold = DEFAULT_LOW_LOWER_THRESHOLD,
			double p_mid_lower_threshold = DEFAULT_MID_LOWER_THRESHOLD,
			double p_high_lower_threshold = DEFAULT_HIGH_LOWER_THRESHOLD,
			double p_low_upper_amount = DEFAULT_LOW_UPPER_AMOUNT,
			double p_mid_upper_amount = DEFAULT_MID_UPPER_AMOUNT,
			double p_high_upper_amount = DEFAULT_HIGH_UPPER_AMOUNT,
			double p_low_lower_amount = DEFAULT_LOW_LOWER_AMOUNT,
			double p_mid_lower_amount = DEFAULT_MID_LOWER_AMOUNT,
			double p_high_lower_amount = DEFAULT_HIGH_LOWER_AMOUNT,
			double p_low_output_gain = DEFAULT_LOW_OUTPUT_GAIN,
			double p_mid_output_gain = DEFAULT_MID_OUTPUT_GAIN,
			double p_high_output_gain = DEFAULT_HIGH_OUTPUT_GAIN,
			double p_attack = DEFAULT_ATTACK,
			double p_release = DEFAULT_RELEASE,
			double p_mix = DEFAULT_MIX,
			double p_lm_frequency = DEFAULT_LM_FREQUENCY,
			double p_mh_frequency = DEFAULT_MH_FREQUENCY,
			double p_knee_db = DEFAULT_KNEE);

	virtual int prepare_process() override;
	virtual int process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;
	virtual bool set_arg(int p_arg_index, double p_value) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual void reset() override;

	// Metering. Band index: 0 = low, 1 = mid, 2 = high. Low Band and High Band
	// modes only move their own band; Full Range mode only moves the mid.
	double get_band_input_db(int p_band) const;
	double get_band_output_db(int p_band) const;
	double get_band_downward_gain_reduction_db(int p_band) const;
	double get_band_upward_gain_db(int p_band) const;

	SiEffectMultibandCompressor();
	~SiEffectMultibandCompressor() {}
};

#endif // SI_EFFECT_MB_COMPRESSOR_H
