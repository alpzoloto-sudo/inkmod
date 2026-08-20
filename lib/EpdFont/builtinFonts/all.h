#pragma once

// Only the system UI font (Inter) ships with this build. All reading-font
// families (Bitter, ChareInk7, LexendDeca) and their emoji/symbol fallbacks
// have been removed; the interface font is used everywhere, including for
// book text.
//
// Inter 14 is used only by the accessibility mode for controls such as the
// on-screen keyboard. It keeps the same complete Cyrillic coverage as the
// normal system font.
#include <builtinFonts/inter_10_bold.h>
#include <builtinFonts/inter_10_regular.h>
#include <builtinFonts/inter_12_bold.h>
#include <builtinFonts/inter_12_regular.h>
#include <builtinFonts/inter_14_bold.h>
#include <builtinFonts/inter_14_regular.h>
#include <builtinFonts/inter_8_regular.h>

// Built-in reader starter family (DejaVu Sans), exposed as "DejaVu Sans".
#include <builtinFonts/dejavu_sans_12_regular.h>
#include <builtinFonts/dejavu_sans_12_bold.h>
#include <builtinFonts/dejavu_sans_12_italic.h>
#include <builtinFonts/dejavu_sans_12_bolditalic.h>

#include <builtinFonts/inter_12_ipa_fallback.h>
#include <builtinFonts/inter_14_ipa_fallback.h>