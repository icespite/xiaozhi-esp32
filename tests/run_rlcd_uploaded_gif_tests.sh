#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"
test_dir="$(mktemp -d /tmp/rlcd-uploaded-gif.XXXXXX)"
trap 'rm -rf -- "$test_dir"' EXIT
"${RLCD_TEST_PYTHON:-server/.venv/bin/python}" - "$test_dir" <<'PY'
from io import BytesIO
from pathlib import Path
import sys
from PIL import Image, ImageDraw
from server.app import prepare_image

directory = Path(sys.argv[1])
frames = []
for index in range(3):
    frame = Image.new("RGBA", (728, 384), "white")
    ImageDraw.Draw(frame).rectangle((index * 200, 30, index * 200 + 100, 350), fill="black")
    frames.append(frame)
for loops in (-1, 0, 2):
    original = BytesIO()
    options = {"loop": loops} if loops >= 0 else {}
    frames[0].save(original, format="GIF", save_all=True, append_images=frames[1:],
                   duration=[70, 190, 330], disposal=2, **options)
    data, extension = prepare_image(original.getvalue())
    assert extension == "gif"
    (directory / f"{loops}.gif").write_bytes(data)
    with Image.open(BytesIO(data)) as converted:
        assert converted.n_frames == 3
        pixels = []
        for index in range(3):
            converted.seek(index)
            pixels.append(converted.convert("RGBA").tobytes())
        (directory / f"{loops}.rgba").write_bytes(b"".join(pixels))
PY
flags=(-g -Wall -Wextra -Werror -fsanitize=address,undefined)
gcc -std=c11 "${flags[@]}" -I tests/gif_host_stubs -I main/display/lvgl_display/gif \
  -c main/display/lvgl_display/gif/gifdec.c -o "$test_dir/gifdec.o"
g++ -std=c++17 "${flags[@]}" -I tests/gif_host_stubs -I main/display/lvgl_display/gif \
  tests/rlcd_uploaded_gif_test.cc "$test_dir/gifdec.o" -o "$test_dir/check"
for loops in -1 0 2; do
  "$test_dir/check" "$test_dir/$loops.gif" "$test_dir/$loops.rgba" "$loops"
done
