extends TestBase

var group: String = "Engine Parameters"
var name: String = "Strata and FM Scalar Lane"

const ENGINE := 23
const CHIP := 9
const BUFFER_SIZE := 256
const OVERFLOW_PUSHES := 4096

var _params: Dictionary = {}
var _fm_params: Dictionary = {}


func run(scene_tree: SceneTree) -> void:
	var descriptors: Array[Dictionary] = []
	for descriptor: Dictionary in SiONVoice.get_engine_param_descriptors(ENGINE):
		if String(descriptor["id"]).begins_with("voice.strata."):
			descriptors.append(descriptor)
	_assert_equal("registered Strata scalar count", descriptors.size(), 3)
	for descriptor: Dictionary in descriptors:
		_params[String(descriptor["id"])] = int(descriptor["param_id"])
		_assert_equal("%s scalar type" % descriptor["id"], descriptor["scalar_type"], TYPE_INT)
		_assert_equal("%s single instance" % descriptor["id"], descriptor["instance_count"], 1)
		_assert_equal("%s realtime support" % descriptor["id"], descriptor["realtime"], true)
	_test_voice_state(descriptors)
	_test_engine_changes()
	_test_fm_voice_state()
	var expected := await _render_fm_scalars(scene_tree, false)
	var queued := await _render_fm_scalars(scene_tree, true)
	_assert_equal("FM scoped scalar edits render exactly like prepared state", queued, expected)
	var peak := 0.0
	for sample in expected:
		peak = maxf(peak, absf(sample))
	_assert_equal("FM comparison renders audio", peak > 0.0, true)
	for mode in [0, 1, 2]:
		var register_audio := await _render_opm_lfo(scene_tree, mode, false)
		var reapplied_audio := await _render_opm_lfo(scene_tree, mode, true)
		_assert_equal("OPM register rate survives mode roundtrip from mode %d" % mode, reapplied_audio, register_audio)
		var register_peak := 0.0
		for sample in register_audio:
			register_peak = maxf(register_peak, absf(sample))
		_assert_equal("OPM LFO comparison renders audio in mode %d" % mode, register_peak > 0.0, true)
	await _run_case(scene_tree, "scoped updates", _test_scopes)
	await _run_case(scene_tree, "multi-write boundary", _test_boundary)
	await _run_case(scene_tree, "queue overflow", _test_overflow)
	await _run_case(scene_tree, "retired scope", _test_retirement)


func _voice() -> SiONVoice:
	return SiONVoice.create_engine(ENGINE, CHIP)


func _id(name_suffix: String) -> int:
	return _params["voice.strata." + name_suffix]


func _test_voice_state(descriptors: Array[Dictionary]) -> void:
	var voice := _voice()
	for descriptor: Dictionary in descriptors:
		var param_id := int(descriptor["param_id"])
		_assert_equal("%s engine reset" % descriptor["id"], voice.get_engine_param(param_id), descriptor["reset_value"])
		voice.set_engine_param(param_id, int(descriptor["max_value"]))
	var version := voice.clone()
	voice.set_engine_param(_id("timbre"), 1234)
	_assert_equal("copy owns its parameter state", version.get_engine_param(_id("timbre")), 32767)
	_assert_equal("prepared voice readback", voice.get_engine_param(_id("timbre")), 1234)
	var mml := voice.get_mml(7, CHIP, false)
	_assert_equal("MML preserves declaration order and text format", mml, "#BR@7{47,1234,32767};")
	var imported := SiONVoice.create()
	_assert_equal("MML retains its voice identity", imported.set_by_mml(mml), 7)
	for descriptor: Dictionary in descriptors:
		var param_id := int(descriptor["param_id"])
		_assert_equal("%s MML roundtrip" % descriptor["id"], imported.get_engine_param(param_id), voice.get_engine_param(param_id))
	_assert_equal("short MML retains its voice identity", imported.set_by_mml("#BR@8{ 1 };"), 8)
	_assert_equal("short MML imports the supplied value", imported.get_engine_param(_id("shape")), 1)
	for descriptor: Dictionary in descriptors.slice(1):
		_assert_equal("%s omitted MML value uses schema reset" % descriptor["id"], imported.get_engine_param(int(descriptor["param_id"])), descriptor["reset_value"])
	_assert_equal("empty MML imports schema defaults", imported.set_by_mml("#BR@9{};"), 9)
	for descriptor: Dictionary in descriptors:
		_assert_equal("%s empty MML value uses schema reset" % descriptor["id"], imported.get_engine_param(int(descriptor["param_id"])), descriptor["reset_value"])
	voice.set_module_type(ENGINE)
	_assert_equal("selecting the same engine preserves scalar state", voice.get_engine_param(_id("timbre")), 1234)
	# Leaving and returning to an engine must not retain stale scalar storage.
	voice.set_module_type(5)
	voice.set_module_type(ENGINE)
	_assert_equal("engine replacement resets scalar state", voice.get_engine_param(_id("timbre")), 0)
	voice.set_engine_param(_id("color"), 42)
	voice.set_note_shift(12)
	_assert_equal("MML import resets the whole voice", voice.set_by_mml("#BR@10{};"), 10)
	_assert_equal("whole voice reset clears shared state", voice.get_note_shift(), 0)
	_assert_equal("whole voice reset clears scalar state", voice.get_engine_param(_id("color")), 0)


func _test_engine_changes() -> void:
	_assert_equal("FM engine registers shared and operator scalars", SiONVoice.get_engine_param_descriptors(5).size(), 55)
	for setter: StringName in [&"set_guitar6", &"set_monolith", &"set_iron", &"set_talus"]:
		var voice := _voice()
		voice.set_engine_param(_id("color"), 42)
		var op := voice.get_channel_params().get_operator_params(0)
		op.set_pulse_generator_type(5) # Square, distinct from these engines' default sine.
		voice.call(setter)
		_assert_equal("%s preserves the authored waveform" % setter, op.get_pulse_generator_type(), 5)
		voice.set_module_type(ENGINE)
		_assert_equal("%s clears the previous engine's scalar state" % setter, voice.get_engine_param(_id("color")), 0)
		_assert_equal("explicit module selection retains waveform selection", op.get_pulse_generator_type(), 0)
	var sampler := _voice()
	var sampler_op := sampler.get_channel_params().get_operator_params(0)
	sampler_op.set_pulse_generator_type(5)
	sampler.set_sampler_voice(PackedFloat32Array([0.0, 0.0]))
	_assert_equal("sampler data assignment preserves the authored waveform", sampler_op.get_pulse_generator_type(), 5)


func _test_fm_voice_state() -> void:
	var descriptors := SiONVoice.get_engine_param_descriptors(5)
	var voice := SiONVoice.create_engine(5, 0)
	voice.get_channel_params().set_operator_count(4)
	for descriptor: Dictionary in descriptors:
		_fm_params[String(descriptor["id"])] = int(descriptor["param_id"])
		for index in int(descriptor["instance_count"]):
			_assert_equal("FM %s[%d] native reset" % [descriptor["id"], index],
				voice.get_engine_param(int(descriptor["param_id"]), index), descriptor["reset_value"])
	var multiple: int = _fm_params["operator.frequency.fine_multiple"]
	var waveform: int = _fm_params["operator.waveform"]
	var mute: int = _fm_params["operator.mute"]
	voice.set_engine_param(multiple, 321, 2)
	voice.set_engine_param(waveform, 26, 2)
	voice.set_engine_param(mute, true, 2)
	_assert_equal("FM scalar retains exact fine precision", voice.get_channel_params().get_operator_params(2).get_fine_multiple(), 321)
	_assert_equal("FM scalar preserves operator isolation", voice.get_engine_param(multiple, 0), 128)
	_assert_equal("FM scalar imports native bool type", typeof(voice.get_engine_param(mute, 2)), TYPE_BOOL)
	_assert_equal("FM waveform selects its native pitch table", voice.get_channel_params().get_operator_params(2).get_pitch_table_type(), 3)
	var version := voice.clone()
	voice.get_channel_params().get_operator_params(2).set_fine_multiple(777)
	voice.get_channel_params().set_filter_cutoff(47)
	voice.set_portament_ms(27)
	_assert_equal("FM scalar reads legacy operator writes", voice.get_engine_param(multiple, 2), 777)
	_assert_equal("FM scalar reads legacy channel writes", voice.get_engine_param(_fm_params["voice.filter.cutoff"]), 47)
	_assert_equal("FM scalar reads legacy voice writes", voice.get_engine_param(_fm_params["voice.glide.time"]), 27)
	_assert_equal("FM copied native state is independent", version.get_engine_param(multiple, 2), 321)
	voice.set_engine_param(_fm_params["voice.lfo.rate"], 900)
	voice.set_engine_param(_fm_params["voice.lfo.time"], 333)
	voice.set_engine_param(_fm_params["voice.lfo.sync_division"], 4)
	voice.set_engine_param(_fm_params["voice.lfo.mode"], 1)
	_assert_equal("FM LFO reads the selected mode's raw value", voice.get_channel_params().get_lfo_frequency_step(), 333)
	_assert_equal("FM LFO inactive values stay independent", voice.get_engine_param(_fm_params["voice.lfo.rate"]), 900)
	var mml_voice := SiONVoice.create_engine(5, 0)
	mml_voice.set_engine_param(_fm_params["operator.envelope.attack_rate"], 24)
	var imported := SiONVoice.create()
	_assert_equal("FM scalar state retains its MML codec", imported.set_by_mml(mml_voice.get_mml(7, 0, false)), 7)
	_assert_equal("FM MML import is visible through scalar readback", imported.get_engine_param(_fm_params["operator.envelope.attack_rate"]), 24)
	_assert_equal("FM MML import resets previous operator state", voice.set_by_mml(mml_voice.get_mml(8, 0, false)), 8)
	_assert_equal("FM reset clears the old bool state", voice.get_engine_param(mute, 2), false)
	_assert_equal("FM reset clears the old voice state", voice.get_engine_param(_fm_params["voice.glide.time"]), 0)


## Compare real audio with identical prepared voices. Only one exact scope is
## edited; sibling slots/entities, another track and another FM module coexist.
## This observes operator selection and channel/track DSP without test-only APIs.
func _render_fm_scalars(scene_tree: SceneTree, queued: bool) -> PackedFloat32Array:
	var driver := SiONDriver.create(BUFFER_SIZE)
	scene_tree.root.add_child(driver)
	await scene_tree.process_frame
	driver.stream_without_output()
	var renderer := SiONOfflineRenderer.new()
	_assert_equal("FM renderer owns the audio thread", renderer.begin(driver), true)
	var base := SiONVoice.create_engine(5, 0)
	base.get_channel_params().set_operator_count(2)
	base.set_engine_param(_fm_params["voice.algorithm"], 1)
	base.set_engine_param(_fm_params["voice.filter.cutoff"], 64)
	base.set_engine_param(_fm_params["voice.filter_envelope.sustain_cutoff"], 32)
	base.set_engine_param(_fm_params["operator.mute"], true, 1)
	base.set_engine_param(_fm_params["operator.amplitude_modulation.shift"], 2, 1)
	base.set_amplitude_modulation(64)
	var edits := [
		["operator.frequency.fine_multiple", 321, 1],
		["operator.waveform", 5, 1],
		["operator.envelope.attack_rate", 63, 1],
		["operator.mute", false, 1],
		["voice.feedback.connection", 1, 0],
		["voice.feedback.amount", 3, 0],
		["voice.algorithm", 0, 0],
		["voice.lfo.rate", 900, 0],
		["voice.lfo.time", 333, 0],
		["voice.lfo.sync_division", 4, 0],
		["voice.lfo.mode", 1, 0],
		["voice.filter.cutoff", 48, 0],
		["voice.filter_envelope.decay_1_rate", 37, 0],
		["voice.output.gain", -12, 0],
		["voice.pitch_bend", 45, 0],
	]
	var tracks: Array[SiMMLTrack] = []
	for scope: Array in [[1, 101, 11], [1, 101, 12], [1, 202, 11], [2, 101, 11], [1, 101, 11]]:
		var track := driver.create_user_controllable_track(scope[0])
		track.set_entity_scope_id(scope[1])
		track.set_slot_scope_id(scope[2])
		var voice := base.clone()
		if tracks.size() == 4:
			voice.set_module_type(6) # Same concrete FM channel, different engine identity.
		if tracks.is_empty() and not queued:
			for edit: Array in edits:
				voice.set_engine_param(_fm_params[edit[0]], edit[1], edit[2])
		voice.update_track_voice(track)
		tracks.append(track)
	var channel: Object = tracks[0].get_channel()
	if queued:
		for edit: Array in edits:
			driver.mailbox_set_engine_param(1, 5, _fm_params[edit[0]], edit[1], edit[2], 101, 11)
	for i in tracks.size():
		driver.mailbox_key_on(tracks[i].get_track_id(), 60 + i, 0, -1, -1, tracks[i].get_instance_id())
	var audio := PackedFloat32Array()
	for block in 4:
		audio.append_array(renderer.render_block())
	_assert_equal("FM scalar edits retain the live channel", is_same(channel, tracks[0].get_channel()), true)
	renderer.finish()
	driver.stop()
	await scene_tree.process_frame
	driver.get_parent().remove_child(driver)
	driver.free()
	return audio


## A hardware register supplies a Rate value even while Time or Sync is selected.
## Reapplying modes before the first block must retain that effective frequency.
func _render_opm_lfo(scene_tree: SceneTree, initial_mode: int, roundtrip: bool) -> PackedFloat32Array:
	var driver := SiONDriver.create(BUFFER_SIZE)
	scene_tree.root.add_child(driver)
	await scene_tree.process_frame
	driver.stream_without_output()
	var renderer := SiONOfflineRenderer.new()
	_assert_equal("OPM LFO renderer owns the audio thread", renderer.begin(driver), true)
	var voice := SiONVoice.create_engine(5, 2)
	voice.set_engine_param(_fm_params["voice.lfo.rate"], 900)
	voice.set_engine_param(_fm_params["voice.lfo.time"], 333)
	voice.set_engine_param(_fm_params["voice.lfo.sync_division"], 4)
	voice.set_engine_param(_fm_params["voice.lfo.mode"], initial_mode)
	voice.set_engine_param(_fm_params["operator.amplitude_modulation.shift"], 3)
	voice.set_amplitude_modulation(64)
	var track := driver.create_user_controllable_track(1)
	voice.update_track_voice(track)
	# The track's existing register callback is the same path MML register events use.
	track.call(&"_default_update_register", 24, 224)
	if roundtrip:
		for mode in [1, 2, 0]:
			driver.mailbox_set_engine_param(1, 5, _fm_params["voice.lfo.mode"], mode)
	driver.mailbox_key_on(1, 60, 0, -1, -1, track.get_instance_id())
	var audio := PackedFloat32Array()
	for block in 8:
		audio.append_array(renderer.render_block())
	renderer.finish()
	driver.stop()
	await scene_tree.process_frame
	driver.get_parent().remove_child(driver)
	driver.free()
	return audio


func _run_case(scene_tree: SceneTree, label: String, body: Callable) -> void:
	var driver := SiONDriver.create(BUFFER_SIZE)
	scene_tree.root.add_child(driver)
	await scene_tree.process_frame
	driver.stream_without_output()
	var renderer := SiONOfflineRenderer.new()
	_assert_equal("%s: sole render owner" % label, renderer.begin(driver), true)
	body.call(label, driver, renderer)
	renderer.finish()
	driver.stop()
	await scene_tree.process_frame
	driver.get_parent().remove_child(driver)
	driver.free()


func _track(driver: SiONDriver, track_id: int, entity: int, slot: int) -> SiMMLTrack:
	var track := driver.create_user_controllable_track(track_id)
	track.set_entity_scope_id(entity)
	track.set_slot_scope_id(slot)
	_voice().update_track_voice(track)
	return track


func _read(track: SiMMLTrack, suffix: String) -> int:
	return _read_strata_diagnostic(track.get_channel(), suffix)


# Strata already publishes these fields for diagnostics. This test does not add
# a readback contract that each migrated engine would have to implement.
func _read_strata_diagnostic(channel: Object, suffix: String) -> int:
	return str(channel).split(suffix + "=")[1].get_slice(",", 0).to_int()


func _push(driver: SiONDriver, suffix: String, value: int, entity: int = -1, slot: int = -1) -> void:
	driver.mailbox_set_engine_param(1, ENGINE, _id(suffix), value, 0, entity, slot)


func _test_scopes(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	var selected := _track(driver, 1, 101, 11)
	var sibling := _track(driver, 1, 101, 12)
	var foreign := _track(driver, 1, 202, 11)
	var other_track := _track(driver, 2, 101, 11)
	var other_engine := driver.create_user_controllable_track(1)
	other_engine.set_entity_scope_id(101)
	other_engine.set_slot_scope_id(11)
	SiONVoice.create().update_track_voice(other_engine)
	_push(driver, "timbre", 1234, 101, 11)
	_assert_equal("%s: main thread does not mutate the channel" % label, _read(selected, "timbre"), 0)
	renderer.render_block()
	_assert_equal("%s: exact entity/slot receives update" % label, _read(selected, "timbre"), 1234)
	_assert_equal("%s: sibling slot stays unchanged" % label, _read(sibling, "timbre"), 0)
	_assert_equal("%s: foreign entity stays unchanged" % label, _read(foreign, "timbre"), 0)
	_assert_equal("%s: another logical track stays unchanged" % label, _read(other_track, "timbre"), 0)
	var other_channel: Object = other_engine.get_channel()
	_assert_equal("%s: another engine retains its channel" % label, other_channel.get_class(), "SiOPMChannelFM")
	_push(driver, "color", 4567, 101)
	renderer.render_block()
	_assert_equal("%s: entity update reaches first slot" % label, _read(selected, "color"), 4567)
	_assert_equal("%s: entity update reaches sibling slot" % label, _read(sibling, "color"), 4567)
	_assert_equal("%s: entity update excludes other entity" % label, _read(foreign, "color"), 0)
	_push(driver, "shape", 1, -1, 11)
	renderer.render_block()
	_assert_equal("%s: slot-only update reaches first entity" % label, _read(selected, "shape"), 1)
	_assert_equal("%s: slot-only update reaches second entity" % label, _read(foreign, "shape"), 1)
	_assert_equal("%s: slot-only update excludes sibling slot" % label, _read(sibling, "shape"), 0)
	_push(driver, "timbre", 3000)
	renderer.render_block()
	for track in [selected, sibling, foreign]:
		_assert_equal("%s: unscoped update reaches matching Strata leaf" % label, _read(track, "timbre"), 3000)


func _test_boundary(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	var track := _track(driver, 1, 101, 11)
	var voice := _voice()
	voice.set_engine_param(_id("timbre"), 9000)
	voice.set_engine_param(_id("color"), 18000)
	voice.update_track_voice(track)
	_assert_equal("%s: published voice initializes the channel" % label, _read(track, "color"), 18000)
	var channel: Object = track.get_channel()
	_push(driver, "shape", 1)
	_push(driver, "timbre", 2000)
	_push(driver, "color", 3000)
	_push(driver, "timbre", 4000)
	_assert_equal("%s: queued writes preserve the current block" % label, _read(track, "timbre"), 9000)
	driver.mailbox_key_on(1, 60, 0, -1, -1, track.get_instance_id())
	var output := renderer.render_block()
	var peak := 0.0
	for sample in output:
		peak = maxf(peak, absf(sample))
	_assert_equal("%s: writes queued before drain apply before the next samples" % label,
		[_read(track, "shape"), _read(track, "timbre"), _read(track, "color")], [1, 4000, 3000])
	_assert_equal("%s: live scalar edits preserve the channel" % label, is_same(track.get_channel(), channel), true)
	_assert_equal("%s: renderer produces audio" % label, peak > 0.0, true)
	_push(driver, "color", 5000)
	_assert_equal("%s: a later write awaits its own block" % label, _read(track, "color"), 3000)
	renderer.render_block()
	_assert_equal("%s: next block receives the later write" % label, _read(track, "color"), 5000)


func _test_overflow(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	var track := _track(driver, 1, 101, 11)
	for i in range(OVERFLOW_PUSHES):
		_push(driver, "timbre", i)
	_push(driver, "color", 9876)
	for block in range(64):
		renderer.render_block()
		if _read(track, "color") == 9876:
			break
	_assert_equal("%s: backlogged scalar reaches the render owner" % label, _read(track, "color"), 9876)
	_assert_equal("%s: backlogged scalars preserve FIFO order" % label, _read(track, "timbre"), OVERFLOW_PUSHES - 1)


func _test_retirement(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	var retired := _track(driver, 1, 101, 11)
	var sibling := _track(driver, 1, 202, 12)
	driver.mailbox_key_on(1, 60, 0, -1, -1, retired.get_instance_id())
	renderer.render_block()
	var retired_channel: Object = retired.get_channel()
	driver.mailbox_retire_track(1, retired.get_instance_id())
	_push(driver, "timbre", 7654, 101, 11)
	_push(driver, "color", 8765, 202, 12)
	renderer.render_block()
	_assert_equal("%s: queued retirement excludes subsequent edits" % label, _read_strata_diagnostic(retired_channel, "timbre"), 0)
	_assert_equal("%s: surviving scoped target receives its edit" % label, _read(sibling, "color"), 8765)
