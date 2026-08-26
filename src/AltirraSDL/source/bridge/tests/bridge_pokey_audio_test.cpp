// AltirraBridge - POKEY audio-state timing regression tests

#include "bridge_pokey_audio.h"

#include <cstdio>

namespace {

int gFailures = 0;

void CheckPeriod(const uint8_t reg[32], uint8_t audctl, uint8_t skctl,
	int channel, uint32_t expected, const char *description)
{
	const uint32_t actual = ATBridge::GetPokeyTimerPeriodCycles(
		reg, audctl, skctl, channel);

	if (actual != expected) {
		std::fprintf(stderr, "FAIL: %s: expected %u cycles, got %u\n",
			description, expected, actual);
		++gFailures;
	}
}

} // namespace

int main() {
	uint8_t reg[32] {};

	reg[0] = 0x20;
	CheckPeriod(reg, 0x40, 0x03, 0, 0x20 + 4,
		"fast channel 1 normal borrow");
	CheckPeriod(reg, 0x40, 0x8b, 0, 0x20 + 6,
		"fast channel 1 two-tone resynchronization");

	CheckPeriod(reg, 0x00, 0x8b, 0, (0x20 + 1) * 28,
		"slow channel 1 is unchanged by two-tone mode");

	reg[0] = 0x34;
	reg[2] = 0x12;
	CheckPeriod(reg, 0x50, 0x03, 1, 0x1234 + 7,
		"fast joined channels 1+2 normal borrow");
	CheckPeriod(reg, 0x50, 0x8b, 1, 0x1234 + 9,
		"fast joined channels 1+2 two-tone resynchronization");
	CheckPeriod(reg, 0x50, 0x8b, 0, 256,
		"fast joined channels 1+2 low timer");

	reg[4] = 0x20;
	CheckPeriod(reg, 0x20, 0x8b, 2, 0x20 + 4,
		"fast channel 3 is unaffected by two-tone mode");

	reg[4] = 0x34;
	reg[6] = 0x12;
	CheckPeriod(reg, 0x28, 0x8b, 3, 0x1234 + 7,
		"fast joined channels 3+4 are unaffected by two-tone mode");

	if (gFailures) {
		std::fprintf(stderr, "%d bridge POKEY audio timing test(s) failed\n",
			gFailures);
		return 1;
	}

	std::puts("Bridge POKEY audio timing tests passed");
	return 0;
}
