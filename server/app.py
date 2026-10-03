#!/usr/bin/env python3
"""LAN upload server for the Xiaozhi RLCD content page (Python 3.10+)."""

import argparse
from datetime import datetime, timezone
from email import policy
from email.parser import BytesParser
from html import escape
from hashlib import sha256
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
import json
from pathlib import Path
import re
import threading
from urllib.parse import urlsplit
import uuid
import warnings

from PIL import Image, ImageOps, UnidentifiedImageError

ROOT = Path(__file__).resolve().parent
MAX_UPLOAD_BYTES = 8 * 1024 * 1024
MAX_TEXT_BYTES = 4096
MAX_IMAGE_PIXELS = 16_000_000
IMAGE_SIZE = (364, 192)
# Match the firmware's per-image download limit and bound animation processing.
MAX_MEDIA_BYTES = 256 * 1024
MAX_GIF_FRAMES = 500
MAX_GIF_DECODE_PIXELS = 128 * 1024 * 1024
MEDIA_NAME = re.compile(r"[0-9a-f]{32}\.(png|gif)\Z")


class ContentError(ValueError):
    pass


def monochrome_frame(source):
    image = ImageOps.exif_transpose(source).convert("RGBA")
    image.thumbnail(IMAGE_SIZE, Image.Resampling.LANCZOS)
    background = Image.new("RGBA", image.size, "white")
    background.alpha_composite(image)
    return background.convert("L").convert("1", dither=Image.Dither.FLOYDSTEINBERG)


def prepare_gif(source):
    frames, durations = [], []
    decoded_pixels = 0
    loop = source.info.get("loop")
    for index in range(MAX_GIF_FRAMES + 1):
        try:
            source.seek(index)
        except EOFError:
            break
        if index == MAX_GIF_FRAMES:
            raise ContentError("GIF 不能超过 500 帧，请缩短动画")
        decoded_pixels += source.width * source.height
        if source.width * source.height > MAX_IMAGE_PIXELS or decoded_pixels > MAX_GIF_DECODE_PIXELS:
            raise ContentError("GIF 解码量过大，请缩小原图尺寸或缩短动画")
        # Pillow composites partial/transparent frames using their disposal
        # method before conversion. Save opaque frames to avoid ghosting.
        frames.append(monochrome_frame(source).convert("P"))
        durations.append(source.info.get("duration", 100) or 100)
    options = {"loop": loop} if loop is not None else {}
    output = BytesIO()
    frames[0].save(output, format="GIF", save_all=True, append_images=frames[1:],
                   duration=durations, disposal=1, optimize=True, **options)
    data = output.getvalue()
    if len(data) > MAX_MEDIA_BYTES:
        raise ContentError("GIF 转换后超过设备支持的 256 KiB，请简化画面或缩短动画")
    return data, "gif"


def prepare_image(raw):
    """Convert still images to PNG and preserve GIF frames, timing and loops."""
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(BytesIO(raw)) as source:
                if source.width * source.height > MAX_IMAGE_PIXELS:
                    raise ContentError("图片不能超过 1600 万像素")
                if source.format == "GIF":
                    return prepare_gif(source)
                image = monochrome_frame(source)
                output = BytesIO()
                image.save(output, format="PNG")
                return output.getvalue(), "png"
    except (UnidentifiedImageError, OSError, SyntaxError, Image.DecompressionBombError,
            Image.DecompressionBombWarning) as error:
        raise ContentError("无法读取图片，请上传有效的 PNG、JPEG、GIF、WebP 或 BMP 图片") from error


class ContentStore:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.media = self.directory / "media"
        self.media.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.state_file = self.directory / "content.json"
        self.state = {"revision": "empty", "text": "", "image": None, "updated_at": None}
        if self.state_file.exists():
            self.state = json.loads(self.state_file.read_text(encoding="utf-8"))

    def current(self):
        with self.lock:
            return dict(self.state)

    def publish(self, text, image=None, *, clear=False):
        if not isinstance(text, str):
            raise ContentError("文字必须是字符串")
        if len(text.encode("utf-8")) > MAX_TEXT_BYTES:
            raise ContentError("文字不能超过 4096 个 UTF-8 字节（约 1300 个汉字）")
        if any(ord(char) < 32 and char not in "\r\n\t" for char in text):
            raise ContentError("文字包含不可显示的控制字符")
        text = text.strip()
        if not clear and not text and not image:
            raise ContentError("请输入文字或选择图片")
        media, extension = prepare_image(image) if image else (None, None)
        revision = uuid.uuid4().hex
        image_name = f"{revision}.{extension}" if media else None
        state = {"revision": revision, "text": text, "image": image_name,
                 "updated_at": datetime.now(timezone.utc).isoformat()}
        # Immutable image URLs keep an in-flight device fetch consistent with
        # its HTML response while another upload replaces the current content.
        with self.lock:
            if media:
                (self.media / image_name).write_bytes(media)
            temporary = self.directory / "content.json.tmp"
            temporary.write_text(json.dumps(state, ensure_ascii=False), encoding="utf-8")
            temporary.replace(self.state_file)
            self.state = state
        return dict(state)

    @staticmethod
    def display_html(state):
        text = escape(state["text"]).replace("\n", "<br>")
        body = ""
        if state["image"]:
            body = f'<img src="/media/{state["image"]}" alt="">'
        if text:
            body += f"<p>{text}</p>"
        if not body:
            body = "<p>还没有上传内容，请在电脑或手机上打开上传服务，发送图片或文字。</p>"
        return ('<!doctype html><html><head><meta charset="utf-8"></head><body>'
                + body + '</body></html>').encode("utf-8")


class UploadServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, store):
        self.store = store
        super().__init__(address, UploadHandler)


class UploadHandler(BaseHTTPRequestHandler):
    server_version = "RLCDUpload/1.0"

    def setup(self):
        super().setup()
        self.connection.settimeout(20)

    def respond(self, status, body=b"", content_type="application/json; charset=utf-8", etag=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Content-Type-Options", "nosniff")
        if etag:
            self.send_header("ETag", etag)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def json_response(self, status, data):
        self.respond(status, json.dumps(data, ensure_ascii=False).encode("utf-8"))

    def do_GET(self):
        path = urlsplit(self.path).path
        # Existing firmware uses ESP-IDF's default User-Agent. A bare server
        # address must lead it to static content, not the JS-only upload UI.
        if path in ("/", "/index.html") and self.headers.get("User-Agent", "").startswith("ESP32 HTTP Client/"):
            self.send_response(302)
            self.send_header("Location", "/display")
            self.send_header("Content-Length", "0")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
        elif path == "/":
            self.respond(200, (ROOT / "index.html").read_bytes(), "text/html; charset=utf-8")
        elif path in ("/api/content", "/display"):
            state = self.server.store.current()
            body = ContentStore.display_html(state) if path == "/display" else json.dumps(state, ensure_ascii=False).encode("utf-8")
            # A rendering change must invalidate the device's cached page even
            # when content.json has not changed across a server upgrade.
            etag = '"' + (sha256(body).hexdigest() if path == "/display" else state["revision"]) + '"'
            if self.headers.get("If-None-Match") == etag:
                self.respond(304, etag=etag)
            elif path == "/display":
                self.respond(200, body, "text/html; charset=utf-8", etag)
            else:
                self.respond(200, body, etag=etag)
        elif path.startswith("/media/") and MEDIA_NAME.fullmatch(path[7:]):
            file = self.server.store.media / path[7:]
            if file.is_file():
                self.respond(200, file.read_bytes(), "image/gif" if file.suffix == ".gif" else "image/png")
            else:
                self.json_response(404, {"error": "图片不存在"})
        else:
            self.json_response(404, {"error": "页面不存在"})

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        if urlsplit(self.path).path != "/api/content":
            self.json_response(404, {"error": "接口不存在"})
            return
        try:
            if self.headers.get("Transfer-Encoding"):
                raise ContentError("请使用 Content-Length 提交上传")
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > MAX_UPLOAD_BYTES:
                self.json_response(413, {"error": "上传请求不能为空，且不能超过 8 MiB"})
                return
            body = self.rfile.read(length)
            if len(body) != length:
                raise ContentError("上传未完成，请重试")
            content_type = self.headers.get("Content-Type", "")
            if content_type.split(";", 1)[0].strip().lower() == "application/json":
                data = json.loads(body)
                if not isinstance(data, dict):
                    raise ContentError("请求必须是 JSON 对象")
                text, image = data.get("text", ""), None
            elif content_type.lower().startswith("multipart/form-data;"):
                message = BytesParser(policy=policy.default).parsebytes(
                    b"Content-Type: " + content_type.encode("ascii") + b"\r\nMIME-Version: 1.0\r\n\r\n" + body)
                if not message.is_multipart() or message.defects:
                    raise ContentError("上传表单格式错误")
                fields = {}
                for part in message.iter_parts():
                    name = part.get_param("name", header="content-disposition")
                    if name not in ("text", "image") or name in fields or part.is_multipart() or part.defects:
                        raise ContentError("上传表单字段错误")
                    fields[name] = part.get_payload(decode=True)
                text = fields.get("text", b"").decode("utf-8")
                image = fields.get("image")
            else:
                raise ContentError("请使用 JSON 或 multipart/form-data 上传")
            state = self.server.store.publish(text, image)
            self.json_response(200, state)
        except (ContentError, ValueError, UnicodeError) as error:
            self.json_response(400, {"error": str(error)})
        except (TimeoutError, OSError):
            self.json_response(500, {"error": "上传或保存失败，请重试"})

    def do_DELETE(self):
        if urlsplit(self.path).path != "/api/content":
            self.json_response(404, {"error": "接口不存在"})
            return
        try:
            self.json_response(200, self.server.store.publish("", clear=True))
        except OSError:
            self.json_response(500, {"error": "清空失败，请重试"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--data-dir", type=Path, default=ROOT / "data")
    args = parser.parse_args()
    server = UploadServer((args.host, args.port), ContentStore(args.data_dir))
    print(f"上传服务已启动：http://{args.host}:{server.server_port}", flush=True)
    print("在设备配网页面填写：http://电脑的局域网IP:端口/display", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
