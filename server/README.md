# 小智 RLCD 上传服务

用浏览器发布文字、图片或图文组合，设备第五页“上传内容”通过 HTTP 获取并显示。每次发布替换当前内容；服务器重启后保留最后一次发布。

## 启动

需要 Python 3.10+。在仓库根目录运行：

```bash
python3 -m venv server/.venv
server/.venv/bin/python -m pip install -r server/requirements.txt
server/.venv/bin/python server/app.py --host 0.0.0.0 --port 8000
```

如果系统缺少 `ensurepip`，可以先创建不带 pip 的虚拟环境，再使用系统 pip 23+ 安装：

```bash
python3 -m venv --without-pip server/.venv
python3 -m pip --python server/.venv/bin/python install -r server/requirements.txt
```

浏览器打开 `http://电脑的局域网IP:8000/`。服务默认监听所有网卡，面向可信局域网使用，无登录认证。

## 连接设备

1. 烧录包含上传内容页的新固件，设备和运行服务的电脑连接同一网络。
2. 进入热点配网，连接 `Xiaozhi-XXXX`，浏览器打开 `http://192.168.4.1`。
3. 在“上传内容地址”填写 `http://电脑的局域网IP:8000/display`，点击“保存上传地址”，再退出配网。不要填写 `localhost` 或 `127.0.0.1`，它们在设备上指向设备自身。
4. 单击 USER 切到第五页“上传内容”，也可说“打开上传内容”，由 `self.disp.switch(mode="upload")` 切换。
5. 在服务首页输入文字、导入 UTF-8 `.txt` 文件或选择图片，点击“发送到屏幕”。设备停留在上传页时每 5 秒检查一次更新，实际完成时间取决于网络。

长按 USER 翻页，双击立即刷新；离开上传页后停止自动检查。内容未变时服务返回 `304`，设备保留当前阅读页码。网络失败时保留已加载内容，后续自动重试。设备未配置或配网时留空上传地址，会使用默认 `http://192.168.8.176:8001/display`；已保存的自定义地址优先，普通网页地址保持独立。若使用默认设备地址，请在对应电脑上以 `--port 8001` 启动服务。

设备应填写以 `/display` 结尾的地址。为兼容已填写服务首页的设备，服务识别 ESP-IDF 默认 User-Agent，并将其对 `/` 或 `/index.html` 的请求重定向到 `/display`；浏览器仍显示上传管理界面。

## 只显示文字或持续返回 304 时排查

停止当前服务，然后在同一份代码和数据目录下启动：

```bash
python3 server/app.py --port 8001 --no-cache
```

该选项忽略 `If-None-Match`，不发送 ETag，`/display` 每次返回完整 `200` 响应，无需重新刷机或依赖按键。终端会记录 `image=文件名/none`、`image_exists=True/False` 和文字字节数：

- `image=none`：当前发布内容没有图片，需要在管理页面重新选择图片并发送。
- `image_exists=False`：记录中的图片文件丢失，检查启动时打印的数据目录或重新上传。
- 有图片时，应继续出现 `GET /media/… 200`；若图片下载成功但仍未显示，请查看设备串口的 `Reader ... parsed` 和 `image ... failed` 日志（需包含这些日志的新固件）。

这是临时排查模式，持续返回 `200` 会让页面和 GIF 随刷新重新加载。定位后去掉 `--no-cache` 恢复正常条件更新。新固件会在图片解码失败后清除 ETag 并重试，正在下载时触发的强制刷新也会在下载结束后执行。

## 图片与文字

- 支持单独文字、单独图片、文字加图片；图文组合在设备上优先显示图片，长按 USER 翻页查看文字。
- 文字最多 4096 个 UTF-8 字节，约 1300 个汉字，支持换行。HTML 作为普通文字显示。
- 浏览器接受最多 7 MiB 的 PNG/JPEG/GIF/WebP/BMP，HTTP 请求总大小最多 8 MiB，图片最多 1600 万像素。
- 服务自动按 EXIF 方向旋转、等比例缩小到 `364×192` 以内、将透明区域填白，再抖动转换成黑白图像。静态图片保存为 PNG，GIF 逐帧转换并保存为动画 GIF，保留帧时长及原有循环设置；缺省或零帧时长按 100ms 播放。
- GIF 支持透明帧和局部更新帧，最多 500 帧、累计解码像素最多 128 Mi；转换后不超过设备支持的 256 KiB。超限会提示错误并保留原发布内容。
- GIF 在浏览器预览和设备当前图片页中播放。离开设备上传页时暂停，返回后继续；每 5 秒的内容检查不会重置动画。旧版已转成静态 PNG 的 GIF 需要重新上传原文件。
- “清空屏幕”发布空状态，设备下一次检查时显示等待上传提示。
- 所有连接同一 `/display` 地址的设备显示同一份最新内容。

## 存储

默认保存在 `server/data/`，也可通过 `--data-dir /path/to/data` 更换目录。`content.json` 原子更新；`media/` 使用不可变图片地址，避免更新过程中设备拿到错误版本。历史转换图片保留在该目录，不保留上传原图。`data/` 已加入 Git 忽略规则。

## HTTP 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 浏览器上传界面 |
| GET | `/api/content` | 最新内容 JSON（revision/text/image/updated_at） |
| POST | `/api/content` | 发布文字 JSON，或含 text/image 的 multipart 表单 |
| DELETE | `/api/content` | 清空当前内容 |
| GET | `/display` | 设备读取的简化 HTML，支持 ETag/If-None-Match |
| GET | `/media/<id>.png` 或 `/media/<id>.gif` | 转换后的黑白图片或动画，返回对应的图片类型 |

```bash
# 发送文字
curl -X POST http://127.0.0.1:8000/api/content \
  -H 'Content-Type: application/json' \
  -d '{"text":"今天也要开心！"}'

# 发送图文
curl http://127.0.0.1:8000/api/content \
  -F 'text=旅行照片' -F 'image=@/path/to/photo.jpg'

# 清空
curl -X DELETE http://127.0.0.1:8000/api/content
```

## 验证

```bash
server/.venv/bin/python -m unittest discover -s server -p 'test_*.py' -v
bash tests/run_rlcd_web_tests.sh
bash tests/run_rlcd_uploaded_gif_tests.sh
```

测试使用临时目录和随机本地端口，不修改运行服务的已发布内容。
上传 GIF 测试会将服务实际转换出的动画交给固件使用的 C 解码器，逐帧比较像素、时长和循环次数（ASan/UBSan）。
