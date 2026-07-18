/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SION_DENORMALS_H
#define SION_DENORMALS_H

// Flush-to-zero / denormals-are-zero control for the audio render thread.
//
// Denormal floats are the numbers between zero and the smallest normal value.
// Hardware handles them via microcode traps rather than the normal pipeline, and an
// operation on one can cost 100x a normal operation. Audio DSP walks into them
// constantly: anything with an exponentially decaying feedback path -- reverb tails,
// filter states, envelope releases -- approaches zero asymptotically without ever
// reaching it, so once the signal goes quiet the state sits in denormal range and
// stays there. The pathological consequence is that a silent effect costs *more* than
// a loud one.
//
// Enabling FTZ (results that would be denormal are written as zero) and DAZ
// (denormal inputs are read as zero) makes the hardware take the fast path. It is
// standard practice in audio and inaudible: the affected values are below -700 dBFS.
//
// These flags live in per-thread CPU control registers (MXCSR on x86, FPCR on ARM),
// so they must be set on the thread that runs the DSP, not once at startup.

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define SION_DENORMALS_X86 1
#include <pmmintrin.h>
#include <xmmintrin.h>
#elif (defined(_M_ARM64) || defined(_M_ARM64EC)) && defined(_MSC_VER)
#define SION_DENORMALS_ARM64_MSVC 1
#include <arm64intr.h>
#include <intrin.h>
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define SION_DENORMALS_ARM64_GNU 1
#include <stdint.h>
#endif

namespace sion {

// Sets flush-to-zero on the calling thread. Cheap enough (a control-register write)
// to call once per audio block rather than tracking thread identity.
static inline void set_denormals_flushed(bool p_enabled) {
#if defined(SION_DENORMALS_X86)
	_MM_SET_FLUSH_ZERO_MODE(p_enabled ? _MM_FLUSH_ZERO_ON : _MM_FLUSH_ZERO_OFF);
	// DAZ needs SSE3. Every x86_64 CPU that can run this build has it.
	_MM_SET_DENORMALS_ZERO_MODE(p_enabled ? _MM_DENORMALS_ZERO_ON : _MM_DENORMALS_ZERO_OFF);
#elif defined(SION_DENORMALS_ARM64_MSVC)
	// FPCR bit 24 (FZ) covers both directions on AArch64; there is no separate DAZ.
	unsigned __int64 fpcr = static_cast<unsigned __int64>(_ReadStatusReg(ARM64_FPCR));
	if (p_enabled) {
		fpcr |= (1ULL << 24);
	} else {
		fpcr &= ~(1ULL << 24);
	}
	_WriteStatusReg(ARM64_FPCR, static_cast<__int64>(fpcr));
#elif defined(SION_DENORMALS_ARM64_GNU)
	// FPCR bit 24 (FZ) covers both directions on AArch64; there is no separate DAZ.
	uint64_t fpcr;
	__asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
	if (p_enabled) {
		fpcr |= (1ULL << 24);
	} else {
		fpcr &= ~(1ULL << 24);
	}
	__asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#else
	(void)p_enabled;
#endif
}

} // namespace sion

#endif // SION_DENORMALS_H
