#pragma once
/*
Purpose: Special liveries (9.0): aircraft in a special paint scheme, kept in
/liveries.txt on the board and edited on the web page's Spotter tab, one a
line: the registration, then its name ("G-VPRD Pride"). One of them in
range for the first time in a day gets its own banner and a phone alert.

The list is the owner's: liveries come and go too often for one built into
the firmware to stay right. In flash, read a line at a time: no RAM kept.
*/
#include <Arduino.h>

namespace Liveries
{
    constexpr const char *kPath = "/liveries.txt";
    constexpr size_t kMaxBytes = 4096;
    // The livery's name for a registration (dashes and case ignored).
    bool find(const char *reg, char *name, size_t len);
    // The whole list (for the web page), and a new one from it.
    void read(String &out);
    bool write(const String &text);
}
