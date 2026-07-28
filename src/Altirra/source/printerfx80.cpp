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

#include <stdafx.h>
#include <at/atcore/propertyset.h>
#include "printerfx80.h"
#include "printerfontfx80.h"

void ATCreateDevicePrinterFX80(const ATPropertySet& pset, IATDevice **dev) {
	vdrefptr<ATDevicePrinterFX80> p(new ATDevicePrinterFX80);

	*dev = p.release();
}

extern const ATDeviceDefinition g_ATDeviceDefPrinterFX80 = { "fx80", "fx80", L"Epson FX-80/FX-80+ 80-Column Printer", ATCreateDevicePrinterFX80 };

ATDevicePrinterFX80::ATDevicePrinterFX80() {
	SetSaveStateAgnostic();
}

ATDevicePrinterFX80::~ATDevicePrinterFX80() {
}

void ATDevicePrinterFX80::GetDeviceInfo(ATDeviceInfo& info) {
	info.mpDef = &g_ATDeviceDefPrinterFX80;
}

void ATDevicePrinterFX80::Init() {
	ATPrinterGraphicsSpec spec {};
	spec.mPageWidthMM = 215.9f;				// 8.5" wide paper
	spec.mPageVBorderMM = 8.0f;				// vertical border
	spec.mLeftMarginMM = kPaperLeftMarginMM<float>;
	spec.mDotRadiusMM = 0.22f;				// guess for dot radius
	spec.mVerticalDotPitchMM = 25.4f / 72.0f;	// 1/72"
	spec.mbBit0Top = false;
	spec.mNumPins = 9;
	mpGraphicsOutput = GetService<IATPrinterOutputManager>()->CreatePrinterGraphicalOutput(g_ATDeviceDefPrinterFX80.mpName, spec);
}

void ATDevicePrinterFX80::Shutdown() {
	mpGraphicsOutput = nullptr;
}

void ATDevicePrinterFX80::GetSettings(ATPropertySet& settings) {
	settings.Clear();

	if (mbAutoLF)
		settings.SetBool("auto_lf", true);

	if (mbSlashedZero)
		settings.SetBool("slashed_zero", true);
}

bool ATDevicePrinterFX80::SetSettings(const ATPropertySet& settings) {
	mbAutoLF = settings.GetBool("auto_lf", false);
	mbSlashedZero = settings.GetBool("slashed_zero", false);

	return true;
}

void ATDevicePrinterFX80::ColdReset() {
	ResetState();
}

bool ATDevicePrinterFX80::WantUnicode() const {
	return false;
}

void ATDevicePrinterFX80::WriteRaw(const uint8 *buf, size_t len) {
	while(len--) {
		uint8 ch = *buf++;

		switch(mState) {
			case State::None:
				ProcessChar(ch);
				break;

			case State::Graphics:
				ProcessGraphics(ch);
				break;

			case State::Esc:
				ProcessEsc(ch);
				break;

			case State::CommandArgs:
				mCommandArgBuf[mPendingCommandCharIndex++] = ch;
				if (mPendingCommandCharIndex >= mPendingCommandChars) {
					mState = State::None;
					
					auto cmd = mpPendingCommand;
					mpPendingCommand = nullptr;

					(this->*cmd)();
				}
				break;
		}
	}
}

void ATDevicePrinterFX80::ResetState() {
	mState = State::None;
	mLeftMarginUnits = 0;
	mRightMarginUnits = 0;
	mLineSpacingUnits = kDefaultLineSpacingUnits;

	mEighthBitAndMask = 0xFF;
	mEighthBitXorMask = 0x00;
	mbIntlCharsEnabled = false;
	mbItalicIntlCharsEnabled = false;

	mbExpandedCurrentLine = false;
	mbExpandedAlways = false;
	mbCompressed = false;
	mbElite = false;
	mbProportional = false;
	mbEmphasized = false;
	mbDoubleStrike = false;
	mbUnderline = false;
	mbItalic = false;
	mbSuperscript = false;
	mbSubscript = false;

	UpdateActiveState();

	mRedefinableGraphicsModes[0] = 0;	// ESC K - default single
	mRedefinableGraphicsModes[1] = 1;	// ESC L - default low-speed double
	mRedefinableGraphicsModes[2] = 2;	// ESC Y - default high-speed double
	mRedefinableGraphicsModes[3] = 3;	// ESC Z - default quadruple
}

void ATDevicePrinterFX80::ProcessChar(uint8 ch) {
	uint8 controlCh = ch;

	// if the italic control chars setting is disabled, ignore bit 7 when
	// testing for control chars
	if (!mbItalicIntlCharsEnabled)
		controlCh &= 0x7F;

	switch(controlCh) {
		case 0x07:	// BEL	sound beep
			return;

		case 0x08:	// BS	backspace
			ProcessCmdBackspace();
			return;

		case 0x09:	// HT	horizontal tab
			return;

		case 0x0A:	// LF	line feed
			PrintLF();
			return;

		case 0x0B:	// VT	vertical tab
			return;

		case 0x0C:	// FF	form feed
			return;

		case 0x0D:	// CR	carriage return
			if (mbAutoLF)
				PrintLF();
			else
				PrintCR();
			return;

		case 0x0E:	// SO	expanded mode on (1-line)
			mbExpandedCurrentLine = true;
			return;

		case 0x0F:	// SI	compressed on
			mbCompressed = true;
			UpdateActiveState();
			return;

		case 0x12:	// DC2	compressed off
			mbCompressed = false;
			UpdateActiveState();
			return;

		case 0x13:	// DC3	deactivate printer
			return;

		case 0x14:	// DC4	expanded mode off (1-line)
			mbExpandedCurrentLine = false;
			return;

		case 0x18:	// CAN	cancel text in line buffer
			ClearPrintBuffer();
			return;

		case 0x1B:	// ESC	escape
			mState = State::Esc;
			return;

		case 0x7F:	// DEL	delete
			if (mBufferedCharCount)
				--mBufferedCharCount;
			return;

		default:
			// if intl chars disabled, ignore chars in $00-1F not mapped to a
			// control char
			if (controlCh < 0x20 && !mbIntlCharsEnabled)
				return;
			break;
	}

	BufferChar(ch);
}

void ATDevicePrinterFX80::ProcessGraphics(uint8 ch) {
	if (!mbGraphicsDiscard) {
		if (mbGraphics9Pin && !(mGraphicsBytesLeft & 1)) {
			mGraphicsPending9PinByte = ch;
		} else {
			// check if the graphics extends beyond right column -- we need to
			// discard everything past that
			const uint32 newXPos = mXPos + mGraphicsXStep;
			if (newXPos > kMaxWidthUnits - mRightMarginUnits)
				mbGraphicsDiscard = true;
			else {
				// print new column
				uint32 pins = 0;
				
				if (mbGraphics9Pin)
					pins = ((uint32)mGraphicsPending9PinByte << 1) + (ch >> 7);
				else
					pins = (uint32)ch << 1;

				if (mbGraphicsNoAdjacentDots)
					pins &= ~mGraphicsLastPins;

				mGraphicsLastPins = pins;

				if (mpGraphicsOutput)
					mpGraphicsOutput->Print(kPaperLeftMarginMM<double> + kMMPerHorizUnit<double> * (double)mXPos, pins);

				mXPos = newXPos;
			}
		}
	}

	if (!--mGraphicsBytesLeft) {
		mState = State::None;
	}
}

void ATDevicePrinterFX80::ProcessEsc(uint8 ch) {
	mState = State::None;

	switch(ch) {
		case 0x21:	// ESC !	Master select
			BeginCommand(1, &ATDevicePrinterFX80::ProcessCmdMasterSelect);
			break;

		case 0x23:	// ESC #	Accept eighth bit as-is
			mEighthBitAndMask = 0xFF;
			mEighthBitXorMask = 0x00;
			break;

		case 0x2A:	// ESC *	Graphics mode
			BeginCommand(3, &ATDevicePrinterFX80::ProcessCmdGraphics);
			break;

		case 0x30:	// ESC 0	Set line spacing to 1/8 inch (9-dot)
			mLineSpacingUnits = 9*3;
			break;

		case 0x31:	// ESC 1	Set line spacing to 7/72 inch (7-dot)
			mLineSpacingUnits = 7*3;
			break;

		case 0x32:	// ESC 2	Set line spacing to 1/6 inch (12-dot)
			mLineSpacingUnits = 12*3;
			break;

		case 0x33:	// ESC 3	Set line spacing to n/216 inch
			BeginCommand(1, &ATDevicePrinterFX80::ProcessCmdLineSpacingFine);
			break;	

		case 0x34:	// ESC 4	Turn italic mode on
			mbItalic = true;
			break;

		case 0x35:	// ESC 5	Turn italic mode off
			mbItalic = false;
			break;

		case 0x36:	// ESC 6	Turn italic int'l chars on
			mbItalicIntlCharsEnabled = true;
			break;

		case 0x37:	// ESC 7	Turn italic int'l chars off
			mbItalicIntlCharsEnabled = false;
			break;

		case 0x3D:	// ESC =	Set eighth bit to 0
			mEighthBitAndMask = 0x7F;
			mEighthBitXorMask = 0x00;
			break;

		case 0x3E:	// ESC >	Set eighth bit to 1
			mEighthBitAndMask = 0x7F;
			mEighthBitXorMask = 0x80;
			break;

		case 0x41:	// ESC A	Set line spacing to n/72 inch
			BeginCommand(1, &ATDevicePrinterFX80::ProcessCmdLineSpacingCoarse);
			break;

		case 0x45:	// ESC E	Emphasized mode on
			if (!mbEmphasized) {
				mbEmphasized = true;
				UpdateActiveState();
			}
			break;

		case 0x46:	// ESC F	Emphasized mode off
			if (mbEmphasized) {
				mbEmphasized = false;
				UpdateActiveState();
			}
			break;

		case 0x47:	// ESC G	Double-strike mode on
			if (!mbDoubleStrike) {
				mbDoubleStrike = true;
				UpdateActiveState();
			}
			break;

		case 0x48:	// ESC H	Double-strike mode off
			if (mbDoubleStrike) {
				mbDoubleStrike = false;
				UpdateActiveState();
			}
			break;

		case 0x49:	// ESC I	Turn on/off intl chars
			BeginCommand(1, &ATDevicePrinterFX80::ProcessCmdIntlChars);
			break;

		case 0x4B:	// ESC K	Single-density graphics (redefinable)
			ProcessCmdGraphics(mRedefinableGraphicsModes[0]);
			break;

		case 0x4C:	// ESC L	Low-speed double density graphics (redefinable)
			ProcessCmdGraphics(mRedefinableGraphicsModes[1]);
			break;

		case 0x4D:	// ESC M	Elite mode on
			if (!mbElite) {
				mbElite = true;
				UpdateActiveState();
			}
			break;

		case 0x50:	// ESC P	Elite mode off
			if (mbElite) {
				mbElite = false;
				UpdateActiveState();
			}
			break;

		case 0x51:	// ESC Q	Set right margin
			BeginCommand(1, &ATDevicePrinterFX80::ProcessCmdRightMargin);
			return;
		
		case 0x53:	// ESC S	Turn on superscript/subscript mode
			BeginCommand(1, &ATDevicePrinterFX80::ProcessCmdSuperSubScript);
			break;

		case 0x54:	// ESC T	Turn off superscript/subscript mode
			mbSuperscript = false;
			mbSubscript = false;
			UpdateActiveState();
			break;

		case 0x57:	// ESC W	Turn on/off expanded mode
			BeginCommand(1, &ATDevicePrinterFX80::ProcessCmdExpanded);
			break;

		case 0x59:	// ESC Y	High-speed double density graphics (redefinable)
			ProcessCmdGraphics(mRedefinableGraphicsModes[2]);
			break;

		case 0x5A:	// ESC Z	Quadruple density graphics (redefinable)
			ProcessCmdGraphics(mRedefinableGraphicsModes[3]);
			break;

		case 0x5E:	// ESC ^	9-pin graphics
			BeginCommand(3, &ATDevicePrinterFX80::ProcessCmdGraphics9Pin);
			break;

		case 0x6A:	// ESC j	Reverse feed n/216"
			BeginCommand(1, &ATDevicePrinterFX80::ProcessCmdReverseFeed);
			break;

		case 0x6C:	// ESC l	Set right margin
			BeginCommand(1, &ATDevicePrinterFX80::ProcessCmdLeftMargin);
			return;

		case 0x70:	// ESC p	Turn on/off proportional mode
			BeginCommand(1, &ATDevicePrinterFX80::ProcessCmdProportional);

		default:
			// Any unrecognized escape chars are ignored and eaten
			// without being printed.
			break;
	}
}

void ATDevicePrinterFX80::BeginCommand(uint8 argBytes, void (ATDevicePrinterFX80::*fn)()) {
	mpPendingCommand = fn;
	mPendingCommandCharIndex = 0;
	mPendingCommandChars = argBytes;
	mState = State::CommandArgs;
}

std::optional<bool> ATDevicePrinterFX80::ParseBool() const {
	// Ignore 8th bit, then check for either binary or ASCII 0/1.
	// Most commands ignore any unrecognized values.
	switch(mCommandArgBuf[0] & 0x7F) {
		case 0x00:
		case 0x30:
			return false;

		case 0x01:
		case 0x31:
			return true;

		default:
			return std::nullopt;
	}
}

void ATDevicePrinterFX80::ProcessCmdBackspace() {
	// Undocumented: BS is ignored in proportional mode, since character
	// width varies
	if (mbProportional)
		return;

	// get current char width (char doesn't matter)
	const sint32 width = GetCharWidth(0x20);

	// ignore if it would backspace beyond left margin
	if (mLeftMarginUnits + width > mXPos)
		return;

	// print buffered characters
	FlushPrintBuffer();

	// back up
	mXPos -= width;
}

void ATDevicePrinterFX80::ProcessCmdExpanded() {
	const auto enable = ParseBool();

	if (!enable.has_value())
		return;

	mbExpandedCurrentLine = mbExpandedAlways = enable.value();
	UpdateActiveState();
}

void ATDevicePrinterFX80::ProcessCmdGraphics(uint8 mode) {
	BeginCommand(3, &ATDevicePrinterFX80::ProcessCmdGraphics);
	mPendingCommandCharIndex = 1;
	mCommandArgBuf[0] = mode;
}

void ATDevicePrinterFX80::ProcessCmdGraphics() {
	// The FX-80 manual says that the MSB is masked to 3 bits. However, the
	// FX-80+ takes 7.
	const uint32 n = mCommandArgBuf[1] + 256*(mCommandArgBuf[2] & 0x7F);
	mGraphicsBytesLeft = n;

	// flush any pending text
	FlushPrintBuffer();

	// zero bytes is a no-op
	if (!n)
		return;

	// check if the mode is valid
	const uint8 mode = mCommandArgBuf[0] & 0x7F;
	mbGraphicsDiscard = true;
	mbGraphics9Pin = false;

	if (mode < 8) {
		mbGraphicsDiscard = false;

		switch(mode) {
			case 0:		// single density
				mGraphicsXStep = 12;	// 5760/12 = 480 dots per line (60 dpi)
				mbGraphicsNoAdjacentDots = false;
				break;
			case 1:
				mGraphicsXStep = 6;		// 5760/6 = 960 dots per line (120 dpi)
				mbGraphicsNoAdjacentDots = false;
				break;
			case 2:
				mGraphicsXStep = 6;		// 5760/6 = 960 dots per line (120 dpi)
				mbGraphicsNoAdjacentDots = true;
				break;
			case 3:
				mGraphicsXStep = 3;		// 5760/3 = 1920 dots per line (240 dpi)
				mbGraphicsNoAdjacentDots = true;
				break;
			case 4:
				mGraphicsXStep = 9;		// 5760/9 = 640 dots per line (80 dpi)
				mbGraphicsNoAdjacentDots = false;
				break;
			case 5:
				mGraphicsXStep = 10;	// 5760/6 = 576 dots per line (72 dpi)
				mbGraphicsNoAdjacentDots = false;
				break;
			case 6:
				mGraphicsXStep = 8;		// 5760/8 = 720 dots per line (90 dpi)
				mbGraphicsNoAdjacentDots = false;
				break;
			case 7:
				mGraphicsXStep = 5;		// 5760/6 = 1152 dots per line (144 dpi)
				mbGraphicsNoAdjacentDots = false;
				break;
		}
	}

	mState = State::Graphics;
}

void ATDevicePrinterFX80::ProcessCmdGraphics9Pin() {
	const uint8 mode = mCommandArgBuf[0] & 0x7F;

	// modes 0-1 valid for FX-80; modes 0-3 valid for FX-80+

	if (mode >= 4)
		mCommandArgBuf[0] = 0x7F;

	ProcessCmdGraphics();

	mbGraphics9Pin = true;
	mGraphicsBytesLeft *= 2;
}

void ATDevicePrinterFX80::ProcessCmdIntlChars() {
	const auto enable = ParseBool();

	if (enable.has_value())
		mbIntlCharsEnabled = enable.value();
}

void ATDevicePrinterFX80::ProcessCmdLeftMargin() {
	const sint32 charCount = mCommandArgBuf[0];
	const sint32 width = GetCharWidth(0x20);
	const sint32 leftMarginUnits = charCount * width;

	// Enforce required minimum of 144 units (two pica chars) between left
	// and right margin; ignore left margin requests exceeding this.
	if (leftMarginUnits + 144 + mRightMarginUnits > kMaxWidthUnits)
		return;

	// change left margin
	mLeftMarginUnits = leftMarginUnits;

	// cancel the print buffer
	ClearPrintBuffer();
}

void ATDevicePrinterFX80::ProcessCmdLineSpacingCoarse() {
	if (mCommandArgBuf[0] >= 86)
		return;

	mLineSpacingUnits = mCommandArgBuf[0] * 3;
}

void ATDevicePrinterFX80::ProcessCmdLineSpacingFine() {
	mLineSpacingUnits = mCommandArgBuf[0];
}

void ATDevicePrinterFX80::ProcessCmdMasterSelect() {
	// Master select code bits are as follows:
	//
	//	1 - elite
	//	4 - compressed
	//	8 - emphasized
	//	16 - double-strike
	//	32 - expanded
	//
	// Proportional mode cannot be enabled via master select and is disabled.

	const uint8 mode = mCommandArgBuf[0];

	mbElite = (mode & 0x01) != 0;
	mbCompressed = (mode & 0x04) != 0;
	mbEmphasized = (mode & 0x08) != 0;
	mbDoubleStrike = (mode & 0x10) != 0;
	mbExpandedAlways = mbExpandedCurrentLine = (mode & 0x20) != 0;

	UpdateActiveState();
}

void ATDevicePrinterFX80::ProcessCmdProportional() {
	const auto mode = ParseBool();

	if (!mode.has_value())
		return;

	mbProportional = mode.value();

	UpdateActiveState();
}

void ATDevicePrinterFX80::ProcessCmdReverseFeed() {
	const sint32 increment = mCommandArgBuf[0];

	FlushPrintBuffer();

	if (increment)
		FeedPaper(-increment);
}

void ATDevicePrinterFX80::ProcessCmdRightMargin() {
	const sint32 charCount = mCommandArgBuf[0];
	const sint32 width = GetCharWidth(0x20);
	const sint32 rightMarginUnits = charCount * width;

	// Enforce required minimum of 144 units (two pica chars) between left
	// and right margin; ignore right margin requests exceeding this.
	if (mLeftMarginUnits + 144 + rightMarginUnits > kMaxWidthUnits)
		return;

	// change left margin
	mRightMarginUnits = rightMarginUnits;

	// cancel the print buffer
	ClearPrintBuffer();
}

void ATDevicePrinterFX80::ProcessCmdSuperSubScript() {
	const auto mode = ParseBool();

	if (!mode.has_value())
		return;

	mbSuperscript = (mode.value() == false);
	mbSubscript = (mode.value() == true);

	UpdateActiveState();
}

void ATDevicePrinterFX80::UpdateActiveState() {
	// Elite has priority over Compressed on the FX-80. However, they can be
	// combined on the FX-80+.
	//
	// Elite overrides Proportional and Emphasized.
	//
	// Proportional implies Emphasized.
	//
	// Emphasized overrides Compressed.
	//
	// Super/subscript implies Double Strike.

	mActiveCharAttr = {};

	if (mbElite)
		mActiveCharAttr |= CharAttr::Elite;
	else if (mbProportional)
		mActiveCharAttr |= CharAttr::Proportional | CharAttr::Emphasized;
	else if (mbEmphasized)
		mActiveCharAttr |= CharAttr::Emphasized;

	if (!(mActiveCharAttr & CharAttr::Emphasized) && mbCompressed)
		mActiveCharAttr |= CharAttr::Compressed;

	// Superscript and subscript rely on double strike to work.
	if (mbSuperscript)
		mActiveCharAttr |= CharAttr::Superscript | CharAttr::DoubleStrike;
	else if (mbSubscript)
		mActiveCharAttr |= CharAttr::Subscript | CharAttr::DoubleStrike;
	else if (mbDoubleStrike)
		mActiveCharAttr |= CharAttr::DoubleStrike;

	if (mbExpandedCurrentLine)
		mActiveCharAttr |= CharAttr::Expanded;
}

void ATDevicePrinterFX80::BufferChar(uint8 ch) {
	if (mBufferedCharCount >= kMaxCharsBuffered) {
		VDFAIL("Char buffer overflow");
		return;
	}

	ch = TransformPrintCharacter(ch);

	// If the character doesn't fit, we need to flush the line and start a new
	// one. However, we then need to recompute the char width, because the
	// expanded state can reset (!).
	sint32 charWidth = GetCharWidth(ch);
	if (mXPos + charWidth >= kMaxWidthUnits - mRightMarginUnits) {
		PrintLF();
		charWidth = GetCharWidth(ch);
	}

	if (mBufferedCharCount == 0)
		mPrintXPos = mXPos;

	mCharBuffer[mBufferedCharCount++] = BufferedChar { mActiveCharAttr, ch };
	mXPos += charWidth;
}

sint32 ATDevicePrinterFX80::GetCharWidth(uint8 ch) const {
	return GetCharWidth(ch, mActiveCharAttr);
}

sint32 ATDevicePrinterFX80::GetCharWidth(uint8 ch, CharAttr attr) const {
	sint32 width = kWidthUnitsPerCharPica;

	if (+(attr & CharAttr::Proportional)) {
		const auto [start, stop] = g_ATPrinterFontFX80.mPropStartStop[ch];
		width = ((stop + 1) - start) * (kWidthUnitsPerCharPica / 12);
	} else {
		switch(+(attr & (CharAttr::Elite | CharAttr::Compressed))) {
			case +CharAttr::None:
			default:
				break;

			case +CharAttr::Elite:
				width = kWidthUnitsPerCharElite;
				break;

			case +CharAttr::Compressed:
				width = kWidthUnitsPerCharCompressed;
				break;

			case +(CharAttr::Compressed | CharAttr::Elite):
				width = kWidthUnitsPerCharCompressedElite;
				break;
		}
	}

	if (+(attr & CharAttr::Expanded))
		width *= 2;

	return width;
}

void ATDevicePrinterFX80::ClearPrintBuffer() {
	// - The print buffer is emptied without being printed.
	// - The X position is reset to the left margin.

	mBufferedCharCount = 0;
	mXPos = mLeftMarginUnits;
}

void ATDevicePrinterFX80::FlushPrintBuffer() {
	if (mpGraphicsOutput) {
		CharAttr allCharAttrs = CharAttr::None;
		sint32 xpos = mPrintXPos;

		for(int pass = 0; pass < 2; ++pass) {
			for(const BufferedChar& ch : vdspan(mCharBuffer, mBufferedCharCount)) {
				const uint16 *charDat = &g_ATPrinterFontFX80.mFont[ch.mChar][0];

				// compute paper position for left edge of character
				double paperXPos = kPaperLeftMarginMM<double> + kMMPerHorizUnit<double> * (double)xpos;

				// compute paper x-step for each dot column, based on character width
				// (12 columns / char)
				const sint32 charWidth = GetCharWidth(ch.mChar, ch.mAttributes);
				double paperDXPos = 0;
				int startColumn = 0;
				int stopColumn = 11;
				
				if (+(ch.mAttributes & CharAttr::Proportional)) {
					// proportional is always pica or pica expanded
					paperDXPos = +(ch.mAttributes & CharAttr::Expanded) ? kMMPerHorizUnit<double> * 12 : kMMPerHorizUnit<double> * 6;

					startColumn = g_ATPrinterFontFX80.mPropStartStop[ch.mChar][0];
					stopColumn = g_ATPrinterFontFX80.mPropStartStop[ch.mChar][1];
				} else {
					paperDXPos = (kMMPerHorizUnit<double> / 12.0) * (double)charWidth;
				}


				// for the second pass, skip any chars that aren't double strike
				if (!pass || +(ch.mAttributes & CharAttr::DoubleStrike)) {
					uint32 prevPins = 0;

					for(int i = startColumn; i <= stopColumn; ++i) {
						uint32 pins = charDat[i];

						if (+(ch.mAttributes & (CharAttr::Emphasized | CharAttr::Expanded))) {
							const uint32 newPins = pins | prevPins;

							prevPins = pins;
							pins = newPins;
						}

						if (+(ch.mAttributes & (CharAttr::Subscript | CharAttr::Superscript))) {
							// select even/odd pins
							if (pass)
								pins <<= 1;

							// compress pins
							pins = (pins & 0x100)
								+ ((pins & 0x40) << 1)
								+ ((pins & 0x10) << 2)
								+ ((pins & 0x4) << 3)
								+ ((pins & 0x1) << 4);

							// shift pins down for subscript
							if (+(ch.mAttributes & CharAttr::Subscript))
								pins >>= 4;
						}

						if (pins) {
							mpGraphicsOutput->Print(paperXPos + paperDXPos * (double)i, pins);

							if (+(ch.mAttributes & CharAttr::Emphasized))
								mpGraphicsOutput->Print(paperXPos + paperDXPos * ((double)i + 0.5), pins);
						}
					}
				}

				xpos += charWidth;
				allCharAttrs |= ch.mAttributes;
			}

			// If we found double strike, we need to feed paper and do another
			// pass. Otherwise, we're done.
			if (!(allCharAttrs & CharAttr::DoubleStrike))
				break;

			// At the end of the first pass, feed paper up by 1/216", then back
			// down by that much after the second pass. This is a third of a dot
			// since the dots are separated by 1/72", but the FX-80 doesn't have
			// the resolution to do half a dot.
			mpGraphicsOutput->FeedPaper(pass ? -kMMPerVertUnit<double> : kMMPerVertUnit<double>);

			if (!pass)
				xpos = mPrintXPos;
		}

		mPrintXPos = xpos;
	}

	mBufferedCharCount = 0;
}

uint8 ATDevicePrinterFX80::TransformPrintCharacter(uint8 ch) const {
	// apply 8th bit set/reset
	ch = (ch & mEighthBitAndMask) ^ mEighthBitXorMask;

	if (mbItalic)
		ch |= 0x80;

	// transform zero to slashed zero (at $7F in the font)
	if (mbSlashedZero && ((ch & 0x7F) == 0x30))
		ch = (ch & 0x80) + 0x7F;

	return ch;
}

void ATDevicePrinterFX80::PrintCR() {
	FlushPrintBuffer();
	mXPos = mLeftMarginUnits;
}

void ATDevicePrinterFX80::PrintLF() {
	PrintCR();
	FeedPaper();

	// Turn off 1-line expanded mode. This happens on an explicit LF, implicit
	// LF due to wrapping, and form feeds.
	if (mbExpandedCurrentLine) {
		mbExpandedCurrentLine = false;

		UpdateActiveState();
	}
}

void ATDevicePrinterFX80::FeedPaper() {
	FeedPaper(mLineSpacingUnits);
}

void ATDevicePrinterFX80::FeedPaper(sint32 units) {
	if (mpGraphicsOutput)
		mpGraphicsOutput->FeedPaper(kMMPerVertUnit<double> * (double)units);
}
