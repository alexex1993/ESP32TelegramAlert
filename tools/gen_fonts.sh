#!/usr/bin/env bash
#
# Regenerates the LVGL fonts in src/pager_font_*.c.
#
# LVGL's stock lv_font_montserrat_* fonts are ASCII-only, so the pager cannot
# use them: Telegram messages are Cyrillic. These fonts merge Montserrat's
# Latin + Cyrillic glyphs with the handful of FontAwesome icons the UI draws.
#
# The generated files are committed, so a normal build does NOT run this --
# you only need it when changing sizes, ranges or the typeface. Requires
# Node.js (lv_font_conv is fetched via npx).
#
# The source typefaces ship inside the LVGL component. Point LVGL_FONT_DIR at
# scripts/built_in_font/ if the autodetected path is wrong.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$PROJECT_DIR/src"

if [[ -z "${LVGL_FONT_DIR:-}" ]]; then
    for candidate in \
        "$PROJECT_DIR/managed_components/lvgl__lvgl/scripts/built_in_font" \
        "$PROJECT_DIR/../TBankInvestESP32/managed_components/lvgl__lvgl/scripts/built_in_font"
    do
        if [[ -f "$candidate/Montserrat-Medium.ttf" ]]; then
            LVGL_FONT_DIR="$candidate"
            break
        fi
    done
fi

if [[ -z "${LVGL_FONT_DIR:-}" || ! -f "$LVGL_FONT_DIR/Montserrat-Medium.ttf" ]]; then
    echo "error: could not find LVGL's built_in_font directory." >&2
    echo "Build the project once so managed_components/ is populated, or set" >&2
    echo "LVGL_FONT_DIR to a checkout's scripts/built_in_font/ path." >&2
    exit 1
fi

TEXT_FONT="$LVGL_FONT_DIR/Montserrat-Medium.ttf"
ICON_FONT="$LVGL_FONT_DIR/FontAwesome5-Solid+Brands+Regular.woff"

# ASCII, Latin-1 (for «» and friends), typographic dashes/quotes/ellipsis,
# ruble and numero signs, and the full Cyrillic block including Ё/ё and the
# Ukrainian Ґ/ґ.
TEXT_RANGES='0x20-0x7F,0xA0-0xFF,0x2010-0x2015,0x2018-0x201F,0x2026,0x20BD,0x2116,0x0400-0x045F,0x0490-0x0491'

# LV_SYMBOL_OK, REFRESH, WARNING, ENVELOPE, BELL, WIFI.
ICON_RANGES='0xF00C,0xF021,0xF071,0xF0E0,0xF0F3,0xF1EB'

# --no-compress is not optional. lv_font_conv compresses glyph bitmaps by
# default at bpp > 1 and marks the font .bitmap_format = 1; LVGL only decodes
# that when CONFIG_LV_USE_FONT_COMPRESSED is on, which it is not by default.
# Without it every glyph decodes to garbage.

for size in 14 20; do
    out="$OUT_DIR/pager_font_${size}.c"
    echo "generating $out"
    npx --yes lv_font_conv@1.5.3 \
        --font "$TEXT_FONT" -r "$TEXT_RANGES" \
        --font "$ICON_FONT" -r "$ICON_RANGES" \
        --size "$size" \
        --bpp 4 \
        --no-compress \
        --format lvgl \
        --lv-include lvgl.h \
        --lv-font-name "pager_font_${size}" \
        -o "$out"
done

echo "done"
