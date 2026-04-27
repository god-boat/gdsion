// stmlib compatibility shim for the in-tree Braids port.
//
// Provides just the small set of utilities and macros used by Braids' DSP
// (Interpolate824/88, Mix, Crossfade, CLIP, CONSTRAIN, Random, DISALLOW_*).
// Drop-in replacement for the parts of Mutable Instruments' stmlib that
// Braids depends on, so the upstream .cc files compile unchanged.

#ifndef BRAIDS_STMLIB_COMPAT_H_
#define BRAIDS_STMLIB_COMPAT_H_

#include <cstdint>
#include <cstddef>

#ifndef DISALLOW_COPY_AND_ASSIGN
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
	TypeName(const TypeName&) = delete;        \
	void operator=(const TypeName&) = delete
#endif

#ifndef CLIP
#define CLIP(x) \
	if (x < -32768) { x = -32768; } else if (x > 32767) { x = 32767; }
#endif

#ifndef CONSTRAIN
#define CONSTRAIN(var, min, max) \
	if (var < (min)) { var = (min); } else if (var > (max)) { var = (max); }
#endif

namespace stmlib {

inline int16_t Interpolate824(const int16_t* table, uint32_t phase) {
	int16_t a = table[phase >> 24];
	int16_t b = table[(phase >> 24) + 1];
	uint32_t t = (phase >> 8) & 0xffff;
	return a + (static_cast<int32_t>(b - a) * static_cast<int32_t>(t) >> 16);
}

inline uint16_t Interpolate824(const uint16_t* table, uint32_t phase) {
	uint16_t a = table[phase >> 24];
	uint16_t b = table[(phase >> 24) + 1];
	uint32_t t = (phase >> 8) & 0xffff;
	return a + (static_cast<int32_t>(b - a) * static_cast<int32_t>(t) >> 16);
}

inline uint32_t Interpolate824(const uint32_t* table, uint32_t phase) {
	uint32_t a = table[phase >> 24];
	uint32_t b = table[(phase >> 24) + 1];
	uint32_t t = (phase >> 8) & 0xffff;
	return a + (static_cast<int64_t>(b - a) * static_cast<int64_t>(t) >> 16);
}

inline int16_t Interpolate824(const uint8_t* table, uint32_t phase) {
	int32_t a = table[phase >> 24];
	int32_t b = table[(phase >> 24) + 1];
	return (a + ((b - a) * static_cast<int32_t>((phase >> 8) & 0xffff) >> 16)) << 8;
}

inline int16_t Interpolate1022(const int16_t* table, uint32_t phase) {
	int32_t a = table[phase >> 22];
	int32_t b = table[(phase >> 22) + 1];
	return a + ((b - a) * static_cast<int32_t>((phase >> 6) & 0xffff) >> 16);
}

inline int16_t Interpolate88(const int16_t* table, uint16_t index) {
	int16_t a = table[index >> 8];
	int16_t b = table[(index >> 8) + 1];
	return a + (static_cast<int32_t>(b - a) * static_cast<int32_t>(index & 0xff) >> 8);
}

inline uint16_t Interpolate88(const uint16_t* table, uint16_t index) {
	uint16_t a = table[index >> 8];
	uint16_t b = table[(index >> 8) + 1];
	return a + (static_cast<int32_t>(b - a) * static_cast<int32_t>(index & 0xff) >> 8);
}

inline int16_t Mix(int16_t a, int16_t b, uint16_t balance) {
	return (static_cast<int32_t>(a) * (65535 - balance) +
			static_cast<int32_t>(b) * balance) >> 16;
}

inline int16_t Crossfade(
		const int16_t* table_a, const int16_t* table_b,
		uint32_t phase, uint16_t balance) {
	int16_t a = Interpolate824(table_a, phase);
	int16_t b = Interpolate824(table_b, phase);
	return Mix(a, b, balance);
}

inline int16_t Crossfade(
		const uint8_t* table_a, const uint8_t* table_b,
		uint32_t phase, uint16_t balance) {
	int16_t a = Interpolate824(table_a, phase);
	int16_t b = Interpolate824(table_b, phase);
	return Mix(a, b, balance);
}

class Random {
public:
	static inline uint32_t state() { return rng_state_; }
	static inline void Seed(uint32_t seed) { rng_state_ = seed ? seed : 0x12345678u; }
	static inline uint32_t GetWord() {
		// Marsaglia xorshift32.
		uint32_t x = rng_state_;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		rng_state_ = x ? x : 0x12345678u;
		return rng_state_;
	}
	static inline int16_t GetSample() {
		return static_cast<int16_t>(GetWord() >> 16);
	}

private:
	static uint32_t rng_state_;
};

}  // namespace stmlib

#endif  // BRAIDS_STMLIB_COMPAT_H_
