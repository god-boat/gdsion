/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_line_delay.h"

SiEffectLineDelay::SiEffectLineDelay(int p_sample_rate) {
	_sample_rate = p_sample_rate;
}

SiEffectLineDelay::SiEffectLineDelay(double p_max_delay, int p_sample_rate) {
	_sample_rate = p_sample_rate;
	resize(p_max_delay);
}

void SiEffectLineDelay::resize(double p_max_delay) {
	_max_delay = p_max_delay;
	int length_in_samples = (int)std::ceil(p_max_delay * _sample_rate);
	_delay_buffer.resize(length_in_samples);
	_delay_buffer.fill(0.0);
	_index = 0;
}

void SiEffectLineDelay::write(double p_sample) {
	if (_delay_buffer.size() == 0) {
		return;
	}
	_delay_buffer.write[_index % _delay_buffer.size()] = p_sample;
	_index++;
}

double SiEffectLineDelay::read(double p_delay) const {
	if (_delay_buffer.size() == 0) {
		return 0.0;
	}
	int delay_in_samples = (int)std::ceil(p_delay * _sample_rate);
	int buffer_size = _delay_buffer.size();
	int delay_index = ((_index - delay_in_samples) % buffer_size + buffer_size) % buffer_size;
	return _delay_buffer[delay_index];
}

double SiEffectLineDelay::read() const {
	if (_delay_buffer.size() == 0) {
		return 0.0;
	}
	int buffer_size = _delay_buffer.size();
	int delay_index = ((_index - buffer_size) % buffer_size + buffer_size) % buffer_size;
	return _delay_buffer[delay_index];
}

double SiEffectLineDelay::process(double p_input) {
	write(p_input);
	return read();
}

void SiEffectLineDelay::reset() {
	_delay_buffer.fill(0.0);
	_index = 0;
}
