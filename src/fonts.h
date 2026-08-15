#pragma once

#include "lvgl.h"

// LVGL's stock lv_font_montserrat_* fonts cover ASCII only, so they cannot
// render the Russian text this pager exists to display. These two are built
// from the same Montserrat face plus a few FontAwesome icons, with the
// Cyrillic block and the emoji blocks included -- see tools/gen_fonts.sh.
//
// The emoji are monochrome Noto Emoji glyphs, rasterised into the same 4bpp
// bitmaps as the text and sharing its baseline, so nothing about the layout
// changes: both sizes keep the line_height/base_line they had before emoji
// were added. There is no colour path here and no second font to fall back
// to -- one glyph table answers for text, icons and emoji alike.
LV_FONT_DECLARE(pager_font_14);
LV_FONT_DECLARE(pager_font_20);
