###################################################
# Part of GDSiON tests                            #
# Copyright (c) 2024 Yuri Sizov and contributors  #
# Provided under MIT                              #
###################################################

extends TestBase

var group: String = "SiONDriver"
var name: String = "Signal Analysis"

## Device displays read frames from a stream's signal taps and analyze them on
## the main thread. The analyzer cases need no driver. The tap cases render
## offline, so a read sees exactly the blocks rendered before it.

const BUFFER_SIZE := 256
const WINDOW := SpectrumAnalyzer.WINDOW_SIZE
const COLUMNS := 301
const FREQ_MIN := 20.0
const FREQ_MAX := 20000.0
# A sine centered on an FFT bin, so window scalloping leaves its level alone.
const SINE_BIN := 85
# SignalTap::LEASE_FRAMES, in blocks.
const LEASE_BLOCKS := 32768 / BUFFER_SIZE
# Halves a constant input (driver-command-queue's main track case).
const HALVE := { "kind": "waveShaper", "args": [0.0, 50.0] }


## A mono live stream on one track, fed one block ahead of the render.
class LiveSource:
	var data: Variant
	var generator: Callable
	var frame: int = 0

	func push(frame_count: int) -> void:
		var pcm := PackedFloat32Array()
		pcm.resize(frame_count)
		for i in range(frame_count):
			pcm[i] = generator.call(frame + i)
		frame += frame_count
		data.push_interleaved_pcm(pcm)


func run(scene_tree: SceneTree) -> void:
	_case_analyzer_calibration()
	_case_analyzer_ballistics()
	await _run_driver_case(scene_tree, "effect output taps", _case_effect_taps)
	await _run_driver_case(scene_tree, "tap lease", _case_tap_lease)
	await _run_driver_case(scene_tree, "tap frame order", _case_frame_order)
	await _run_driver_case(scene_tree, "spectrum of an effect's output", _case_spectrum)


# Analyzer.

func _sine(amplitude: float, cycles: float) -> PackedFloat32Array:
	var frames := PackedFloat32Array()
	frames.resize(WINDOW)
	for i in range(WINDOW):
		frames[i] = amplitude * sin(TAU * cycles * i / WINDOW)
	return frames


func _silence() -> PackedFloat32Array:
	var frames := PackedFloat32Array()
	frames.resize(WINDOW)
	return frames


func _column_at(freq: float) -> int:
	return roundi((COLUMNS - 1) * log(freq / FREQ_MIN) / log(FREQ_MAX / FREQ_MIN))


func _loudest(levels: PackedFloat32Array) -> int:
	var best := 0
	for i in range(levels.size()):
		if levels[i] > levels[best]:
			best = i
	return best


## A delta far beyond both time constants lands on the measured level.
func _settled(analyzer: SpectrumAnalyzer, frames: PackedFloat32Array) -> PackedFloat32Array:
	return analyzer.analyze(frames, 10.0)


func _case_analyzer_calibration() -> void:
	var analyzer := SpectrumAnalyzer.new()
	analyzer.configure(48000.0, FREQ_MIN, FREQ_MAX, COLUMNS)
	var column := _column_at(SINE_BIN * 48000.0 / WINDOW)

	var levels := _settled(analyzer, _sine(1.0, SINE_BIN))
	_assert_equal("calibration: one level per column", levels.size(), COLUMNS)
	_assert_equal("calibration: the sine's column is the loudest", _loudest(levels), column)
	_assert_equal("calibration: a full-scale sine reads 0 dBFS", absf(levels[column]) < 0.01, true)
	_assert_equal("calibration: a decade below is far down", levels[_column_at(99.6)] < -80.0, true)
	_assert_equal("calibration: a decade above is far down", levels[_column_at(9961.0)] < -80.0, true)

	levels = _settled(analyzer, _sine(0.5, SINE_BIN))
	_assert_equal("calibration: half amplitude reads -6 dB", absf(levels[column] + 6.0206) < 0.01, true)

	levels = _settled(analyzer, _silence())
	_assert_equal("calibration: silence reads the floor", levels[column], -150.0)


func _case_analyzer_ballistics() -> void:
	var analyzer := SpectrumAnalyzer.new()
	analyzer.configure(48000.0, FREQ_MIN, FREQ_MAX, COLUMNS)
	var column := _column_at(SINE_BIN * 48000.0 / WINDOW)
	var sine := _sine(1.0, SINE_BIN)

	# One time constant covers 1 - 1/e of the way: attack 20 ms, release 250 ms.
	var rising: float = analyzer.analyze(sine, 0.02)[column]
	_assert_equal("ballistics: the attack rises from the floor", absf(rising - (-150.0 + 150.0 * (1.0 - exp(-1.0)))) < 0.01, true)
	var top: float = _settled(analyzer, sine)[column]
	var falling: float = analyzer.analyze(_silence(), 0.25)[column]
	_assert_equal("ballistics: the release falls toward the floor", absf(falling - (top + (-150.0 - top) * (1.0 - exp(-1.0)))) < 0.01, true)


# Taps.

func _run_driver_case(scene_tree: SceneTree, label: String, body: Callable) -> void:
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


## The key-on starts from an empty ring, so the input arrives after it.
func _start_live_source(driver: SiONDriver, renderer: SiONOfflineRenderer, track_id: int, generator: Callable) -> LiveSource:
	var source := LiveSource.new()
	source.generator = generator
	source.data = SiOPMWaveStreamData.new()
	source.data.configure_live(int(driver.get_sample_rate()), 1, BUFFER_SIZE * 64)
	source.data.activate()
	var track: SiMMLTrack = driver.create_user_controllable_track(track_id)
	var voice := SiONVoice.create()
	voice.set_wave_data(source.data)
	voice.update_track_voice(track)
	driver.mailbox_stream_key_on(track_id, 60, 0, -1, track.get_instance_id())
	renderer.render_block()
	source.push(BUFFER_SIZE * 8)
	return source


func _render(renderer: SiONOfflineRenderer, source: LiveSource, block_count: int) -> void:
	for i in range(block_count):
		source.push(BUFFER_SIZE)
		renderer.render_block()


func _newest(driver: SiONDriver, track_id: int, fx_index: int) -> float:
	return driver.read_track_effect_output(track_id, fx_index, BUFFER_SIZE)[BUFFER_SIZE - 1]


func _case_effect_taps(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	driver.mailbox_track_effects_set_chain(1, [HALVE, HALVE])
	var source := _start_live_source(driver, renderer, 1, func(_frame: int) -> float: return 0.25)
	_newest(driver, 1, 0)
	_newest(driver, 1, 1)
	_render(renderer, source, 4)

	var block := driver.read_track_effect_output(1, 0, BUFFER_SIZE)
	var first := block[BUFFER_SIZE - 1]
	_assert_equal("%s: a read returns the frames asked for" % label, block.size(), BUFFER_SIZE)
	_assert_equal("%s: the first effect's output is captured" % label, first > 0.0, true)
	_assert_equal("%s: a constant input reads constant" % label, block[0], first)
	_assert_equal("%s: each tap follows its own effect" % label, is_equal_approx(_newest(driver, 1, 1), first * 0.5), true)

	driver.mailbox_track_effects_set_bypass(1, 1, true)
	_render(renderer, source, 2)
	_assert_equal("%s: a bypassed effect's tap reads its input" % label, _newest(driver, 1, 1), _newest(driver, 1, 0))

	driver.mailbox_track_effects_set_chain(SiONDriver.MAIN_TRACK_ID, [HALVE])
	_newest(driver, SiONDriver.MAIN_TRACK_ID, 0)
	_render(renderer, source, 2)
	_assert_equal("%s: main's effects have taps" % label, _newest(driver, SiONDriver.MAIN_TRACK_ID, 0) > 0.0, true)


func _case_tap_lease(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	driver.mailbox_track_effects_set_chain(1, [HALVE])
	var source := _start_live_source(driver, renderer, 1, func(_frame: int) -> float: return 0.25)
	_render(renderer, source, 4)
	_assert_equal("%s: nothing is captured before the first read" % label, _newest(driver, 1, 0), 0.0)
	_render(renderer, source, 2)
	var level := _newest(driver, 1, 0)
	_assert_equal("%s: a read starts the capture" % label, level > 0.0, true)

	# Unread past its lease, the tap stops. The next read starts it from silence.
	_render(renderer, source, LEASE_BLOCKS + 2)
	driver.mailbox_track_effects_set_bypass(1, 0, true)
	_render(renderer, source, 2)
	_assert_equal("%s: an expired tap restarts from silence" % label, _newest(driver, 1, 0), 0.0)
	_render(renderer, source, 1)
	_assert_equal("%s: a renewed tap captures the current signal" % label, is_equal_approx(_newest(driver, 1, 0), level * 2.0), true)


func _case_frame_order(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	driver.mailbox_track_effects_set_chain(1, [HALVE])
	var source := _start_live_source(driver, renderer, 1, func(frame: int) -> float: return frame * 1e-5)
	_newest(driver, 1, 0)
	_render(renderer, source, 4)
	# The stream channel's integer pipe holds a slow ramp for several frames, so
	# order shows as a rise with no step back, across the block seam too.
	var frames := driver.read_track_effect_output(1, 0, BUFFER_SIZE * 2)
	var rising := frames[frames.size() - 1] > frames[0]
	for i in range(1, frames.size()):
		rising = rising and frames[i] >= frames[i - 1]
	_assert_equal("%s: frames read oldest first across blocks" % label, rising, true)


func _case_spectrum(label: String, driver: SiONDriver, renderer: SiONOfflineRenderer) -> void:
	var sample_rate := driver.get_sample_rate()
	var freq := SINE_BIN * sample_rate / WINDOW
	driver.mailbox_track_effects_set_chain(1, [HALVE])
	var source := _start_live_source(driver, renderer, 1, func(frame: int) -> float: return 0.5 * sin(TAU * freq * frame / sample_rate))
	driver.read_track_effect_output(1, 0, WINDOW)
	_render(renderer, source, WINDOW / BUFFER_SIZE + 2)

	var analyzer := SpectrumAnalyzer.new()
	analyzer.configure(sample_rate, FREQ_MIN, FREQ_MAX, COLUMNS)
	var levels := _settled(analyzer, driver.read_track_effect_output(1, 0, WINDOW))
	_assert_equal("%s: the effect's sine is the loudest column" % label, absi(_loudest(levels) - _column_at(freq)) <= 1, true)
	_assert_equal("%s: the sine stands well above the floor" % label, levels[_column_at(freq)] > -30.0, true)
