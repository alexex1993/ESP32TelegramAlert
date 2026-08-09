#pragma once

#include "lvgl.h"

// LVGL's stock lv_font_montserrat_* fonts cover ASCII only, so they cannot
// render the Russian text this pager exists to display. These two are built
// from the same Montserrat face plus a few FontAwesome icons, with the
// Cyrillic block included -- see tools/gen_fonts.sh.
LV_FONT_DECLARE(pager_font_14);
LV_FONT_DECLARE(pager_font_20);
