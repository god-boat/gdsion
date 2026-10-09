// SiOPMChannelBase, reduced to what a pipe-rendering channel touches. Like the
// engine's buffer(), harness_buffer() skips a channel that is idling.
#pragma once

#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "harness_godot.h"
#include "templates/singly_linked_list.h"

using namespace godot;

class SiOPMChannelParams;

class SiOPMChannelBase : public Object {
	GDCLASS(SiOPMChannelBase, Object)

protected:
	static void _bind_methods() {}

	SiOPMRefTable *_table = nullptr;
	SiOPMSoundChip *_sound_chip = nullptr;
	Callable _process_function;

	bool _is_note_on = false;
	SinglyLinkedList<int> *_in_pipe = nullptr;
	SinglyLinkedList<int> *_base_pipe = nullptr;
	SinglyLinkedList<int> *_out_pipe = nullptr;

	Vector<double> _volumes;
	int _instrument_gain_db = 0;
	bool _is_source_idling = true;
	int _pan = 64;
	bool _has_effect_send = false;
	int _filter_type = 0;

public:
	virtual void get_channel_params(const Ref<SiOPMChannelParams> &p_params) const {}
	virtual void set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation = true) {}

	virtual int get_instrument_gain_db() const { return _instrument_gain_db; }
	virtual void set_instrument_gain_db(int p_db) { _instrument_gain_db = p_db; }
	virtual void set_sv_filter(int p_cutoff = 128, int p_resonance = 0, int p_attack_rate = 0, int p_decay_rate1 = 0, int p_decay_rate2 = 0, int p_release_rate = 0, int p_decay_cutoff1 = 128, int p_decay_cutoff2 = 128, int p_sustain_cutoff = 128, int p_release_cutoff = 128) {}

	virtual int get_pitch() const { return 0; }
	virtual void set_pitch(int p_value) {}
	virtual void offset_volume(int p_expression, int p_velocity) {}

	virtual bool is_idling() const { return _is_source_idling; }
	virtual void note_on() { _is_note_on = true; }
	virtual void note_off() { _is_note_on = false; }

	virtual void initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
		_volumes.resize(SiOPMSoundChip::STREAM_SEND_SIZE);
		_volumes.write[0] = 1.0;
	}
	virtual void reset() {
		_is_note_on = false;
		_is_source_idling = true;
	}

	void harness_attach(SinglyLinkedList<int> *p_in, SinglyLinkedList<int> *p_base, SinglyLinkedList<int> *p_out) {
		_in_pipe = p_in;
		_base_pipe = p_base;
		_out_pipe = p_out;
	}

	// Renders p_length samples into fresh pipe chains of zeros.
	void harness_buffer(int p_length) {
		_in_pipe->harness_reset(p_length);
		_base_pipe->harness_reset(p_length);
		_out_pipe->harness_reset(p_length);
		if (!is_idling()) {
			_process_function.call(p_length);
		}
	}

	SiOPMChannelBase(SiOPMSoundChip *p_chip = nullptr) :
			_sound_chip(p_chip) {
		_table = SiOPMRefTable::get_instance();
	}
	virtual ~SiOPMChannelBase() {}
};
