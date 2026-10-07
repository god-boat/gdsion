###################################################
# Part of GDSiON tests                            #
# Copyright (c) 2024 Yuri Sizov and contributors  #
# Provided under MIT                              #
###################################################

extends TestBase

var group: String = "SiONDriver"
var name: String = "Driver Command Queue"

## Every case renders offline, so the renderer is the sole render owner and the
## command order is deterministic: queued work takes effect at the next block.
## Tempo retains synchronous setter semantics pending track/channel ownership.

const BUFFER_SIZE := 256
const MAX_BLOCKS := 2048
# More pushes than the queue holds on top of the commands a case queues first.
const OVERFLOW_PUSHES := 4096


func run(scene_tree: SceneTree) -> void:
	await _run_case(scene_tree, "note start", _case_note_start)
	await _run_case(scene_tree, "overflow keeps queued work", _case_overflow)
	await _run_case(scene_tree, "retirement", _case_retirement)
	await _run_case(scene_tree, "reset with sounding and queued notes", _case_reset.bind(true))
	await _run_case(scene_tree, "reset retaining effect streams", _case_reset.bind(false))
	await _run_case(scene_tree, "synchronous tempo", _case_tempo)
	await _run_case(scene_tree, "track mute", _case_mute)


func _run_case(scene_tree: SceneTree, label: String, body: Callable) -> void:
	var driver := SiONDriver.create(BUFFER_SIZE)
	if not _assert_not_null("%s: driver create" % label, driver):
		return
	scene_tree.root.add_child(driver)
	await scene_tree.process_frame

	driver.stream_without_output()
	var renderer := SiONOfflineRenderer.new()
	_assert_equal("%s: renderer begin" % label, renderer.begin(driver), true)

	body.call(label, driver, renderer)

	if renderer.is_active():
		renderer.finish()
	driver.stop()
	await scene_tree.process_frame
	driver.get_parent().remove_child(driver)
	driver.free()


func _make_voice() -> SiONVoice:
	var voice := SiONVoice.create()
	voice.set_envelope(63, 0, 0, 63, 0, 0)
	return voice


func _start_note(driver: SiONDriver, track_id: int) -> SiMMLTrack:
	var track: SiMMLTrack = driver.create_user_controllable_track(track_id)
	var instance_id := track.get_instance_id()
	_make_voice().update_track_voice(track)
	driver.mailbox_key_on(track_id, 60, 0, -1, -1, instance_id)
	return track


func _peak(block: PackedFloat32Array) -> float:
	var peak := 0.0
	for sample in block:
		peak = maxf(peak, absf(sample))
	return peak


func _render_peak(renderer: SiONOfflineRenderer, block_count: int) -> float:
	var peak := 0.0
	for i in range(block_count):
		peak = maxf(peak, _peak(renderer.render_block()))
	return peak


## Renders until [param condition] holds; returns the blocks it took, or -1.
func _render_until(renderer: SiONOfflineRenderer, condition: Callable) -> int:
	for i in range(MAX_BLOCKS):
		if condition.call():
			return i
		renderer.render_block()
	return MAX_BLOCKS if condition.call() else -1


func _case_note_start(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	# Synchronous setup precedes the queued key-on, so the first block sounds.
	_start_note(driver, 1)
	_assert_equal("%s: first block sounds" % label, _peak(renderer.render_block()) > 0.0, true)


func _case_overflow(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	# Nothing drains between these pushes, so the ring fills up and the rest wait
	# in the backlog. Nothing is lost or reordered: the key-on queued first
	# applies first, and the key-off queued last still applies.
	var track := _start_note(driver, 1)
	for i in range(OVERFLOW_PUSHES):
		driver.mailbox_set_track_volume(1, 1.0)
	driver.mailbox_key_off(1, true, track.get_instance_id())
	_assert_equal("%s: the earliest commands apply first" % label, _peak(renderer.render_block()) > 0.0, true)
	_assert_equal("%s: the key-on ran" % label, track.is_finished(), false)

	var blocks := _render_until(renderer, func() -> bool: return track.is_finished())
	_assert_equal("%s: a command queued past the ring's capacity applies" % label, blocks >= 0, true)


func _case_retirement(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	var track := _start_note(driver, 1)
	renderer.render_block()
	var instance_id := track.get_instance_id()
	driver.mailbox_retire_track(1, instance_id)
	# The render owner deletes the track, so the condition holds its id, not the object.
	var blocks := _render_until(renderer, func() -> bool: return not is_instance_id_valid(instance_id))
	_assert_equal("%s: a retired track is deleted once silent" % label, blocks >= 0, true)


func _case_reset(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer, reset_effector: bool) -> void:
	# One track is sounding and another has a queued key-on; restart discards old commands.
	var linked := _start_note(driver, 1)
	renderer.render_block()
	var queued := _start_note(driver, 2)
	# The offline caller owns the tracks. Recycled objects must shed disposal state.
	linked.mark_for_disposal()
	queued.mark_for_disposal()
	renderer.finish()
	driver.stop()
	driver.stream_without_output(reset_effector)
	_assert_equal("%s: renderer begin after reset" % label, renderer.begin(driver), true)

	# New tracks may reuse the reclaimed objects; they start a clean lifetime.
	var fresh := _start_note(driver, 1)
	_assert_equal("%s: a new track after reset sounds" % label, _render_peak(renderer, 2) > 0.0, true)
	_assert_equal("%s: a new track after reset is sounding" % label, fresh.is_finished(), false)

	driver.mailbox_key_off(1, true, fresh.get_instance_id())
	var blocks := _render_until(renderer, func() -> bool: return fresh.is_finished())
	_assert_equal("%s: the new track finishes" % label, blocks >= 0, true)
	# No disposal state survived the reset: a finished track that was never
	# retired stays alive.
	renderer.render_block()
	_assert_equal("%s: the new track is not disposed" % label, is_instance_valid(fresh), true)
	_assert_equal("%s: the old linked track object survives as a free track" % label, is_instance_valid(linked), true)


func _case_tempo(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	driver.set_bpm(120)
	_assert_equal("%s: the setter publishes tempo immediately" % label, driver.get_bpm(), 120.0)
	renderer.render_block()
	driver.set_bpm(90)
	_assert_equal("%s: a streaming tempo edit is synchronous" % label, driver.get_bpm(), 90.0)
	renderer.render_block()
	_assert_equal("%s: rendering preserves the edited tempo" % label, driver.get_bpm(), 90.0)


func _case_mute(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	# Mixer mute can precede the first note and must survive voice application.
	driver.mailbox_track_effects_set_mute(1, true)
	_start_note(driver, 1)
	_assert_equal("%s: mute precedes the first sample" % label, _peak(renderer.render_block()), 0.0)
	driver.mailbox_track_effects_set_mute(1, false)
	_assert_equal("%s: unmuting resumes the sounding note" % label, _peak(renderer.render_block()) > 0.0, true)

	# Recycled effect streams start a new lifetime with the default mute state.
	driver.mailbox_track_effects_set_mute(1, true)
	renderer.render_block()
	renderer.finish()
	driver.stop()
	driver.stream_without_output()
	_assert_equal("%s: renderer begin after reset" % label, renderer.begin(driver), true)
	_start_note(driver, 1)
	_assert_equal("%s: reset clears the old stream's mute" % label, _peak(renderer.render_block()) > 0.0, true)
