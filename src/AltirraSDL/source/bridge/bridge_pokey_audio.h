// AltirraBridge - POKEY audio-state decoding helpers

#ifndef ALTIRRASDL_BRIDGE_POKEY_AUDIO_H
#define ALTIRRASDL_BRIDGE_POKEY_AUDIO_H

#include <cstdint>

namespace ATBridge {

// Returns the steady-state timer period in system cycles. This mirrors the
// timer math in ATPokeyEmulator::RecomputeTimerPeriod() plus the two-tone
// resynchronization delay shown by the native Altirra audio monitor.
inline constexpr uint32_t GetPokeyTimerPeriodCycles(
	const uint8_t reg[32], uint8_t audctl, uint8_t skctl, int ch)
{
	const bool base15k = (audctl & 0x01) != 0;
	const bool fast1 = (audctl & 0x40) != 0;
	const bool fast3 = (audctl & 0x20) != 0;
	const bool join12 = (audctl & 0x10) != 0;
	const bool join34 = (audctl & 0x08) != 0;
	const bool twoTone = (skctl & 0x08) != 0;
	const bool fastTimer = ch == 0 ? fast1 : ch == 2 ? fast3 : false;
	const bool hiLinkedTimer = ch == 1 ? join12 : ch == 3 ? join34 : false;
	const bool loLinkedTimer = ch == 0 ? join12 : ch == 2 ? join34 : false;

	if (hiLinkedTimer) {
		const int loCh = ch & ~1;
		const bool fastLinkedTimer = ch == 1 ? fast1 : fast3;
		uint32_t period = ((uint32_t)reg[0x00 + ch * 2] << 8)
			+ (uint32_t)reg[0x00 + loCh * 2] + 1;

		if (fastLinkedTimer) {
			// Two-tone mode resynchronizes timers 1+2 two cycles after
			// underflow. It has no corresponding effect on timers 3+4.
			period += twoTone && ch == 1 ? 8 : 6;
		} else if (base15k) {
			period *= 114;
		} else {
			period *= 28;
		}

		return period;
	}

	if (loLinkedTimer) {
		if (fastTimer)
			return 256;
		else if (base15k)
			return 256 * 114;
		else
			return 256 * 28;
	}

	uint32_t period = (uint32_t)reg[0x00 + ch * 2] + 1;

	if (fastTimer) {
		// The normal borrow pipeline is three cycles. Two-tone mode adds
		// two more cycles to fast timer 1 through the resynchronization.
		period += twoTone && ch == 0 ? 5 : 3;
	} else if (base15k) {
		period *= 114;
	} else {
		period *= 28;
	}

	return period;
}

} // namespace ATBridge

#endif
