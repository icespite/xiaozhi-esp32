#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"
test_dir="$(mktemp -d /tmp/rlcd-web-tests.XXXXXX)"
trap 'rm -rf -- "$test_dir"' EXIT
flags=(-g -Wall -Wextra -Werror -fsanitize=address,undefined)
g++ -std=c++17 "${flags[@]}" \
  -I main/boards/waveshare-s3-rlcd-4.2/managers \
  tests/rlcd_web_url_test.cc -o "$test_dir/url"
"$test_dir/url"
python3 scripts/generate_rlcd_wifi_portal.py \
  --component managed_components/78__esp-wifi-connect \
  --fragment main/boards/waveshare-s3-rlcd-4.2/wifi_portal.html \
  --output "$test_dir/portal"
node tests/rlcd_wifi_portal_test.mjs "$test_dir/portal/rlcd_wifi_configuration.html"
g++ -std=c++17 "${flags[@]}" \
  -I main/boards/waveshare-s3-rlcd-4.2/managers \
  tests/rlcd_web_page_parser_test.cc \
  main/boards/waveshare-s3-rlcd-4.2/managers/web_page_parser.cc \
  -o "$test_dir/parser"
"$test_dir/parser" "$@"
gcc -std=c11 "${flags[@]}" -I tests/gif_host_stubs \
  -I main/display/lvgl_display/gif \
  -c main/display/lvgl_display/gif/gifdec.c -o "$test_dir/gifdec.o"
g++ -std=c++17 "${flags[@]}" -I tests/gif_host_stubs \
  -I main/display/lvgl_display/gif tests/rlcd_gif_decoder_test.cc \
  "$test_dir/gifdec.o" -o "$test_dir/gif"
"$test_dir/gif"
