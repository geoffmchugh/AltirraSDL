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

#ifndef f_AT_PRINTERFX80_H
#define f_AT_PRINTERFX80_H

#include <at/atcore/deviceimpl.h>
#include <at/atcore/deviceprinter.h>
#include <at/atcore/enumutils.h>

class ATDevicePrinterFX80 final : public ATDeviceT<IATPrinterOutput> {
public:
	ATDevicePrinterFX80();
	~ATDevicePrinterFX80();

	void GetDeviceInfo(ATDeviceInfo& info) override;
	void Init() override;
	void Shutdown() override;
	void GetSettings(ATPropertySet& settings) override;
	bool SetSettings(const ATPropertySet& settings) override;
	void ColdReset() override;

public:	// IATPrinterOutput
	bool WantUnicode() const override;
	void WriteRaw(const uint8 *buf, size_t len) override;

private:
	enum class State : uint8 {
		None,
		Esc,
		Graphics,
		CommandArgs
	};

	// Attributes tracked on a per-character basis. These are all the attributes
	// that need to be tracked for each character because mode switches
	// can be buffered. Attributes that force a buffer empty when changed
	// don't need to be included.
	enum class CharAttr : uint16 {
		None = 0,
		Elite = 0x01,
		Compressed = 0x02,
		Expanded = 0x04,
		Proportional = 0x08,
		Underline = 0x10,
		Subscript = 0x20,
		Superscript = 0x40,
		DoubleStrike = 0x80,
		Emphasized = 0x100
	};

	void ResetState();

	void ProcessChar(uint8 ch);
	void ProcessGraphics(uint8 ch);
	void ProcessEsc(uint8 ch);

	void BeginCommand(uint8 argBytes, void (ATDevicePrinterFX80::*fn)());
	std::optional<bool> ParseBool() const;

	void ProcessCmdBackspace();
	void ProcessCmdExpanded();
	void ProcessCmdGraphics(uint8 mode);
	void ProcessCmdGraphics();
	void ProcessCmdGraphics9Pin();
	void ProcessCmdIntlChars();
	void ProcessCmdLeftMargin();
	void ProcessCmdLineSpacingCoarse();
	void ProcessCmdLineSpacingFine();
	void ProcessCmdMasterSelect();
	void ProcessCmdProportional();
	void ProcessCmdReverseFeed();
	void ProcessCmdRightMargin();
	void ProcessCmdSuperSubScript();

	void UpdateActiveState();

	void BufferChar(uint8 ch);
	sint32 GetCharWidth(uint8 ch) const;
	sint32 GetCharWidth(uint8 ch, CharAttr attr) const;
	void ClearPrintBuffer();
	void FlushPrintBuffer();

	uint8 TransformPrintCharacter(uint8 ch) const;

	void PrintCR();
	void PrintLF();
	void FeedPaper();
	void FeedPaper(sint32 units);

	// The FX-80 tracks horizontal positions in units of 1/720" and vertical
	// positions in 1/216".
	template<typename T>
	static constexpr T kMMPerHorizUnit = T(25.4 / 720.0f);
	template<typename T>
	static constexpr T kMMPerVertUnit = T(25.4 / 216.0f);

	template<typename T>
	static constexpr T kPaperLeftMarginMM = T(8.0);

	static constexpr sint32 kMaxWidthUnits = 5760;
	static constexpr sint32 kWidthUnitsPerCharPica = 72;
	static constexpr sint32 kWidthUnitsPerCharElite = 60;
	static constexpr sint32 kWidthUnitsPerCharCompressed = 42;
	static constexpr sint32 kWidthUnitsPerCharCompressedElite = 36;

	// Default line spacing is 1/6"
	static constexpr sint32 kDefaultLineSpacingUnits = 36;

	uint32 mLeftMarginUnits = 0;
	uint32 mRightMarginUnits = 0;
	sint32 mLineSpacingUnits = 0;

	// ESC KLYZ modes
	uint8 mRedefinableGraphicsModes[4] {};

	uint32 mXPos = 0;
	uint32 mBufferedCharCount = 0;
	uint32 mPrintXPos = 0;

	using PendingCommandFn = void (ATDevicePrinterFX80::*)();

	alignas(alignof(PendingCommandFn))	// workaround for MSVC bogus C4121 alignment warning on x86
	PendingCommandFn mpPendingCommand = nullptr;

	uint8 mPendingCommandCharIndex = 0;
	uint8 mPendingCommandChars = 0;
	uint8 mCommandArgBuf[16] {};

	State mState = State::None;

	AT_IMPLEMENT_ENUM_FLAGS_FRIEND_STATIC(CharAttr);

	CharAttr mActiveCharAttr {};

	uint8 mEighthBitAndMask = 0xFF;
	uint8 mEighthBitXorMask = 0x00;

	// Enables printing of chars in $00-1F that are not control characters.
	bool mbIntlCharsEnabled = false;

	// Enables $80-9F and $FF. If this is disabled, bit 7 is ignored when
	// testing for control characters.
	bool mbItalicIntlCharsEnabled = false;

	bool mbAutoLF = false;
	bool mbSlashedZero = false;

	// Expanded doubles up all columns. It can be enabled only
	// for the current line, or subsequent lines.
	bool mbExpandedCurrentLine = false;
	bool mbExpandedAlways = false;

	// Compressed prints characters at 132/160 cpi.
	bool mbCompressed = false;

	// Elite prints characters at 72/160 cpi.
	bool mbElite = false;

	// Proportional prints characters at variable width. It also
	// implies emphasized in the active state (but not the user
	// facing state).
	bool mbProportional = false;

	// Emphasized is effectively bold, re-printing each column on
	// the next column.
	bool mbEmphasized = false;

	// Double-strike reprints the same line half a dot line lower. If
	// super/subscript is enabled, even and odd dots from the character
	// data are printed.
	bool mbDoubleStrike = false;

	// Underline prints an additional underline beneath the text.
	bool mbUnderline = false;
	bool mbItalic = false;

	bool mbSuperscript = false;
	bool mbSubscript = false;

	sint32 mGraphicsXStep = 0;
	uint32 mGraphicsBytesLeft = 0;
	uint32 mGraphicsLastPins = 0;
	uint8 mGraphicsPending9PinByte = 0;
	bool mbGraphics9Pin = false;
	bool mbGraphicsNoAdjacentDots = false;
	bool mbGraphicsDiscard = false;

	vdrefptr<IATPrinterGraphicalOutput> mpGraphicsOutput;

	struct BufferedChar {
		CharAttr mAttributes;
		uint8 mChar;
	};

	// We only need a max of 160 characters in this buffer.
	static constexpr size_t kMaxCharsBuffered = 160;
	BufferedChar mCharBuffer[160] {};
};

#endif
