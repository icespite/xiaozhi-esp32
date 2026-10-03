#include "custom_lcd_display.h"
#include "managers/web_page_parser.h"
#include "managers/web_url.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <strings.h>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);

namespace {

constexpr const char* kTag = "WebPage";
// Bound untrusted responses and LVGL text allocations on the ESP32.
constexpr size_t kMaxHtmlBytes = 32 * 1024;
constexpr int64_t kRequestDeadlineUs = 20 * 1000 * 1000;

struct ResponseHeaders {
    bool is_html = false;
    bool compressed = false;
    std::string etag;
};

esp_err_t OnHttpEvent(esp_http_client_event_t* event) {
    auto& response = *static_cast<ResponseHeaders*>(event->user_data);
    if (event->event_id == HTTP_EVENT_HEADERS_SENT) {
        response.is_html = false;
        response.compressed = false;
        response.etag.clear();
    } else if (event->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(event->header_key, "Content-Type") == 0) {
            response.is_html = strncasecmp(event->header_value, "text/html", 9) == 0 ||
                               strncasecmp(event->header_value, "application/xhtml+xml", 21) == 0;
        } else if (strcasecmp(event->header_key, "Content-Encoding") == 0) {
            response.compressed = strcasecmp(event->header_value, "identity") != 0;
        } else if (strcasecmp(event->header_key, "ETag") == 0 && strlen(event->header_value) <= 128) {
            response.etag = event->header_value;
        }
    }
    return ESP_OK;
}

std::string ReadResponse(esp_http_client_handle_t client, uint8_t* buffer, size_t capacity,
                         size_t& size, const ResponseHeaders& headers,
                         const std::atomic<bool>& stopping, bool html, bool* unchanged) {
    int64_t started_at = esp_timer_get_time();
    auto cancelled = [&]() {
        return stopping.load() || esp_timer_get_time() - started_at >= kRequestDeadlineUs;
    };
    // Use streaming reads: ESP-IDF ignores return values from ON_DATA callbacks,
    // so callback errors alone cannot stop an oversized or endless response.
    for (int redirects = 0; redirects <= 3; ++redirects) {
        if (cancelled()) return "加载超时，双击重试";
        esp_err_t result = esp_http_client_open(client, 0);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "GET failed: %s", esp_err_to_name(result));
            return "连接失败，双击重试";
        }
        int64_t length = esp_http_client_fetch_headers(client);
        if (length < 0 || cancelled()) return "连接失败或超时，双击重试";
        int status = esp_http_client_get_status_code(client);
        if (status == 304 && unchanged) {
            *unchanged = true;
            return "";
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (redirects == 3 || esp_http_client_set_redirection(client) != ESP_OK) {
                return "网页重定向失败";
            }
            esp_http_client_close(client);
            continue;
        }
        if (status != 200) return "HTTP " + std::to_string(status) + "，双击重试";
        if ((html && !headers.is_html) || headers.compressed) return "暂不支持此网页格式";
        if (length > static_cast<int64_t>(capacity)) return "网页过大，无法加载";

        char chunk[1024];
        // fetch_headers() can already buffer body bytes; read them even when
        // the underlying parser reports a complete response.
        while (true) {
            if (cancelled()) return "加载超时，双击重试";
            int read = esp_http_client_read(client, chunk, sizeof(chunk));
            if (read < 0) return "读取失败，双击重试";
            if (read == 0) {
                return esp_http_client_is_complete_data_received(client) ? "" :
                       "网页接收不完整，双击重试";
            }
            size_t bytes = static_cast<size_t>(read);
            if (bytes > capacity - size) return "网页过大，无法加载";
            memcpy(buffer + size, chunk, bytes);
            size += bytes;
        }
    }
    return "网页重定向失败";
}

std::string Fetch(const std::string& url, uint8_t* buffer, size_t capacity, size_t& size,
                  const std::atomic<bool>& stopping, bool html, std::string* final_url = nullptr,
                  const std::string& referer = {}, std::string* etag = nullptr, bool* unchanged = nullptr) {
    ResponseHeaders response;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = OnHttpEvent;
    config.user_data = &response;
    config.disable_auto_redirect = true;
    config.max_redirection_count = 3;
    auto client = esp_http_client_init(&config);
    if (!client) return "连接失败，双击重试";
    esp_http_client_set_header(client, "Accept", html ? "text/html, application/xhtml+xml" :
                                                     "image/png, image/jpeg, image/gif");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (!referer.empty()) esp_http_client_set_header(client, "Referer", referer.c_str());
    bool conditional = etag && !etag->empty();
    if (conditional) esp_http_client_set_header(client, "If-None-Match", etag->c_str());
    std::string error = ReadResponse(client, buffer, capacity, size, response, stopping, html,
                                     conditional ? unchanged : nullptr);
    if (error.empty() && etag && (!unchanged || !*unchanged)) *etag = response.etag;
    if (error.empty() && final_url) {
        char resolved[2049];
        if (esp_http_client_get_url(client, resolved, sizeof(resolved)) == ESP_OK) *final_url = resolved;
        else error = "网页地址过长";
    }
    esp_http_client_cleanup(client);
    return error;
}

}  // namespace

void CustomLcdDisplay::SetupWebUI() {
    SetupReaderUI(web_reader_, MODE_WEB);
    SetupReaderUI(upload_reader_, MODE_UPLOAD);
    DisplayLockGuard lock(this);
    upload_poll_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        DisplayLockGuard lock(self);
        if (self->display_mode_ == MODE_UPLOAD && !self->web_stopping_) {
            self->StartWebLoad(self->upload_reader_, false);
        }
    }, 5000, this);
}

void CustomLcdDisplay::SetupReaderUI(ReaderPage& page, DisplayMode mode) {
    page.owner = this;
    page.mode = mode;
    DisplayLockGuard lock(this);
    page.root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(page.root, 400, 300);
    lv_obj_set_pos(page.root, 0, 0);
    lv_obj_set_style_bg_color(page.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page.root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page.root, 0, 0);
    lv_obj_set_style_pad_all(page.root, 0, 0);
    lv_obj_set_style_radius(page.root, 0, 0);
    lv_obj_remove_flag(page.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page.root, LV_OBJ_FLAG_HIDDEN);

    auto title = page.title = lv_label_create(page.root);
    lv_obj_set_style_text_font(title, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_width(title, 132);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_label_set_text(title, mode == MODE_UPLOAD ? "上传内容" : "网页阅读");
    lv_obj_set_pos(title, 10, 8);

    page.status = lv_label_create(page.root);
    lv_obj_set_style_text_font(page.status, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(page.status, lv_color_white(), 0);
    lv_obj_set_width(page.status, 244);
    lv_obj_set_style_text_align(page.status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(page.status, LV_LABEL_LONG_DOT);
    lv_label_set_text(page.status, "网页阅读");
    lv_obj_set_pos(page.status, 146, 10);

    page.view = lv_obj_create(page.root);
    // Whole lines per screen prevent cutting a line in half when paging.
    int line_height = font_puhui_16_4.line_height + 4;
    int content_height = (228 / line_height) * line_height;
    lv_obj_set_size(page.view, 380, content_height);
    lv_obj_set_pos(page.view, 10, 40);
    lv_obj_set_style_bg_color(page.view, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(page.view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page.view, 0, 0);
    lv_obj_set_style_pad_all(page.view, 0, 0);
    lv_obj_set_style_radius(page.view, 0, 0);
    lv_obj_remove_flag(page.view, LV_OBJ_FLAG_SCROLLABLE);

    page.content = lv_label_create(page.view);
    lv_obj_set_width(page.content, 364);
    lv_obj_set_pos(page.content, 8, 0);
    lv_obj_set_style_text_font(page.content, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(page.content, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(page.content, 4, 0);
    lv_label_set_long_mode(page.content, LV_LABEL_LONG_WRAP);
    lv_label_set_text(page.content, "打开网页后加载内容");

    page.image = lv_image_create(page.view);
    lv_obj_set_pos(page.image, 8, 0);
    lv_obj_set_size(page.image, 364, content_height - line_height);
    lv_image_set_inner_align(page.image, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_flag(page.image, LV_OBJ_FLAG_HIDDEN);

    page.hint = lv_label_create(page.root);
    lv_obj_set_style_text_font(page.hint, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(page.hint, lv_color_white(), 0);
    lv_obj_set_pos(page.hint, 10, 279);
    UpdateWebPagination(page);
}

void CustomLcdDisplay::ClearWebImage(ReaderPage& page) {
    // Detach LVGL before releasing descriptors and animation callbacks.
    if (page.image) {
        lv_image_set_src(page.image, nullptr);
        lv_obj_add_flag(page.image, LV_OBJ_FLAG_HIDDEN);
    }
    if (page.active_image >= 0) page.images[page.active_image]->Release();
    page.active_image = -1;
}

void CustomLcdDisplay::UpdateWebAnimation(ReaderPage& page) {
    if (page.active_image >= 0) {
        page.images[page.active_image]->SetPlaying(display_mode_ == page.mode);
    }
}

void CustomLcdDisplay::SetWebContent(ReaderPage& page, const web_page::TextContent& content) {
    page.sections.clear();
    int height = std::max(1, static_cast<int>(lv_obj_get_content_height(page.view)));
    auto add_text = [&](size_t start, size_t length) {
        std::string text = content.text.substr(start, length);
        auto first = text.find_first_not_of(" \n");
        if (first == text.npos) return;
        text = text.substr(first, text.find_last_not_of(" \n") - first + 1);
        lv_label_set_text(page.content, text.c_str());
        lv_obj_update_layout(page.content);
        int pages = std::max(1, (static_cast<int>(lv_obj_get_height(page.content)) + height - 1) / height);
        page.sections.push_back({std::move(text), -1, pages});
    };
    size_t start = 0;
    for (size_t i = 0; i < content.images.size(); ++i) {
        add_text(start, content.images[i].text_offset - start);
        page.sections.push_back({{}, static_cast<int>(i), 1});
        start = content.images[i].text_offset;
    }
    add_text(start, content.text.size() - start);
    page.page_index = 0;
}

void CustomLcdDisplay::UpdateWebPagination(ReaderPage& page) {
    lv_obj_update_layout(page.view);
    int height = std::max(1, static_cast<int>(lv_obj_get_content_height(page.view)));
    int pages = 0;
    for (const auto& section : page.sections) pages += section.pages;
    // Initial/offline message still uses the original text label.
    if (pages == 0) pages = std::max(1, (static_cast<int>(lv_obj_get_height(page.content)) + height - 1) / height);
    page.page_index %= pages;
    int offset = page.page_index;
    const WebSection* current = nullptr;
    for (const auto& section : page.sections) {
        if (offset < section.pages) { current = &section; break; }
        offset -= section.pages;
    }
    int image_index = current ? current->image_index : -1;
    if (page.active_image != image_index) ClearWebImage(page);
    lv_obj_set_y(page.content, -offset * height);
    lv_obj_set_style_text_align(page.content, LV_TEXT_ALIGN_LEFT, 0);
    if (image_index >= 0) {
        auto& media = *page.images[image_index];
        const lv_image_dsc_t* descriptor = media.Decode();
        page.active_image = image_index;
        if (descriptor) {
            lv_image_set_src(page.image, descriptor);
            lv_obj_remove_flag(page.image, LV_OBJ_FLAG_HIDDEN);
            media.SetFrameCallback([&page]() { lv_obj_invalidate(page.image); });
            UpdateWebAnimation(page);
            lv_obj_set_y(page.content, lv_obj_get_height(page.image));
            lv_obj_set_style_text_align(page.content, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(page.content, media.alt.c_str());
        } else {
            std::string message = media.alt.empty() ? "图片" : media.alt;
            message += "\n" + media.error;
            lv_label_set_text(page.content, message.c_str());
        }
    } else if (current) {
        lv_label_set_text(page.content, current->text.c_str());
    }
    char hint[128];
    snprintf(hint, sizeof(hint), "%d/%d  长按翻页  双击刷新  单击切页",
             page.page_index + 1, pages);
    lv_label_set_text(page.hint, hint);
}

void CustomLcdDisplay::NextWebPage() {
    DisplayLockGuard lock(this);
    auto& page = ActiveReader();
    if (!page.content) return;
    ++page.page_index;
    UpdateWebPagination(page);
}

void CustomLcdDisplay::SwitchToWebPage() {
    DisplayLockGuard lock(this);
    display_mode_ = MODE_WEB;
    ApplyDisplayMode();
}

void CustomLcdDisplay::SwitchToUploadPage() {
    DisplayLockGuard lock(this);
    display_mode_ = MODE_UPLOAD;
    ApplyDisplayMode();
}

void CustomLcdDisplay::RefreshWebPage() {
    DisplayLockGuard lock(this);
    StartWebLoad(ActiveReader());
}

void CustomLcdDisplay::StartWebLoad(ReaderPage& page, bool force) {
    if (!page.status || web_stopping_ || page.loading.exchange(true)) return;
    if (!WifiManager::GetInstance().IsConnected()) {
        lv_label_set_text(page.status, "未联网，双击重试");
        if (!page.loaded) {
            lv_label_set_text(page.content, "请先连接 Wi-Fi，再双击 USER 加载网页。");
            UpdateWebPagination(page);
        }
        page.loading = false;
        return;
    }
    page.request_url = page.mode == MODE_UPLOAD ? web_page::GetUploadUrl() : web_page::GetUrl();
    if (page.request_url.empty()) {
        lv_label_set_text(page.status, "请先设置上传内容地址");
        if (!page.loaded) {
            lv_label_set_text(page.content, "请启动电脑上的上传服务，在配网页面填写上传内容地址：\nhttp://电脑IP:8000/display");
            UpdateWebPagination(page);
        }
        page.loading = false;
        return;
    }
    page.request_etag = !force && page.loaded_url == page.request_url ? page.etag : "";
    if (force || !page.loaded) lv_label_set_text(page.status, "正在加载...");
    // Never perform TLS/HTTP while holding the LVGL lock or on the button callback.
    if (xTaskCreate(WebLoadTask, "web_page_load", 8192, &page, 1, nullptr) != pdPASS) {
        page.loading = false;
        lv_label_set_text(page.status, "内存不足，双击重试");
    }
}

void CustomLcdDisplay::WebLoadTask(void* arg) {
    auto& page = *static_cast<ReaderPage*>(arg);
    auto* self = page.owner;
    {
        // Destroy C++ objects before vTaskDelete(). Only downloads run off the LVGL thread.
        web_page::WebImage::Buffer buffer(
            static_cast<uint8_t*>(heap_caps_malloc(kMaxHtmlBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
            heap_caps_free);
        std::string error;
        web_page::TextContent content;
        std::vector<std::unique_ptr<web_page::WebImage>> images;
        bool media_failed = false;
        bool unchanged = false;
        std::string etag = page.request_etag;
        const std::string url = page.request_url;
        std::string final_url = url;
        {
            DisplayLockGuard lock(self);
            const auto host_start = url.find("://") + 3;
            const auto host = url.substr(host_start, url.find_first_of("/?#", host_start) - host_start);
            lv_label_set_text(page.title, page.mode == MODE_UPLOAD ? "上传内容" : host.c_str());
        }
        if (!buffer) error = "内存不足，双击重试";
        else {
            size_t size = 0;
            error = Fetch(url, buffer.get(), kMaxHtmlBytes, size, self->web_stopping_, true, &final_url,
                          {}, page.mode == MODE_UPLOAD ? &etag : nullptr, &unchanged);
            if (error.empty() && !unchanged) {
                content = web_page::ExtractPage(std::string_view(reinterpret_cast<char*>(buffer.get()), size), final_url);
                if (content.text.empty() && content.images.empty()) error = "网页没有可显示的内容";
            }
        }
        buffer.reset();
        size_t total_bytes = 0;
        int64_t media_started = esp_timer_get_time();
        for (const auto& reference : content.images) {
            if (self->web_stopping_) break;
            auto media = std::make_unique<web_page::WebImage>();
            media->alt = reference.alt;
            size_t capacity = std::min(web_page::kMaxImageBytes, web_page::kMaxMediaBytes - total_bytes);
            if (reference.url.empty()) media->error = "图片地址不支持";
            else if (!capacity) media->error = "图片总量超限";
            else if (esp_timer_get_time() - media_started > 40 * 1000 * 1000LL) media->error = "图片加载超时";
            else {
                media->bytes.reset(static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
                if (!media->bytes) media->error = "图片内存不足";
                else media->error = Fetch(reference.url, media->bytes.get(), capacity, media->size,
                                          self->web_stopping_, false, nullptr, final_url);
            }
            if (!media->error.empty() || media->size == 0) {
                if (media->error.empty()) media->error = "图片内容为空";
                media->bytes.reset();
                media_failed = true;
            } else {
                auto* resized = static_cast<uint8_t*>(heap_caps_realloc(media->bytes.get(), media->size,
                                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (resized) {
                    media->bytes.release();
                    media->bytes.reset(resized);
                }
                // Charge the actual allocation if shrinking fails.
                total_bytes += resized ? media->size : capacity;
            }
            images.push_back(std::move(media));
        }
        if (!self->web_stopping_) {
            DisplayLockGuard lock(self);
            if (error.empty() && unchanged) {
                // Keep the current page/GIF position when nothing was uploaded.
                lv_label_set_text(page.status, "已同步 · 每5秒检查更新");
            } else if (error.empty()) {
                self->ClearWebImage(page);
                page.images = std::move(images);
                self->SetWebContent(page, content);
                lv_label_set_text(page.status, content.truncated ? "内容过长，已截短" :
                                  media_failed ? "正文已更新，部分图片失败" : "图文阅读 · 已更新");
                page.loaded = true;
                page.loaded_url = url;
                page.etag = media_failed ? "" : etag;
            } else {
                lv_label_set_text(page.status, error.c_str());
                if (!page.loaded) {
                    lv_label_set_text(page.content,
                                      "无法加载网页。\n请检查网络和配网时设置的网址，双击 USER 重试。");
                }
            }
            if (!unchanged) self->UpdateWebPagination(page);
        }
    }
    page.loading = false;
    vTaskDelete(nullptr);
}
