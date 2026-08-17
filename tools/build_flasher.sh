#!/usr/bin/env bash
#
# Builds the firmware and assembles the browser flasher into dist/, ready to be
# published as a static site (Cloudflare Pages, GitHub Pages, anything).
#
# The page is esp-web-tools driving WebSerial: the .bin files go straight from
# the visitor's browser into the board, so there is nothing to deploy but files.
#
#   bash tools/build_flasher.sh              # version from `git describe`
#   FLASHER_VERSION=v1.2.0 bash tools/...    # or pinned (CI passes the tag)
#
# Needs: pio, python3, node/npm (npm only to vendor esp-web-tools; the install
# is cached in web/node_modules, so repeat runs are offline).

set -euo pipefail

ENV_NAME="esp32-c6-lcd-1_47"
ESP_WEB_TOOLS_VERSION="10.4.0"
CHIP_FAMILY="ESP32-C6"

# Public address of the published site. The canonical link in web/index.html
# points here too; robots.txt and sitemap.xml below are generated from it, so
# the domain lives in exactly two places and never in three.
SITE_URL="${FLASHER_SITE_URL:-https://pager.alexnew.ru}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/.pio/build/$ENV_NAME"
SDKCONFIG="$ROOT/sdkconfig.$ENV_NAME"
OUT="$ROOT/dist"

VERSION="${FLASHER_VERSION:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}"
BUILD_DATE="$(date -u '+%Y-%m-%d')"

echo "==> Building firmware ($ENV_NAME)"
pio run -e "$ENV_NAME"

for f in bootloader.bin partitions.bin firmware.bin; do
  [ -f "$BUILD/$f" ] || { echo "missing $BUILD/$f" >&2; exit 1; }
done

# --- Flash offsets -----------------------------------------------------------
#
# None of the three offsets is hardcoded here. The bootloader and the table come
# from sdkconfig (the C6 puts its bootloader at 0x0, unlike the classic ESP32's
# 0x1000), and the app offset follows whatever partitions.csv lays out -- nvs is
# oversized in this project, which pushes factory to 0x40000 instead of the
# stock 0x10000. A wrong offset here would ship a page that bricks every board
# it touches, so read them back from the artifacts the build just produced.

sdkconfig_val() {
  sed -n "s/^$1=//p" "$SDKCONFIG" | tr -d '"' | head -1
}

BOOTLOADER_OFFSET="$(sdkconfig_val CONFIG_BOOTLOADER_OFFSET_IN_FLASH)"
PARTITIONS_OFFSET="$(sdkconfig_val CONFIG_PARTITION_TABLE_OFFSET)"

GEN_PART="$(find "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-espidf/components/partition_table" \
              -name gen_esp32part.py -print -quit 2>/dev/null || true)"
[ -n "$GEN_PART" ] || { echo "gen_esp32part.py not found in the ESP-IDF package" >&2; exit 1; }

# gen_esp32part.py prints the decoded CSV on stdout and its chatter on stderr.
APP_OFFSET="$(python3 "$GEN_PART" "$BUILD/partitions.bin" 2>/dev/null \
              | awk -F, '$2 == "app" { print $4; exit }')"

[ -n "$BOOTLOADER_OFFSET" ] && [ -n "$PARTITIONS_OFFSET" ] && [ -n "$APP_OFFSET" ] \
  || { echo "could not determine flash offsets" >&2; exit 1; }

# esp-web-tools' manifest wants numbers, and JSON has no hex literals.
dec() { printf '%d' "$(($1))"; }

echo "==> Offsets: bootloader $BOOTLOADER_OFFSET, partitions $PARTITIONS_OFFSET, app $APP_OFFSET"

# --- Vendor esp-web-tools ----------------------------------------------------
#
# Self-hosted rather than pulled from unpkg at runtime: the flasher should not
# stop working because a CDN does, and the page is then a closed set of files.

if [ ! -d "$ROOT/web/node_modules/esp-web-tools" ]; then
  echo "==> Fetching esp-web-tools@$ESP_WEB_TOOLS_VERSION"
  npm install --prefix "$ROOT/web" --no-audit --no-fund --no-save \
      "esp-web-tools@$ESP_WEB_TOOLS_VERSION"
fi
[ -f "$ROOT/web/node_modules/esp-web-tools/dist/web/install-button.js" ] \
  || { echo "esp-web-tools layout changed: dist/web/install-button.js is gone" >&2; exit 1; }

# --- Assemble the site -------------------------------------------------------

echo "==> Assembling $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/esp-web-tools"

cp -R "$ROOT/web/node_modules/esp-web-tools/dist/web/." "$OUT/esp-web-tools/"
cp "$BUILD/bootloader.bin" "$BUILD/partitions.bin" "$BUILD/firmware.bin" "$OUT/"

sed -e "s|%%VERSION%%|$VERSION|g" \
    -e "s|%%BUILD_DATE%%|$BUILD_DATE|g" \
    "$ROOT/web/index.html" > "$OUT/index.html"

# Static page assets: the screenshot on the front page, and any search-console
# verification files dropped into web/ (google*.html, `yandex_*.html`). They are
# copied by pattern rather than by name so adding one is a matter of putting the
# file in web/ -- the verifier has to be served from the site root or the domain
# stops being verified on the next deploy.
cp "$ROOT/web/example.jpg" "$OUT/"
for f in "$ROOT"/web/google*.html "$ROOT"/web/yandex_*.html; do
  # An unmatched glob comes through literally, hence the existence check; it is
  # a `continue` rather than an `&&` so a miss cannot fail the loop under -e.
  [ -e "$f" ] || continue
  cp "$f" "$OUT/"
done

# Three separate parts, not one merged image: merging would pad the gap between
# the table and the app with 0xff and so wipe nvs on every update, taking the
# bot token, the wifi credentials and the unread queue with it.
cat > "$OUT/manifest.json" <<JSON
{
  "name": "ESP32 Telegram Pager",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "$CHIP_FAMILY",
      "parts": [
        { "path": "bootloader.bin", "offset": $(dec "$BOOTLOADER_OFFSET") },
        { "path": "partitions.bin", "offset": $(dec "$PARTITIONS_OFFSET") },
        { "path": "firmware.bin",   "offset": $(dec "$APP_OFFSET") }
      ]
    }
  ]
}
JSON

# --- Indexing ----------------------------------------------------------------
#
# The page is meant to be found by search engines, so robots.txt allows
# everything; the firmware images are the one exception -- they are payload, not
# documents, and a crawler pulling 1.5 MB of .bin on every visit is pure waste.
# The esp-web-tools bundle stays crawlable on purpose: Googlebot renders the
# page, and blocking its script would make the flash button vanish from the
# rendered copy.
cat > "$OUT/robots.txt" <<ROBOTS
User-agent: *
Allow: /
Disallow: /*.bin\$

Sitemap: $SITE_URL/sitemap.xml
ROBOTS

cat > "$OUT/sitemap.xml" <<SITEMAP
<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
  <url>
    <loc>$SITE_URL/</loc>
    <lastmod>$BUILD_DATE</lastmod>
    <changefreq>monthly</changefreq>
  </url>
</urlset>
SITEMAP

# Cloudflare Pages serves _headers itself; it is inert anywhere else.
cat > "$OUT/_headers" <<'HEADERS'
/*.bin
  Cache-Control: public, max-age=300
/manifest.json
  Cache-Control: no-cache
/example.jpg
  Cache-Control: public, max-age=86400
HEADERS

echo "==> Done: $OUT ($VERSION)"
du -h "$OUT/bootloader.bin" "$OUT/partitions.bin" "$OUT/firmware.bin"
echo "    Local preview: python3 -m http.server -d dist 8000  (WebSerial allows http://localhost)"
