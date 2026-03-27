extends TestBase

var group: String = "SiONDriver"
var name: String = "Driver Sequence BPM Timing"

const BUFFER_SIZE := 64
const EXPECTED_NOTE_SECONDS := 0.5
const MAX_BLOCKS := 4096


func run(scene_tree: SceneTree) -> void:
	await _assert_sequence_duration(scene_tree, "tempo command", "t120 l4 c;", false)
	await _assert_sequence_duration(scene_tree, "set_bpm", "l4 c;", true)


func _assert_sequence_duration(scene_tree: SceneTree, label: String, mml: String, override_bpm: bool) -> void:
	var driver := SiONDriver.create(BUFFER_SIZE)
	_assert_not_null("%s: driver create" % label, driver)
	if driver == null:
		return

	scene_tree.root.add_child(driver)
	await scene_tree.process_frame

	var data: SiONData = driver.compile(mml)
	_assert_not_null("%s: compile" % label, data)
	if data == null:
		await _cleanup_driver(scene_tree, driver)
		return

	if override_bpm:
		data.set_bpm(120)

	driver.stream()
	await scene_tree.process_frame

	var voice := SiONVoice.create()
	_assert_not_null("%s: voice create" % label, voice)
	if voice == null:
		await _cleanup_driver(scene_tree, driver)
		return
	voice.set_envelope(63, 0, 0, 63, 0, 0)

	var tracks := driver.sequence_on(data, voice, 0, 0, 1, 0, false)
	_assert_equal("%s: track count" % label, tracks.size(), 1)
	if tracks.size() != 1:
		await _cleanup_driver(scene_tree, driver)
		return

	var track: SiMMLTrack = tracks[0]
	_assert_not_null("%s: track create" % label, track)
	if track == null:
		await _cleanup_driver(scene_tree, driver)
		return

	var renderer := SiONOfflineRenderer.new()
	var began := renderer.begin(driver)
	_assert_equal("%s: renderer begin" % label, began, true)
	if not began:
		await _cleanup_driver(scene_tree, driver, renderer)
		return

	var block_count := 0
	while block_count < MAX_BLOCKS and is_instance_valid(track) and not track.is_finished():
		renderer.render_block()
		block_count += 1

	var finished := is_instance_valid(track) and track.is_finished()
	_assert_equal("%s: track finished" % label, finished, true)
	if not finished:
		_append_extra_to_output(
			"label=%s blocks=%d sample_rate=%d buffer=%d" % [
				label,
				block_count,
				driver.get_sample_rate(),
				renderer.get_block_size_frames(),
			]
		)
		await _cleanup_driver(scene_tree, driver, renderer)
		return

	var rendered_frames := renderer.get_total_frames_rendered()
	var expected_frames := int(round(driver.get_sample_rate() * EXPECTED_NOTE_SECONDS))
	var tolerance := maxi(renderer.get_block_size_frames() * 8, 512)
	var duration_ok := absi(rendered_frames - expected_frames) <= tolerance
	_assert_equal("%s: rendered duration" % label, duration_ok, true)
	if not duration_ok:
		_append_extra_to_output(
			"label=%s sample_rate=%d expected_frames=%d rendered_frames=%d tolerance=%d blocks=%d" % [
				label,
				driver.get_sample_rate(),
				expected_frames,
				rendered_frames,
				tolerance,
				block_count,
			]
		)

	await _cleanup_driver(scene_tree, driver, renderer)


func _cleanup_driver(scene_tree: SceneTree, driver: SiONDriver, renderer: SiONOfflineRenderer = null) -> void:
	if renderer != null and renderer.is_active():
		renderer.finish()

	if driver == null:
		return

	driver.stop()
	await scene_tree.process_frame

	if driver.get_parent() != null:
		driver.get_parent().remove_child(driver)
	driver.free()
