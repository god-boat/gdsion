#ifndef SIOPM_CHANNEL_GUITAR6_H
#define SIOPM_CHANNEL_GUITAR6_H

#include <godot_cpp/templates/vector.hpp>
#include "chip/channels/siopm_channel_base.h"

using namespace godot;

class SiOPMSoundChip;

class SiOPMChannelGuitar6 : public SiOPMChannelBase {
	GDCLASS(SiOPMChannelGuitar6, SiOPMChannelBase)

	static constexpr int NUM_STRINGS = 6;
	static constexpr int MAX_DELAY_SAMPLES = 5400;
	static constexpr int CHAR_NOISE_LENGTH = 5400;
	static constexpr int MAX_PENDING_PLUCKS = 64;
	static constexpr double REFERENCE_SAMPLE_RATE = 48000.0;

	// Standard guitar open-string semitone offsets (E2..E4).
	static constexpr int STRING_SEMITONES[NUM_STRINGS] = { 19, 24, 29, 34, 38, 43 };

	// --- Seeded noise generator ---

	struct NoiseGen {
		static constexpr uint32_t MODULUS = 4151801719u;
		uint32_t seed = 0;

		double norm() {
			uint32_t lo = (16807u * (seed & 0xFFFF)) >> 0;
			uint32_t hi = (16807u * (seed >> 16)) >> 0;
			lo = (lo + ((hi & 0x7FFF) << 16)) >> 0;
			lo = (lo + (hi >> 15)) >> 0;
			if (lo > MODULUS) {
				lo -= MODULUS;
			}
			seed = lo;
			return (double)lo / (double)MODULUS;
		}

		double bipolar() { return norm() * 2.0 - 1.0; }
	};

	// --- Pluck event ---

	struct PluckEvent {
		int string_index = 0;
		int tab_index = 0;
		double velocity = 0;
		int target_sample = 0;
	};

	// --- Per-string state ---

	struct GuitarString {
		double delay[MAX_DELAY_SAMPLES] = {};

		NoiseGen string_noise_gen;
		NoiseGen damp_noise_gen;

		double af = 0;
		double df = 0;
		double dr = 0;

		double velocity = 0;
		int write_index = -1;
		bool feed_noise = false;
		double char_variation = 0;
		double pre_pan = 0;
		double read_offset = 0;
		double plug_damp = 0;
		int period_n = -1;
		double dc = 0;
		double gain_l = 0;
		double gain_r = 0;
		int semitone = 0;

		double noise_lp_coef = 0;
		double noise_gain_compensation = 1.0;
		double char_noise_step = 1.0;
		double noise_lp0 = 0;
		double noise_lp1 = 0;
		double char_noise_pos = 0;

		PluckEvent pending_plucks[MAX_PENDING_PLUCKS];
		int pending_pluck_count = 0;

		void init(int p_index, double p_sample_rate);
		void add_pluck(int p_tab_index, double p_velocity, int p_target_sample);
		void process(double *p_left, double *p_right, int p_block_start, int p_num_samples,
				const double *p_char_noise, int p_char_noise_len,
				double p_character_variation, double p_string_damp, double p_string_damp_variation,
				double p_plug_damp_param, double p_plug_damp_variation, double p_string_tension,
				double p_stereo_spread, double p_sample_rate);
		void reset_state();

	private:
		void execute_pluck(const PluckEvent &p_pluck, const double *p_char_noise, int p_char_noise_len,
				double p_character_variation, double p_string_damp, double p_string_damp_variation,
				double p_plug_damp_param, double p_plug_damp_variation, double p_string_tension,
				double p_stereo_spread, double p_sample_rate);
		void process_add(double *p_left, double *p_right, int p_start, int p_count,
				const double *p_char_noise, int p_char_noise_len);
	};

	// --- Body resonator ---

	struct Resonator {
		double b0 = 0, a1 = 0, a2 = 0, gain = 0;
		double x1l = 0, x2l = 0, y1l = 0, y2l = 0;
		double x1r = 0, x2r = 0, y1r = 0, y2r = 0;

		void init(double p_freq, double p_q, double p_gain, double p_sample_rate);
		void process(double *p_left, double *p_right, int p_num_samples);
	};

	// --- Channel-level state ---

	NoiseGen _char_noise_gen;
	double _char_noise[CHAR_NOISE_LENGTH] = {};
	GuitarString _strings[NUM_STRINGS];
	Resonator _body_resonators[3];
	bool _body_bypass = false;

	double _character_seed = 65535.0;
	double _character_variation = 0.5;
	double _string_damp = 0.5;
	double _string_damp_variation = 0.25;
	double _plug_damp = 0.5;
	double _plug_damp_variation = 0.25;
	double _string_tension = 0.0;
	double _stereo_spread = 0.2;

	double _expression = 1.0;

	int _current_pitch = 0;

	// Scratch buffers for stereo mixing within one block.
	double _scratch_left[2048] = {};
	double _scratch_right[2048] = {};

	void _randomize_character_noise(uint32_t p_seed);
	void _init_body_resonators(double p_sample_rate);

	void _no_process_guitar(int p_length);

protected:
	static void _bind_methods() {}

	String _to_string() const;

public:
	void set_guitar6_params(double p_character_seed, double p_character_variation,
			double p_string_damp, double p_string_damp_variation,
			double p_plug_damp, double p_plug_damp_variation,
			double p_string_tension, double p_stereo_spread, bool p_body_bypass);

	void pluck_string(int p_string_index, int p_tab_index, double p_velocity, int p_target_sample);

	virtual int get_pitch() const override { return _current_pitch; }
	virtual void set_pitch(int p_value) override { _current_pitch = p_value; }

	virtual void set_all_attack_rate(int p_value) override {}
	virtual void set_all_release_rate(int p_value) override {}

	virtual void offset_volume(int p_expression, int p_velocity) override;

	virtual void note_on() override;
	virtual void note_off() override;

	virtual void reset_channel_buffer_status() override;
	virtual void buffer(int p_length) override;

	virtual void initialize(SiOPMChannelBase *p_prev, int p_buffer_index) override;
	virtual void reset() override;

	SiOPMChannelGuitar6(SiOPMSoundChip *p_chip = nullptr);
};

#endif // SIOPM_CHANNEL_GUITAR6_H
