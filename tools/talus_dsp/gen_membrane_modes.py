"""Generates gdsion/src/dsp/membrane_modes.h: the circular-membrane mode table.

Each mode (m, n) of an ideal membrane fixed at its rim rings at the Bessel
zero j_mn. Its ratio is j_mn / j_01, and a strike at radius rho (0 center,
1 rim) excites it in proportion to J_m(j_mn * rho). The header lists the
lowest MODE_COUNT modes by ratio, each with its weights over rho from 0 to 1.

numpy only: J_m comes from Bessel's integral, which the trapezoid rule
integrates to machine precision because the integrand is smooth and periodic.

    python gdsion/tools/talus_dsp/gen_membrane_modes.py
"""

import math

import numpy as np

import mode_tables

MODE_COUNT = 48
POSITION_POINTS = mode_tables.POSITION_POINTS
M_MAX = 30
N_MAX = 12

_TAU = np.linspace(0.0, math.pi, 4097)


def bessel_j(m: int, x: float) -> float:
	"""J_m(x) = (1/pi) * integral over [0, pi] of cos(m t - x sin t) dt."""
	values = np.cos(m * _TAU - x * np.sin(_TAU))
	step = _TAU[1] - _TAU[0]
	return float((values.sum() - 0.5 * (values[0] + values[-1])) * step / math.pi)


def bessel_zeros(m: int, count: int) -> list[float]:
	zeros = []
	x = 0.5 if m == 0 else float(m)
	previous = bessel_j(m, x)
	while len(zeros) < count:
		x_next = x + 0.05
		current = bessel_j(m, x_next)
		if previous == 0.0 or previous * current < 0.0:
			low, high = x, x_next
			for _ in range(80):
				middle = 0.5 * (low + high)
				if bessel_j(m, low) * bessel_j(m, middle) <= 0.0:
					high = middle
				else:
					low = middle
			zeros.append(0.5 * (low + high))
		x, previous = x_next, current
	return zeros


def membrane_modes() -> list[tuple[float, int, int, float]]:
	modes = []
	for m in range(M_MAX + 1):
		for n, zero in enumerate(bessel_zeros(m, N_MAX), start=1):
			modes.append((zero, m, n))
	modes.sort()
	base = modes[0][0]
	return [(zero / base, m, n, zero) for zero, m, n in modes[:MODE_COUNT]]


def main() -> None:
	modes = membrane_modes()
	# Every mode in the table must come before any mode the search range left out.
	assert modes[-1][3] < bessel_zeros(M_MAX + 1, 1)[0]
	assert modes[-1][3] < bessel_zeros(0, N_MAX)[-1]

	rows = []
	for ratio, m, n, zero in modes:
		weights = [bessel_j(m, zero * i / (POSITION_POINTS - 1)) for i in range(POSITION_POINTS)]
		# The rim is a node of every mode.
		weights[-1] = 0.0
		rows.append((ratio, weights, f"({m},{n})"))
	mode_tables.write_header("membrane_modes.h", "gen_membrane_modes.py", "MEMBRANE_MODE_COUNT", [mode_tables.Table(
		"MEMBRANE_MODES",
		[
			"The lowest modes (m,n) of an ideal circular membrane fixed at its rim, in order",
			"of ratio. ratio is j_mn / j_01, and a strike at radius rho (0 center, 1 rim)",
			"excites the mode by J_m(j_mn * rho).",
		],
		rows,
	)])


if __name__ == "__main__":
	main()
