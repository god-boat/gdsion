"""Iron's handling noise: Scrape, Pick Bend and Fret Noise, measured on the
stub-compiled C++ channel. Prints one PASS/FAIL line per case, then a summary.

    python gdsion/tools/talus_dsp/build.py
    python gdsion/tools/talus_dsp/iron_tests.py [--case I1 ...]
"""

import argparse
import math
import sys
import time

import numpy as np

import harness as h
import tests as t

RATE = 48000
E2 = 40
SCRAPE_SECONDS = 0.012
LN1000 = math.log(1000)


def voice(**overrides: int) -> list[int]:
	"""The default voice by lowercase slot name, with Humanize and Timing at 0: the pluck
	lands exactly when the pick's contact ends, and every pick is the same."""
	values = list(h.IRON_DEFAULTS)
	for name, value in {"humanize": 0, "timing_ms": 0, **overrides}.items():
		values[h.IRON_SLOTS.index(name.upper())] = int(value)
	return values


def clean(**overrides: int) -> list[int]:
	"""A voice whose partials stay harmonic and readable: no stiffness, no cabinet, little gain."""
	return voice(**{"gain": 60, "tight_hz": 20, "cabinet": 3, "stiffness": 0, "pick_scrape": 0, "fret_noise": 0, **overrides})


def contact_samples(scrape: int, velocity: float, rate: int = RATE) -> int:
	return int(SCRAPE_SECONDS * scrape * 0.01 * (1.3 - 0.6 * velocity) * rate)


def rms(y: np.ndarray) -> float:
	return float(np.sqrt(np.mean(np.square(y)))) if len(y) else 0.0


def db(ratio: float) -> float:
	return 20 * math.log10(max(ratio, 1e-12))


def hit(params: list[int], note: float = E2, velocity: float = 1.0, seconds: float = 0.3, rate: int = RATE) -> h.Job:
	return h.Job("iron", rate=rate, params=params).hit(note, velocity).render(seconds)


# ---------------------------------------------------------------------------
# Scrape
# ---------------------------------------------------------------------------

def i1_scrape_lead():
	"""The pick drags for its contact time, then the pluck lands: at Scrape 0 at once, at 100 after 8.4 ms (v = 1).

	The drag grows with Scrape and stays under the note it leads in.
	"""
	scrapes = (0, 25, 50, 100)
	renders = h.run([hit(voice(pick_scrape=s, fret_noise=0)) for s in scrapes])
	attack = renders[0][:RATE // 20]
	offsets, leads = [], []
	for scrape, y in zip(scrapes, renders):
		contact = contact_samples(scrape, 1.0)
		# Where this render's attack lines up best with the attack at Scrape 0.
		lags = range(-RATE // 500, RATE // 500 + 1)
		offsets.append(max(lags, key=lambda lag: float(np.dot(attack, y[contact + lag:contact + lag + len(attack)])) if contact + lag >= 0 else -math.inf))
		leads.append(db(rms(y[:contact]) / rms(y[contact:contact + RATE // 10])) if contact else -math.inf)
	passed = max(abs(o) for o in offsets) <= 1 and leads[0] == -math.inf and leads[1] < leads[2] < leads[3] < 0 and leads[2] > -30
	return passed, "lead re the note " + ", ".join(f"{s} %: {lead:.1f} dB" for s, lead in zip(scrapes[1:], leads[1:])) + f"; pluck within {max(abs(o) for o in offsets)} samples of the contact's end"


def i2_scrape_chirp():
	"""The drag rings the string between the pick and the bridge: a comb at f0 / position.

	Pick Pos 0 is 4 % of the string, so E2 chirps at 2060 Hz and its multiples, at least 6 dB
	over the troughs between them. Moving the pick to 13 % moves the comb, and those peaks go.
	"""
	velocity = 0.3
	contact = contact_samples(100, velocity)
	near, far = (y[:contact] for y in h.run([hit(clean(pick_scrape=100, pick_position=p, gain=40), velocity=velocity) for p in (0, 30)]))
	segment = t.note_hz(E2) / 0.04
	def comb(y):
		level = lambda hz: t.mode_level(y, RATE, hz, start=0.0, length=contact / RATE)
		return db(np.mean([level(k * segment) for k in (1, 2, 3)]) / np.mean([level((k + 0.5) * segment) for k in (1, 2, 3)]))
	passed = comb(near) >= 6 and comb(far) < 3
	return passed, f"peaks over troughs at {segment:.0f} Hz: {comb(near):+.1f} dB with the pick at 4 %, {comb(far):+.1f} dB at 13 %"


def i3_scrape_windings():
	"""The plain strings give the pick little to drag over: E5 scrapes at least 10 dB under E2."""
	contact = contact_samples(100, 0.8)
	low, high = (y[:contact] for y in h.run([hit(voice(pick_scrape=100, fret_noise=0), note=n, velocity=0.8) for n in (E2, 76)]))
	drop = db(rms(high) / rms(low))
	return drop <= -10, f"E5 against E2: {drop:.1f} dB"


# ---------------------------------------------------------------------------
# Pick Bend
# ---------------------------------------------------------------------------

def bend_cents(y: np.ndarray, start: float, expected: float = 0.0) -> float:
	"""Cents over E2 across 100 ms, read from the third partial near where it is expected."""
	partial = 3 * t.note_hz(E2)
	return t.cents(t.frequency(y, RATE, partial * 2 ** (expected / 1200), start=start, length=0.1), partial)


def expected_bend(cents: float, velocity: float, start: float, sustain: float = 4.0) -> float:
	"""The mean over 100 ms of a bend that follows the string's energy: the square of a 60 dB decay over Sustain."""
	tau = sustain / (2 * LN1000)
	return cents * velocity ** 2 * tau / 0.1 * (math.exp(-start / tau) - math.exp(-(start + 0.1) / tau))


def i4_pick_bend():
	"""A full-velocity pick starts Pick Bend cents sharp and settles as the string's energy does.

	Half the velocity bends a quarter as far. Read against the same pick at Pick Bend 0, within
	1 cent or 6 %: the channel bends the period to first order, 3 cents over at the full 100.
	"""
	cases = [(20, 1.0), (20, 0.5), (100, 1.0), (100, 0.5)]
	flat = dict(zip((1.0, 0.5), h.run([hit(clean(pick_bend_cents=0), velocity=v, seconds=1.0) for v in (1.0, 0.5)])))
	renders = h.run([hit(clean(pick_bend_cents=cents), velocity=v, seconds=1.0) for cents, v in cases])
	passed, details = True, []
	for (cents, velocity), y in zip(cases, renders):
		for start in (0.02, 0.3):
			expected = expected_bend(cents, velocity, start)
			measured = bend_cents(y, start, expected) - bend_cents(flat[velocity], start)
			passed = passed and abs(measured - expected) <= max(1.0, 0.06 * expected)
			details.append(f"{cents} ct v {velocity} at {1000 * start:.0f} ms {measured:+.1f} ({expected:+.1f})")
	return passed, "; ".join(details)


def i5_bend_settles():
	"""The bend leaves the tuning alone: a full-velocity pick at Pick Bend 100 rings within 1 cent of pitch after 2 s."""
	y = h.run([hit(clean(pick_bend_cents=100, gain=80), seconds=2.5)])[0]
	settled = bend_cents(y, 2.0)
	return abs(settled) <= 1, f"{settled:+.2f} cents"


# ---------------------------------------------------------------------------
# Fret Noise
# ---------------------------------------------------------------------------

def i6_fret_rattle():
	"""A finger lifting or a slur landing buzzes the string on the fret, for about 20 ms, and a bend does not.

	Against the same render at Fret Noise 0: the buzz is there in the first 10 ms after a release
	and after a two-semitone slur, 25 dB down by 30 ms, and absent after a quarter-tone bend.
	"""
	def play(fret, then):
		return then(h.Job("iron", params=voice(fret_noise=fret, pick_scrape=0)).hit(45, 0.9).render(0.25)).render(0.1)
	moves = {"release": lambda job: job.off(), "slur": lambda job: job.pitch(47), "bend": lambda job: job.pitch(45.5)}
	renders = h.run([play(fret, then) for then in moves.values() for fret in (0, 100)])
	at = int(0.25 * RATE)
	window = RATE // 100
	levels = {}
	for i, name in enumerate(moves):
		quiet, noisy = renders[2 * i], renders[2 * i + 1]
		body = rms(quiet[at - RATE // 10:at])
		buzz = noisy - quiet
		levels[name] = (db(rms(buzz[at:at + window]) / body), db(rms(buzz[at + 3 * window:at + 4 * window]) / body), rms(buzz[:at]))
	passed = all(levels[name][0] > -20 and levels[name][1] < levels[name][0] - 25 and levels[name][2] == 0 for name in ("release", "slur")) and levels["bend"][0] == db(0)
	return passed, ", ".join(f"{name} {first:.1f} dB then {later:.1f} dB" for name, (first, later, _) in levels.items())


def i13_repick():
	"""A new pick ends the buzz of the release before it: only a dragging hand still sounds over the new note.

	Notes picked again 4 ms after their release, against Fret Noise 0 with nothing else random.
	From 5 to 25 ms after the new pluck, the releases that lifted clean (about half) leave the
	note within -40 dB of the same note without fret noise.
	"""
	def play(fret):
		job = h.Job("iron", params=voice(fret_noise=fret, pick_scrape=0, pick_attack=0))
		for _ in range(30):
			job.hit(45, 0.9).render(0.2).off().render(0.004).hit(45, 0.9).render(0.2).off().render(0.4)
		return job
	quiet, noisy = h.run([play(0), play(100)])
	step = int(0.2 * RATE) + int(0.004 * RATE) + int(0.2 * RATE) + int(0.4 * RATE)
	# The ringing string takes 3 ms to fade before the new pluck replaces it.
	pluck = int(0.2 * RATE) + int(0.004 * RATE) + int(0.003 * RATE)
	residues = []
	for i in range(30):
		window = slice(i * step + pluck + RATE // 200, i * step + pluck + RATE // 40)
		residues.append(db(rms((noisy - quiet)[window]) / rms(quiet[window])))
	clean = sum(residue < -40 for residue in residues)
	return clean >= 8, f"{clean} of 30 new notes within -40 dB, the cleanest {min(residues):.0f} dB, the noisiest {max(residues):.0f} dB"


def i7_finger_drags():
	"""Half the releases lift clean, and about one in ten drags far enough to squeak. Fret Noise 0 has none.

	Over 80 releases at Fret Noise 100, against the same render at 0, past the 40 ms the fret
	buzz takes: a drag is anything left there. Between 4 and 16 run past 40 ms, none past 160 ms.
	"""
	def play(fret):
		job = h.Job("iron", params=voice(fret_noise=fret, pick_scrape=0, pick_attack=0))
		for _ in range(80):
			job.hit(45, 0.9).render(0.1).off().render(0.3)
		return job
	quiet, noisy, silent = h.run([play(0), play(100), play(0)])
	step = int(0.4 * RATE)
	lengths = []
	for i in range(80):
		drag = (noisy - quiet)[i * step + int(0.14 * RATE):(i + 1) * step]
		loud = np.nonzero(np.abs(drag) > 2)[0]
		lengths.append(0.04 + loud[-1] / RATE if len(loud) else 0.0)
	squeaks = sum(length > 0.04 for length in lengths)
	passed = 4 <= squeaks <= 16 and max(lengths) <= 0.16 and np.array_equal(quiet, silent)
	return passed, f"{squeaks} of 80 releases drag past 40 ms, the longest {1000 * max(lengths):.0f} ms"


# ---------------------------------------------------------------------------
# The channel contract
# ---------------------------------------------------------------------------

def i8_idle():
	"""The channel idles within 250 ms of every release, squeaks included, at 44.1 and 48 kHz."""
	jobs = []
	for rate in (44100, 48000):
		job = h.Job("iron", rate=rate, params=list(h.IRON_DEFAULTS[:-1]) + [100])
		for _ in range(24):
			job.hit(45, 0.9).render(0.15).off().until_idle(2.0)
		jobs.append(job)
	idles = [int(v) for v in h.report_values(h.run_with_report(jobs)[1], "IDLE")]
	seconds = [samples / rate for samples, rate in zip(idles, [44100] * 24 + [48000] * 24)]
	passed = min(idles) > 0 and max(seconds) <= 0.25
	return passed, f"idle {1000 * min(seconds):.0f} to {1000 * max(seconds):.0f} ms after the release"


def i9_segments():
	"""Odd segment lengths render the same samples as one block, drags and squeaks included."""
	def play(blocks):
		job = h.Job("iron", params=list(h.IRON_DEFAULTS[:-1]) + [100])
		for _ in range(6):
			job.hit(45, 0.9).render(0.1, blocks=blocks).off().render(0.2, blocks=blocks)
		return job
	whole, split = h.run([play(None), play([1, 7, 33, 2, 191, 64, 5] * 400)])
	return np.array_equal(whole, split), f"{len(whole)} samples"


def i10_determinism():
	"""Two renders from initialize are bit-identical, every random draw included.

	At full Gain, Scrape and Fret Noise, where anything the first render left behind is loudest.
	"""
	def play():
		job = h.Job("iron", params=voice(humanize=35, timing_ms=4, gain=100, pick_scrape=100, fret_noise=100))
		for note in (40, 45, 47, 52) * 6:
			job.hit(note, 0.9).render(0.15).off().render(0.2)
		return job
	a, b = h.run([play(), play()])
	return np.array_equal(a, b), f"{len(a)} samples"


def i12_release_after_ringing_out():
	"""A note released after its strings rang out still drags, then and not on the next pick.

	Palm-muted notes held 0.6 s leave the channel idle before the release. Against Fret Noise 0,
	and with nothing else random in the voice: the drags sound in the gaps after the releases,
	and every held note is untouched. Rendered in short segments, since the channel only idles
	on a silent one, down to the 3 samples a drag's silent start has to survive.
	"""
	def play(fret):
		job = h.Job("iron", params=voice(fret_noise=fret, pick_scrape=0, pick_attack=0, palm_mute=100))
		for _ in range(40):
			job.hit(45, 0.9).render(0.6, blocks=[64] * 450).off().render(0.3, blocks=[3] * 4800)
		return job
	quiet, noisy = h.run([play(0), play(100)])
	held, step = int(0.6 * RATE), int(0.9 * RATE)
	drags = sum(bool(np.any((noisy != quiet)[i * step + held:(i + 1) * step])) for i in range(40))
	leaks = sum(bool(np.any((noisy != quiet)[i * step:i * step + held])) for i in range(40))
	return drags >= 8 and leaks == 0, f"{drags} of 40 releases drag, {leaks} held notes disturbed"


def i11_level():
	"""A default full-velocity note still peaks within 1.5 dB of -7 dBFS, at 44.1 and 48 kHz."""
	peaks = [db(np.abs(y).max() / h.PIPE_PEAK) for y in h.run([hit(list(h.IRON_DEFAULTS), seconds=0.5, rate=rate) for rate in (44100, 48000)])]
	return all(abs(p + 7) <= 1.5 for p in peaks), ", ".join(f"{p:.2f} dBFS" for p in peaks)


CASES = {
	"I1": i1_scrape_lead,
	"I2": i2_scrape_chirp,
	"I3": i3_scrape_windings,
	"I4": i4_pick_bend,
	"I5": i5_bend_settles,
	"I6": i6_fret_rattle,
	"I7": i7_finger_drags,
	"I8": i8_idle,
	"I9": i9_segments,
	"I10": i10_determinism,
	"I11": i11_level,
	"I12": i12_release_after_ringing_out,
	"I13": i13_repick,
}


def main() -> None:
	parser = argparse.ArgumentParser()
	parser.add_argument("--case", nargs="*", default=list(CASES))
	args = parser.parse_args()
	failures = 0
	for name in args.case:
		start = time.time()
		try:
			passed, detail = CASES[name]()
		except Exception as error:  # A measurement that cannot run is a failure, reported as such.
			passed, detail = False, f"{type(error).__name__}: {error}"
		failures += not passed
		print(f"{'PASS' if passed else 'FAIL'} {name:4s} {detail} ({time.time() - start:.1f} s)", flush=True)
	print(f"{len(args.case) - failures}/{len(args.case)} passed")
	sys.exit(1 if failures else 0)


if __name__ == "__main__":
	main()
