"""Generates gdsion/src/dsp/bar_modes.h: the mode tables of a bar free at both ends.

A uniform bar's bending modes ring at (beta_n L)^2, where beta_n L is the nth
root of cos(x) cosh(x) = 1: ratios 1, 2.756, 5.404, 8.933, ... A strike at a
point along the bar excites mode n by its shape there,

    phi_n(s) = cosh(b s) + cos(b s) - sigma_n (sinh(b s) + sin(b s)),  b = beta_n L,
    sigma_n = (cosh b - cos b) / (sinh b - sin b),

with s running from one end (0) to the other (1). The weights run from the
center of the bar (s = 1/2) to an end (s = 1), over the fundamental's shape at
the center, so a center hit drives the fundamental at 1 and skips every even
mode, which has a node there.

A tuned bar (marimba, vibraphone) is cut thinner under its center. The arch
lowers the first two modes against the rest until they sit at 1 : 4 : 10; the
modes above the third keep the uniform bar's spacing. Its mode shapes are taken
as the uniform bar's.

    python gdsion/tools/talus_dsp/gen_bar_modes.py
"""

import math

import mode_tables

MODE_COUNT = 24
POSITION_POINTS = mode_tables.POSITION_POINTS
TUNED_SECOND = 4.0
TUNED_THIRD = 10.0


def beta(n: int) -> float:
	"""The nth root (from 1) of cos(x) cosh(x) = 1, by Newton from (2n + 1) pi / 2."""
	x = (2 * n + 1) * math.pi / 2
	for _ in range(50):
		# f = cos(x) - 1 / cosh(x) has the same roots and stays finite.
		f = math.cos(x) - 1.0 / math.cosh(x)
		slope = -math.sin(x) + math.tanh(x) / math.cosh(x)
		x -= f / slope
	return x


def shape(b: float, s: float) -> float:
	"""phi(s), with the growing exponentials cancelled by hand: cosh and sinh of b overflow past mode 20."""
	decay = math.exp(-b)
	d = (math.cos(b) - math.sin(b) - decay) / (1.0 - decay * decay - 2.0 * decay * math.sin(b))
	sigma = 1.0 - 2.0 * decay * d
	return math.cos(b * s) - sigma * math.sin(b * s) + math.exp(-b * (1.0 - s)) * d + math.exp(-b * s) * (1.0 - decay * d)


def main() -> None:
	betas = [beta(n) for n in range(1, MODE_COUNT + 1)]
	ratios = [(b / betas[0]) ** 2 for b in betas]
	center = abs(shape(betas[0], 0.5))
	# Both ends of a free bar move alike, twice the shape's unit.
	assert all(abs(abs(shape(b, 1.0)) - 2.0) < 1e-9 and abs(shape(b, 0.0) - 2.0) < 1e-9 for b in betas)

	weights = []
	for index, b in enumerate(betas):
		row = [shape(b, 0.5 + 0.5 * i / (POSITION_POINTS - 1)) / center for i in range(POSITION_POINTS)]
		# The fundamental and every other odd mode start positive at the center.
		sign = -1.0 if row[0] < 0.0 or (index % 2 == 1 and row[1] < 0.0) else 1.0
		row = [sign * w for w in row]
		if index % 2 == 1:
			# The center is a node of every even mode.
			row[0] = 0.0
		weights.append(row)

	tuned = [1.0, TUNED_SECOND, TUNED_THIRD] + [r * TUNED_THIRD / ratios[2] for r in ratios[3:]]
	mode_tables.write_header("bar_modes.h", "gen_bar_modes.py", "BAR_MODE_COUNT", [
		mode_tables.Table(
			"BAR_MODES",
			[
				"The lowest bending modes of a uniform bar free at both ends, in order.",
				"ratio is (beta_n / beta_1)^2, and a strike excites the mode by its shape at",
				"that point, from the center of the bar to an end, over the fundamental's",
				"shape at the center.",
			],
			[(ratio, row, f"mode {i + 1}") for i, (ratio, row) in enumerate(zip(ratios, weights))],
		),
		mode_tables.Table(
			"TUNED_BAR_MODES",
			[
				"A bar cut thinner under its center, as a marimba's or a vibraphone's is: the",
				"arch lowers the first two modes until they sit at 1 : 4 : 10, and the modes",
				"above the third keep the uniform bar's spacing. The shapes are the uniform bar's.",
			],
			[(ratio, row, f"mode {i + 1}") for i, (ratio, row) in enumerate(zip(tuned, weights))],
		),
	])


if __name__ == "__main__":
	main()
