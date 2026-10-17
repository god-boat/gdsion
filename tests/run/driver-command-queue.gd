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
	await _run_case(scene_tree, "persistent timed clicks", _case_timed_clicks)
	await _run_case(scene_tree, "timed note retrigger", _case_timed_retrigger)
	await _run_case(scene_tree, "overflow keeps queued work", _case_overflow)
	await _run_case(scene_tree, "retirement", _case_retirement)
	await _run_case(scene_tree, "reset with sounding and queued notes", _case_reset.bind(true))
	await _run_case(scene_tree, "reset retaining effect streams", _case_reset.bind(false))
	await _run_case(scene_tree, "synchronous tempo", _case_tempo)
	await _run_case(scene_tree, "track mute", _case_mute)
	await _run_case(scene_tree, "operator count scopes", _case_operator_count_scopes)


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
	voice.get_channel_params().get_operator_params(0).set_attack_rate(63)
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


func _case_timed_clicks(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	driver.set_bpm(120)
	var track := driver.create_user_controllable_track(0xFFFF)
	_make_voice().update_track_voice(track)
	var beat_frames := int(round(driver.get_sample_rate() * 0.5))
	var beat_blocks := ceili(float(beat_frames) / BUFFER_SIZE)
	var click_length := driver.convert_event_length(1.0)
	var click_frames := int(round(float(beat_frames) / 16.0))

	# Keep the same native track through four bars, then eight idle bars.
	for beat in range(17):
		if beat == 16:
			for i in range(beat_blocks * 32):
				renderer.render_block()
		var click_label := "%s: beat %d" % [label, beat]
		driver.mailbox_key_on(0xFFFF, 86 if beat % 4 == 0 else 94, click_length, -1, -1, track.get_instance_id())
		_assert_equal("%s sounds immediately" % click_label, _peak(renderer.render_block()) > 0.0, true)
		var blocks := _render_until(renderer, func() -> bool: return track.is_finished())
		var duration_ok := blocks >= 0 and absi((blocks + 1) * BUFFER_SIZE - click_frames) <= BUFFER_SIZE * 8
		_assert_equal("%s keeps its duration" % click_label, duration_ok, true)
		for i in range(beat_blocks - blocks - 1):
			renderer.render_block()


func _case_timed_retrigger(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	driver.set_bpm(120)
	var track := driver.create_user_controllable_track(1)
	_make_voice().update_track_voice(track)
	driver.mailbox_key_on(1, 60, driver.convert_event_length(16.0), -1, -1, track.get_instance_id())
	renderer.render_block()
	_assert_equal("%s: previous note is still playing" % label, track.is_finished(), false)

	# The short replacement must not inherit the long note's remaining samples.
	driver.mailbox_key_on(1, 72, driver.convert_event_length(1.0), -1, -1, track.get_instance_id())
	_assert_equal("%s: replacement sounds immediately" % label, _peak(renderer.render_block()) > 0.0, true)
	var blocks := _render_until(renderer, func() -> bool: return track.is_finished())
	var expected_frames := int(round(driver.get_sample_rate() * 0.5 / 16.0))
	var duration_ok := blocks >= 0 and absi((blocks + 1) * BUFFER_SIZE - expected_frames) <= BUFFER_SIZE * 8
	_assert_equal("%s: replacement keeps its duration" % label, duration_ok, true)


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


func _case_operator_count_scopes(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	var fm := SiONVoice.create_engine(5, 0)
	fm.get_channel_params().set_operator_count(2)
	var sampler := SiONVoice.create_engine(10, 0)
	var samples := PackedFloat32Array()
	samples.resize(BUFFER_SIZE * 32)
	samples.fill(0.1)
	sampler.set_sampler_voice(samples, true, 1)
	var tracks: Array[SiMMLTrack] = []
	for scope: Array in [[1, 101, 11], [1, 101, 12], [1, 202, 13], [1, 303, 14], [2, 101, 15]]:
		var track := driver.create_user_controllable_track(scope[0])
		track.set_entity_scope_id(scope[1])
		track.set_slot_scope_id(scope[2])
		var voice := sampler if tracks.size() == 3 else fm
		voice.update_track_voice(track)
		driver.mailbox_key_on(scope[0], 60, 0, -1, -1, track.get_instance_id())
		tracks.append(track)
	renderer.render_block()
	driver.mailbox_set_fm_operator_count(1, 3, 0, false, 0, 0, 101)
	renderer.render_block()
	for index in [0, 1]:
		_assert_equal("%s: every note on the selected leaf changes" % label,
			str(tracks[index].get_channel()).contains("ops=3,"), true)
	for index in [2, 4]:
		_assert_equal("%s: sibling leaf and other track stay unchanged" % label,
			str(tracks[index].get_channel()).contains("ops=2,"), true)
	var sampler_channel: Object = tracks[3].get_channel()
	_assert_equal("%s: sampler sibling retains its engine" % label,
		sampler_channel.get_class(), "SiOPMChannelSampler")
	# The public native API also permits track-wide commands. Those select FM
	# channels while preserving non-FM siblings on a mixed-engine track.
	driver.mailbox_set_fm_operator_count(1, 4, 0, false, 0, 0)
	renderer.render_block()
	for index in [0, 1, 2]:
		_assert_equal("%s: explicit broadcast selects FM channels" % label,
			str(tracks[index].get_channel()).contains("ops=4,"), true)
	sampler_channel = tracks[3].get_channel()
	_assert_equal("%s: explicit broadcast excludes the sampler" % label,
		sampler_channel.get_class(), "SiOPMChannelSampler")
	_assert_equal("%s: explicit broadcast stays on its logical track" % label,
		str(tracks[4].get_channel()).contains("ops=2,"), true)


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
