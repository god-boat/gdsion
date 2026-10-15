extends TestBase

var group: String = "Engine Parameters"
var name: String = "Strata Scalar Lane"

const ENGINE := 23
const CHIP := 9
const BUFFER_SIZE := 256
const OVERFLOW_PUSHES := 4096

var _params: Dictionary = {}


func run(scene_tree: SceneTree) -> void:
	var descriptors := SiONVoice.get_engine_param_descriptors(ENGINE)
	_assert_equal("registered Strata scalar count", descriptors.size(), 3)
	for descriptor: Dictionary in descriptors:
		_params[String(descriptor["id"])] = int(descriptor["param_id"])
		_assert_equal("%s scalar type" % descriptor["id"], descriptor["scalar_type"], TYPE_INT)
		_assert_equal("%s single instance" % descriptor["id"], descriptor["instance_count"], 1)
		_assert_equal("%s realtime support" % descriptor["id"], descriptor["realtime"], true)
	_test_voice_state(descriptors)
	_test_engine_changes()
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
	_assert_equal("unmigrated engine has no scalar descriptors", SiONVoice.get_engine_param_descriptors(5).is_empty(), true)
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
