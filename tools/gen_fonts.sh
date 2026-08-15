#!/usr/bin/env bash
#
# Regenerates the LVGL fonts in src/pager_font_*.c.
#
# LVGL's stock lv_font_montserrat_* fonts are ASCII-only, so the pager cannot
# use them: Telegram messages are Cyrillic. These fonts merge Montserrat's
# Latin + Cyrillic glyphs with the handful of FontAwesome icons the UI draws
# and the emoji block from Noto Emoji.
#
# The generated files are committed, so a normal build does NOT run this --
# you only need it when changing sizes, ranges or the typeface. Requires
# Node.js (lv_font_conv is fetched via npx).
#
# The source typefaces ship inside the LVGL component. Point LVGL_FONT_DIR at
# scripts/built_in_font/ if the autodetected path is wrong. Noto Emoji is not
# bundled anywhere, so it is downloaded once into tools/.fontcache/ (gitignored,
# SIL OFL 1.1); point NOTO_EMOJI_TTF at a local copy to skip the download.
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

# Noto Emoji, the *monochrome* member of the family -- not Noto Color Emoji.
# lv_font_conv rasterises outlines, and a colour font stores its glyphs as
# embedded CBDT/sbix bitmaps it cannot read at all. Monochrome line art is also
# what actually survives 14 px on this glass; a downscaled colour sticker is
# mush at that size.
NOTO_EMOJI_URL='https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf'
if [[ -z "${NOTO_EMOJI_TTF:-}" ]]; then
    NOTO_EMOJI_TTF="$PROJECT_DIR/tools/.fontcache/NotoEmoji.ttf"
    if [[ ! -f "$NOTO_EMOJI_TTF" ]]; then
        echo "downloading Noto Emoji -> $NOTO_EMOJI_TTF"
        mkdir -p "$(dirname "$NOTO_EMOJI_TTF")"
        curl -fsSL -o "$NOTO_EMOJI_TTF.part" "$NOTO_EMOJI_URL"
        mv "$NOTO_EMOJI_TTF.part" "$NOTO_EMOJI_TTF"
    fi
fi
if [[ ! -f "$NOTO_EMOJI_TTF" ]]; then
    echo "error: emoji font not found at $NOTO_EMOJI_TTF" >&2
    exit 1
fi

# ASCII, Latin-1 (for «» and friends), typographic dashes/quotes/ellipsis,
# ruble and numero signs, and the full Cyrillic block including Ё/ё and the
# Ukrainian Ґ/ґ.
TEXT_RANGES='0x20-0x7F,0xA0-0xFF,0x2010-0x2015,0x2018-0x201F,0x2026,0x20BD,0x2116,0x0400-0x045F,0x0490-0x0491'

# LV_SYMBOL_OK, REFRESH, WARNING, ENVELOPE, BELL, WIFI.
ICON_RANGES='0xF00C,0xF021,0xF071,0xF0E0,0xF0F3,0xF1EB'

# Every emoji block Telegram can send, not a hand-picked shortlist: an emoji
# the sender chose and the pager silently swallows is worse than the ~270 KB
# this costs, and there is well over a megabyte free in the factory partition.
# lv_font_conv keeps only the codepoints the font actually has, so the loose
# upper bounds here cost nothing (~1400 glyphs land in each size).
#
# Three details in the list are load-bearing:
#
#   * 0x200D (ZWJ) and 0xFE0F (VS16) are included deliberately. Noto Emoji maps
#     both to a zero-advance empty glyph, so the ❤️ and 👨‍👩‍👧 that Telegram
#     sends draw their base glyphs and nothing else. Leave them out and each
#     one becomes a missing glyph in the middle of the text.
#   * The skin-tone modifiers 0x1F3FB-0x1F3FF are excluded, which is why the
#     first range stops at 0x1F3FA. They are *not* zero-width: Noto draws them
#     as a filled swatch, so 👍🏽 would come out as a thumb followed by a blob.
#     Left unmapped they vanish, but only because LV_USE_FONT_PLACEHOLDER is
#     off in sdkconfig.defaults -- the two go together.
#   * The regional indicators inside 0x1F170-0x1F251 are kept. Monochrome Noto
#     has no flags, so 🇷🇺 renders as the boxed letters "RU", which is the
#     designed fallback and reads fine on a pager.
EMOJI_RANGES='0x200D,0x203C,0x2049,0x2122,0x2139,0x2194-0x21AA,0x231A-0x231B,0x2328,0x23CF-0x23FA,0x24C2,0x25AA-0x25FE,0x2600-0x27BF,0x2934-0x2935,0x2B05-0x2B07,0x2B1B-0x2B1C,0x2B50,0x2B55,0x3030,0x303D,0x3297,0x3299,0xFE0F,0x1F004,0x1F0CF,0x1F170-0x1F251,0x1F300-0x1F3FA,0x1F400-0x1F6FF,0x1F7E0-0x1F7EB,0x1F90C-0x1F9FF,0x1FA70-0x1FAFF'

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
        --font "$NOTO_EMOJI_TTF" -r "$EMOJI_RANGES" \
        --size "$size" \
        --bpp 4 \
        --no-compress \
        --format lvgl \
        --lv-include lvgl.h \
        --lv-font-name "pager_font_${size}" \
        -o "$out"
done

echo "done"
