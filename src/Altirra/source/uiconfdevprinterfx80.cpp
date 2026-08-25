//	Altirra - Atari 800/800XL/5200 emulator
//	Copyright (C) 2009-2018 Avery Lee
//
//	This program is free software; you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation; either version 2 of the License, or
//	(at your option) any later version.

#include <stdafx.h>
#include "printertypes.h"
#include "uiconfgeneric.h"

bool ATUIConfDevPrinterFX80(VDGUIHandle hParent, ATPropertySet& props) {
	return ATUIShowDialogGenericConfig(hParent, props, L"Epson FX-80/FX-80+ Printer",
		[](IATUIConfigView& view) {
			view.AddCheckbox().SetText(L"SW1-1 Enable &slashed zero").SetTag("slashed_zero").SetLabel(L"DIP switches");
			view.AddCheckbox().SetText(L"SW1-2 &Compressed mode default on").SetTag("compressed_on");
			view.AddCheckbox().SetText(L"SW1-5 E&mphasized mode default on").SetTag("emphasize_on");
			view.AddCheckbox().SetText(L"SW2-3 &Skip perforation default on").SetTag("skip_on");
			view.AddCheckbox().SetText(L"SW2-4 Automatic &line feed (LF on CR)").SetTag("auto_lf");
			view.AddCheckbox().SetText(L"J5 Use 12-inch paper height").SetTag("paper_ht12");

			view.AddIntDropDown()
				.SetTag("intl_mode")
				.SetLabel(L"International Mode")
				.AddChoice(0, L"USA")
				.AddChoice(1, L"France")
				.AddChoice(2, L"Germany")
				.AddChoice(3, L"United Kingdom")
				.AddChoice(4, L"United Denmark")
				.AddChoice(5, L"Sweden")
				.AddChoice(6, L"Italy")
				.AddChoice(7, L"Spain");
		}
	);
}
