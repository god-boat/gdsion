/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_VOICE_H
#define SION_VOICE_H

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include "sequencer/simml_voice.h"

using namespace godot;

class SiOPMWaveTable;
class SiOPMWavePCMData;
class SiOPMWaveSamplerData;
enum SiONChipType : signed int;
enum SiONModuleType : unsigned int;

// Provides all of voice setting parameters of SiON.
class SiONVoice : public SiMMLVoice {
	GDCLASS(SiONVoice, SiMMLVoice)

	String _name;

protected:
	static void _bind_methods();

public:
	// NOTE: Godot doesn't support exposing constructors to the API, so we make do with a static factory method. Hopefully this can be fixed at some point.
	static Ref<SiONVoice> create(SiONModuleType p_module_type = (SiONModuleType)5, int p_channel_num = 0, int p_attack_rate = 58, int p_release_rate = 63, int p_pitch_shift = 0, int p_connection_type = -1, int p_wave_shape2 = 0, int p_pitch_shift2 = 0);
	// Engine construction supplies structure; authored parameters use the normal setters.
	static Ref<SiONVoice> create_engine(SiONModuleType p_module_type, SiONChipType p_chip_type);
	static TypedArray<Dictionary> get_engine_param_descriptors(int p_engine_kind);
	void set_engine_param(int p_param_id, const Variant &p_value, int p_index = 0);
	Variant get_engine_param(int p_param_id, int p_index = 0) const;

	//

	String get_name() const { return _name; }
	void set_name(const String &p_name) { _name = p_name; }

	void set_params(TypedArray<int> p_args);
	void set_params_opl(TypedArray<int> p_args);
	void set_params_opm(TypedArray<int> p_args);
	void set_params_opn(TypedArray<int> p_args);
	void set_params_opx(TypedArray<int> p_args);
	void set_params_ma3(TypedArray<int> p_args);
	void set_params_al(TypedArray<int> p_args);

	TypedArray<int> get_params() const;
	TypedArray<int> get_params_opl() const;
	TypedArray<int> get_params_opm() const;
	TypedArray<int> get_params_opn() const;
	TypedArray<int> get_params_opx() const;
	TypedArray<int> get_params_ma3() const;
	TypedArray<int> get_params_al() const;

	String get_mml(int p_index, SiONChipType p_chip_type = (SiONChipType)-1, bool p_append_postfix = true) const;
	int set_by_mml(String p_mml);

	Ref<SiOPMWaveTable> set_wave_table(Vector<double> *p_data);

	Ref<SiOPMWavePCMData> set_pcm_voice(const Variant &p_data, int p_sampling_note = 69, int p_src_channel_count = 2, int p_channel_count = 0);
	Ref<SiOPMWavePCMData> set_pcm_wave(int p_index, const Variant &p_data, int p_sampling_note = 69, int p_key_range_from = 0, int p_key_range_to = 127, int p_src_channel_count = 2, int p_channel_count = 0);
	Ref<SiOPMWaveSamplerData> set_sampler_voice(const Variant &p_data, bool p_ignore_note_off = false, int p_channel_count = 2);
	Ref<SiOPMWaveSamplerData> set_sampler_wave(int p_index, const Variant &p_data, bool p_ignore_note_off = false, int p_pan = 0, int p_src_channel_count = 2, int p_channel_count = 0, bool p_fixed_pitch = false);


	void set_pitch_envelope(TypedArray<int> p_envelope, int p_loop_point = -1, int p_step = 1);


	// --- pitch_shift accessor ------------------------------------------------
	int get_pitch_shift() const { return pitch_shift; }
	void set_pitch_shift(int p_shift) { pitch_shift = p_shift; }
	int get_pitch_bend() const { return pitch_bend; }
	void set_pitch_bend(int p_bend) { pitch_bend = p_bend; }
	// -----------------------------------------------------------------------------

	Ref<SiONVoice> clone();
	virtual void reset() override;

	SiONVoice(SiONModuleType p_module_type = (SiONModuleType)5, int p_channel_num = 0, int p_attack_rate = 58, int p_release_rate = 63, int p_pitch_shift = 0, int p_connection_type = -1, int p_wave_shape2 = 0, int p_pitch_shift2 = 0);
	~SiONVoice() {}

	// Access the internal wave_data (sampler table / data). Primarily for scripting utilities.
	Variant get_wave_data() const { return wave_data; }
};

#endif // SION_VOICE_H
