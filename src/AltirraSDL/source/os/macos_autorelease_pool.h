//	AltirraSDL - scoped macOS autorelease pool

#pragma once

class ATMacAutoreleasePool {
public:
#ifdef __APPLE__
	ATMacAutoreleasePool();
	~ATMacAutoreleasePool();

	ATMacAutoreleasePool(const ATMacAutoreleasePool&) = delete;
	ATMacAutoreleasePool& operator=(const ATMacAutoreleasePool&) = delete;

private:
	void *mpPool = nullptr;
#else
	ATMacAutoreleasePool() = default;
#endif
};
