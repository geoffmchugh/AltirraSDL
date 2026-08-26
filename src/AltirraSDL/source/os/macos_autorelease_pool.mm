//	AltirraSDL - scoped macOS autorelease pool implementation

#include "macos_autorelease_pool.h"

#import <Foundation/Foundation.h>

ATMacAutoreleasePool::ATMacAutoreleasePool()
	: mpPool([[NSAutoreleasePool alloc] init])
{
}

ATMacAutoreleasePool::~ATMacAutoreleasePool() {
	[(NSAutoreleasePool *)mpPool drain];
}
