/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_STREAM_H
#define SI_EFFECT_STREAM_H

#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/string.hpp>
#include "effector/si_effect_base.h"

using namespace godot;

class SiOPMSoundChip;
class SiOPMStream;

class SiEffectStream {

	SiOPMSoundChip *_sound_chip = nullptr;
	Vector<Ref<SiEffectBase>> _chain;
	Vector<bool> _bypassed;

	SiOPMStream *_stream = nullptr;
	double _post_fader_gain = 1.0;
	int _post_pan = 64; // 0-128
	// Deeper streams execute first.
	int _depth = 0;
	int _pan = 64;
	bool _has_effect_send = false;
	bool _mute = false;

	Vector<double> _volumes;
	Vector<SiOPMStream *> _output_streams;

	void _add_effect(String p_cmd, Vector<double> p_args, int p_argc);
	void _set_postfix_param(int p_slot, String p_cmd, Vector<double> p_args, int p_argc);

public:
	Vector<Ref<SiEffectBase>> get_chain() const { return _chain; }
	void set_chain(const Vector<Ref<SiEffectBase>> &p_effects) { _chain = p_effects; }
	void add_to_chain(const Ref<SiEffectBase> &p_effect) { _chain.push_back(p_effect); }
	SiOPMStream *get_stream() const { return _stream; }
	int get_effect_count() const { return _chain.size(); }
	void insert_effect(int p_index, const Ref<SiEffectBase> &p_effect);
	void remove_effect(int p_index);
	void swap_effects(int p_index_a, int p_index_b);
	void set_effect_args(int p_index, Vector<double> p_args);
	void set_effect_bypass(int p_index, bool p_bypassed);

	double get_post_fader_gain() const { return _post_fader_gain; }
	int get_post_pan() const { return _post_pan; }

	int get_depth() const { return _depth; }
	int get_pan() const;
	void set_pan(int p_value);

	bool is_mute() const { return _mute; }
	void set_mute(bool p_value) { _mute = p_value; }

	bool is_outputting_directly() const;

	void set_all_stream_send_levels(Vector<int> p_param);
	void set_stream_send(int p_stream_num, double p_volume);
	double get_stream_send(int p_stream_num);

	void connect(SiOPMStream *p_output = nullptr);
	void set_post_fader_gain(double p_gain);
	void set_post_pan(int p_pan);
	int prepare_process();
	int process(int p_start_idx, int p_length, bool p_write_in_stream = true);

	//

	void parse_mml(int p_slot, String p_mml, String p_postfix);

	void initialize(int p_depth);
	void reset();
	void free();

	// Prefer creating effect streams via SiEffector.
	SiEffectStream(SiOPMSoundChip *p_chip, SiOPMStream *p_stream = nullptr);
	~SiEffectStream() {}
};

#endif // SI_EFFECT_STREAM_H
