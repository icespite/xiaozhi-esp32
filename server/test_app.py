import concurrent.futures
import http.client
from io import BytesIO
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from PIL import Image, ImageDraw

from app import ContentStore, MAX_UPLOAD_BYTES, UploadHandler, UploadServer


class UploadTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.store = ContentStore(self.directory.name)
        self.server = UploadServer(("127.0.0.1", 0), self.store)
        self.quiet = patch.object(UploadHandler, "log_message")
        self.quiet.start()
        self.worker = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01})
        self.worker.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.worker.join()
        self.quiet.stop()
        self.directory.cleanup()

    def request(self, method, path="/api/content", body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def publish_text(self, text):
        return self.request("POST", body=json.dumps({"text": text}), headers={"Content-Type": "application/json"})

    @staticmethod
    def image_bytes(mode="RGBA", size=(1000, 800), color=(255, 0, 0, 100), format="PNG"):
        image = Image.new(mode, size, color)
        result = BytesIO()
        image.save(result, format=format)
        return result.getvalue()

    def publish_image(self, data, text="图片说明"):
        boundary = "rlcd-test-boundary"
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="text"\r\n\r\n{text}\r\n'
                f'--{boundary}\r\nContent-Disposition: form-data; name="image"; filename="../../evil.png"\r\n'
                'Content-Type: image/png\r\n\r\n').encode() + data + f"\r\n--{boundary}--\r\n".encode()
        return self.request("POST", body=body, headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})

    @staticmethod
    def gif_bytes(*, loop=0, size=(24, 16), transparent=False):
        frames = []
        for index in range(3):
            frame = Image.new("P", size, 0 if transparent else 2)
            frame.putpalette([255, 255, 255, 0, 0, 0, 255, 255, 255] + [0] * 759)
            left = index * size[0] // 3
            ImageDraw.Draw(frame).rectangle((left, 0, left + size[0] // 3 - 1, size[1] - 1), fill=1)
            frames.append(frame)
        output = BytesIO()
        options = {"loop": loop} if loop is not None else {}
        if transparent:
            options["transparency"] = 0
        frames[0].save(output, format="GIF", save_all=True, append_images=frames[1:],
                       duration=[70, 190, 330], disposal=[1, 2, 3], **options)
        return output.getvalue()

    def test_gif_animation_timing_loops_and_media_type(self):
        for loop in (None, 0, 2):
            with self.subTest(loop=loop):
                raw = self.gif_bytes(loop=loop)
                status, _, body = self.publish_image(raw)
                self.assertEqual(status, 200, body)
                content = json.loads(body)
                self.assertRegex(content["image"], r"^[0-9a-f]{32}\.gif$")
                status, headers, converted = self.request("GET", "/media/" + content["image"])
                self.assertEqual(status, 200)
                self.assertEqual(headers["Content-Type"], "image/gif")
                self.assertLessEqual(len(converted), 256 * 1024)
                with Image.open(BytesIO(converted)) as animation:
                    self.assertEqual(animation.n_frames, 3)
                    self.assertEqual(animation.info.get("loop"), loop)
                    durations, pixels = [], []
                    for frame in range(animation.n_frames):
                        animation.seek(frame)
                        durations.append(animation.info["duration"])
                        pixels.append(animation.convert("L").tobytes())
                    self.assertEqual(durations, [70, 190, 330])
                    self.assertEqual(len(set(pixels)), 3)
                self.assertIn(content["image"].encode(), self.request("GET", "/display")[2])
                self.assertEqual(ContentStore(self.directory.name).current()["image"], content["image"])

    def test_gif_transparent_partial_frames_do_not_leave_ghosts(self):
        raw = self.gif_bytes(transparent=True)
        status, _, body = self.publish_image(raw)
        self.assertEqual(status, 200, body)
        converted = self.request("GET", "/media/" + json.loads(body)["image"])[2]
        with Image.open(BytesIO(raw)) as original, Image.open(BytesIO(converted)) as result:
            self.assertEqual(original.n_frames, result.n_frames)
            for frame in range(original.n_frames):
                original.seek(frame)
                result.seek(frame)
                expected = Image.new("RGBA", original.size, "white")
                expected.alpha_composite(original.convert("RGBA"))
                self.assertEqual(result.convert("RGB").tobytes(), expected.convert("RGB").tobytes())

    def test_gif_resize_and_output_limits_preserve_previous_upload(self):
        status, _, body = self.publish_image(self.gif_bytes(size=(728, 384)))
        self.assertEqual(status, 200, body)
        converted = self.request("GET", "/media/" + json.loads(body)["image"])[2]
        with Image.open(BytesIO(converted)) as animation:
            self.assertEqual(animation.size, (364, 192))
            self.assertEqual(animation.n_frames, 3)
        before = self.store.current()
        for constant, limit in (("MAX_MEDIA_BYTES", 10), ("MAX_GIF_FRAMES", 2), ("MAX_GIF_DECODE_PIXELS", 1)):
            with self.subTest(constant=constant), patch("app." + constant, limit):
                self.assertEqual(self.publish_image(self.gif_bytes())[0], 400)
                self.assertEqual(self.store.current(), before)

    def test_empty_page_and_browser_form(self):
        status, _, body = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(b'upload-form', body)
        status, headers, body = self.request("GET", "/display")
        self.assertEqual(status, 200)
        self.assertIn("还没有上传内容".encode(), body)
        self.assertIn("text/html", headers["Content-Type"])

    def test_text_escaping_and_conditional_updates(self):
        original = '<script>alert("hi")</script>\n你好 & 世界'
        self.assertEqual(self.publish_text(original)[0], 200)
        status, headers, body = self.request("GET", "/display")
        self.assertEqual(status, 200)
        self.assertNotIn(b"<script>", body)
        self.assertIn(b"&lt;script&gt;", body)
        self.assertIn(b"<br>", body)
        etag = headers["ETag"]
        status, _, body = self.request("GET", "/display", headers={"If-None-Match": etag})
        self.assertEqual((status, body), (304, b""))
        self.publish_text("新的内容")
        status, headers, _ = self.request("GET", "/display", headers={"If-None-Match": etag})
        self.assertEqual(status, 200)
        self.assertNotEqual(headers["ETag"], etag)

    def test_device_homepage_redirects_to_static_display(self):
        self.publish_image(self.image_bytes())
        for path in ("/", "/index.html", "/?source=wifi"):
            with self.subTest(path=path):
                status, headers, body = self.request("GET", path, headers={"User-Agent": "ESP32 HTTP Client/1.0"})
                self.assertEqual(status, 302)
                self.assertEqual(headers["Location"], "/display")
                self.assertEqual(body, b"")
                status, _, display = self.request("GET", headers["Location"])
                self.assertEqual(status, 200)
                self.assertIn(b'<img src="/media/', display)
                self.assertNotIn(b'upload-form', display)
        status, _, body = self.request("GET", "/", headers={"User-Agent": "Mozilla/5.0"})
        self.assertEqual(status, 200)
        self.assertIn(b'upload-form', body)

    def test_uploaded_image_precedes_text_and_invalidates_old_cached_page(self):
        self.publish_image(self.image_bytes(), text="先看到图片，翻页看文字")
        old_etag = '"' + self.store.current()["revision"] + '"'
        status, headers, body = self.request("GET", "/display", headers={"If-None-Match": old_etag})
        self.assertEqual(status, 200)
        self.assertLess(body.index(b"<img"), body.index(b"<p>"))
        self.assertNotEqual(headers["ETag"], old_etag)
        new_etag = headers["ETag"]
        self.assertEqual(self.request("GET", "/display", headers={"If-None-Match": new_etag})[0], 304)
        render = self.store.display_html
        with patch.object(ContentStore, "display_html", side_effect=lambda state: render(state) + b"\n"):
            self.assertEqual(self.request("GET", "/display", headers={"If-None-Match": new_etag})[0], 200)

    def test_image_conversion_and_stable_old_media(self):
        status, _, body = self.publish_image(self.image_bytes())
        self.assertEqual(status, 200, body)
        content = json.loads(body)
        self.assertRegex(content["image"], r"^[0-9a-f]{32}\.png$")
        status, headers, png = self.request("GET", "/media/" + content["image"])
        self.assertEqual(status, 200)
        self.assertEqual(headers["Content-Type"], "image/png")
        with Image.open(BytesIO(png)) as converted:
            self.assertEqual(converted.mode, "1")
            self.assertLessEqual(converted.width, 364)
            self.assertLessEqual(converted.height, 192)
        self.assertLess(len(png), 256 * 1024)
        _, _, html = self.request("GET", "/display")
        self.assertIn(("/media/" + content["image"]).encode(), html)
        self.publish_text("下一条")
        self.assertEqual(self.request("GET", "/media/" + content["image"])[2], png)

    def test_restart_retains_latest_content(self):
        self.publish_text("重启后还在")
        restarted = ContentStore(self.directory.name)
        self.assertEqual(restarted.current(), self.store.current())

    def test_invalid_upload_preserves_previous_content(self):
        self.publish_text("保留我")
        before = self.store.current()
        self.assertEqual(self.publish_image(b"not an image")[0], 400)
        for invalid in ("", "字" * 1366, "null\x00byte"):
            self.assertEqual(self.publish_text(invalid)[0], 400)
        self.assertEqual(self.store.current(), before)

    def test_text_limit_fits_device_html_and_preserves_unicode(self):
        self.assertEqual(self.publish_text("字" * 1365)[0], 200)
        self.assertEqual(self.publish_text('"' * 4096)[0], 200)
        _, _, body = self.request("GET", "/display")
        self.assertLess(len(body), 32 * 1024)
        self.assertEqual(self.publish_text("a" * 4097)[0], 400)

    def test_clear_changes_revision_and_survives_restart(self):
        self.publish_image(self.image_bytes())
        before = self.store.current()["revision"]
        self.assertEqual(self.request("DELETE")[0], 200)
        current = ContentStore(self.directory.name).current()
        self.assertNotEqual(current["revision"], before)
        self.assertEqual(current["text"], "")
        self.assertIsNone(current["image"])

    def test_bad_json_types_size_and_traversal(self):
        for raw in (b"invalid", b"[]", b'{"text":42}', b'{"text":null}'):
            self.assertEqual(self.request("POST", body=raw, headers={"Content-Type": "application/json"})[0], 400)
        self.assertEqual(self.request("POST", body=b"", headers={"Content-Length": str(MAX_UPLOAD_BYTES + 1)})[0], 413)
        for path in ("/media/../../app.py", "/media/%2e%2e/content.json", "/data/content.json"):
            self.assertEqual(self.request("GET", path)[0], 404)

    def test_concurrent_publishes_are_atomic(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda n: self.publish_text(f"消息 {n}")[0], range(12)))
        self.assertEqual(results, [200] * 12)
        persisted = json.loads((Path(self.directory.name) / "content.json").read_text())
        self.assertEqual(persisted, self.store.current())


if __name__ == "__main__":
    unittest.main()
