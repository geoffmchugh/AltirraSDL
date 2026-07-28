//	Altirra - Atari 800/800XL/5200 emulator
//	Copyright (C) 2026 Avery Lee
//
//	This program is free software; you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation; either version 2 of the License, or
//	(at your option) any later version.
//
//	This program is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License along
//	with this program. If not, see <http://www.gnu.org/licenses/>.

#ifndef f_AT_PRINTERFONTFX80_H
#define f_AT_PRINTERFONTFX80_H

class ATPrinterFontFX80 {
public:
	consteval ATPrinterFontFX80();

	consteval void Write(
		int offset,
		const char *src0,
		const char *src1,
		const char *src2,
		const char *src3,
		const char *src4,
		const char *src5,
		const char *src6,
		const char *src7,
		const char *src8
	);

	uint16 mFont[256][12] {};

	// start/stop columns (inclusive) for proportional mode
	uint8 mPropStartStop[256][2] {};
};

extern const ATPrinterFontFX80 g_ATPrinterFontFX80;

#endif
