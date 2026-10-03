#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"
test_dir="$(mktemp -d /tmp/rlcd-image-tests.XXXXXX)"
trap 'rm -rf -- "$test_dir"' EXIT
"${RLCD_TEST_PYTHON:-server/.venv/bin/python}" - "$test_dir" <<'PY'
from io import BytesIO
from pathlib import Path
import sys
from PIL import Image, ImageDraw
from server.app import prepare_image

directory = Path(sys.argv[1])
for width, height in [(728, 384), (17, 9)]:
    source = Image.new("RGBA", (width, height), "white")
    ImageDraw.Draw(source).rectangle((0, 0, width // 2, height - 1), fill="black")
    raw = BytesIO()
    source.save(raw, format="PNG")
    data, extension = prepare_image(raw.getvalue())
    assert extension == "png"
    path = directory / f"{width}.png"
    path.write_bytes(data)
    with Image.open(BytesIO(data)) as converted:
        assert converted.mode == "1"
        Path(str(path) + ".gray").write_bytes(converted.convert("L").tobytes())
PY
cmake -S tests/rlcd_image_host -B "$test_dir/build" -G Ninja
if ! cmake --build "$test_dir/build" --parallel "${RLCD_TEST_JOBS:-4}" >"$test_dir/build.log" 2>&1; then
  cat "$test_dir/build.log"
  exit 1
fi
"$test_dir/build/rlcd_image_test" "$test_dir/728.png" "$test_dir/17.png"
