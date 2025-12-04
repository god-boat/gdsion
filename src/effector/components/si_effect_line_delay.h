/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_LINE_DELAY_H
#define SI_EFFECT_LINE_DELAY_H

#include <godot_cpp/templates/vector.hpp>
#include <cmath>

using namespace godot;

// A simple delay line component for use in effects.
// This is a building block class, not a standalone effect.
class SiEffectLineDelay {

	int _sample_rate = 44100;
	int _index = 0;
	double _max_delay = 0.0;
	Vector<double> _delay_buffer;

public:
	void resize(double p_max_delay);
	void write(double p_sample);
	double read(double p_delay) const;
	double read() const;
	double process(double p_input);
	void reset();

	double get_max_delay() const { return _max_delay; }
	int get_sample_rate() const { return _sample_rate; }

	SiEffectLineDelay(int p_sample_rate = 44100);
	SiEffectLineDelay(double p_max_delay, int p_sample_rate = 44100);
	~SiEffectLineDelay() {}
};

#endif // SI_EFFECT_LINE_DELAY_H
