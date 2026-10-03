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
};

esp_err_t OnHttpEvent(esp_http_client_event_t* event) {
    auto& response = *static_cast<ResponseHeaders*>(event->user_data);
    if (event->event_id == HTTP_EVENT_HEADERS_SENT) {
        response.is_html = false;
        response.compressed = false;
    } else if (event->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(event->header_key, "Content-Type") == 0) {
            response.is_html = strncasecmp(event->header_value, "text/html", 9) == 0 ||
                               strncasecmp(event->header_value, "application/xhtml+xml", 21) == 0;
        } else if (strcasecmp(event->header_key, "Content-Encoding") == 0) {
            response.compressed = strcasecmp(event->header_value, "identity") != 0;
        }
    }
    return ESP_OK;
}

std::string ReadResponse(esp_http_client_handle_t client, uint8_t* buffer, size_t capacity,
                         size_t& size, const ResponseHeaders& headers,
                         const std::atomic<bool>& stopping, bool html) {
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
                  const std::string& referer = {}) {
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
    std::string error = ReadResponse(client, buffer, capacity, size, response, stopping, html);
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
    DisplayLockGuard lock(this);
    web_page_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(web_page_, 400, 300);
    lv_obj_set_pos(web_page_, 0, 0);
    lv_obj_set_style_bg_color(web_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(web_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(web_page_, 0, 0);
    lv_obj_set_style_pad_all(web_page_, 0, 0);
    lv_obj_set_style_radius(web_page_, 0, 0);
    lv_obj_remove_flag(web_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(web_page_, LV_OBJ_FLAG_HIDDEN);

    auto title = web_title_label_ = lv_label_create(web_page_);
    lv_obj_set_style_text_font(title, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_width(title, 132);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_label_set_text(title, "网页阅读");
    lv_obj_set_pos(title, 10, 8);

    web_status_label_ = lv_label_create(web_page_);
    lv_obj_set_style_text_font(web_status_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(web_status_label_, lv_color_white(), 0);
    lv_obj_set_width(web_status_label_, 244);
    lv_obj_set_style_text_align(web_status_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(web_status_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(web_status_label_, "网页阅读");
    lv_obj_set_pos(web_status_label_, 146, 10);

    web_content_view_ = lv_obj_create(web_page_);
    // Whole lines per screen prevent cutting a line in half when paging.
    int line_height = font_puhui_16_4.line_height + 4;
    int content_height = (228 / line_height) * line_height;
    lv_obj_set_size(web_content_view_, 380, content_height);
    lv_obj_set_pos(web_content_view_, 10, 40);
    lv_obj_set_style_bg_color(web_content_view_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(web_content_view_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(web_content_view_, 0, 0);
    lv_obj_set_style_pad_all(web_content_view_, 0, 0);
    lv_obj_set_style_radius(web_content_view_, 0, 0);
    lv_obj_remove_flag(web_content_view_, LV_OBJ_FLAG_SCROLLABLE);

    web_content_label_ = lv_label_create(web_content_view_);
    lv_obj_set_width(web_content_label_, 364);
    lv_obj_set_pos(web_content_label_, 8, 0);
    lv_obj_set_style_text_font(web_content_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(web_content_label_, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(web_content_label_, 4, 0);
    lv_label_set_long_mode(web_content_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(web_content_label_, "打开网页后加载内容");

    web_image_obj_ = lv_image_create(web_content_view_);
    lv_obj_set_pos(web_image_obj_, 8, 0);
    lv_obj_set_size(web_image_obj_, 364, content_height - line_height);
    lv_image_set_inner_align(web_image_obj_, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_flag(web_image_obj_, LV_OBJ_FLAG_HIDDEN);

    web_hint_label_ = lv_label_create(web_page_);
    lv_obj_set_style_text_font(web_hint_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(web_hint_label_, lv_color_white(), 0);
    lv_obj_set_pos(web_hint_label_, 10, 279);
    UpdateWebPagination();
}

void CustomLcdDisplay::ClearWebImage() {
    // Detach LVGL before releasing descriptors and animation callbacks.
    if (web_image_obj_) {
        lv_image_set_src(web_image_obj_, nullptr);
        lv_obj_add_flag(web_image_obj_, LV_OBJ_FLAG_HIDDEN);
    }
    if (web_active_image_ >= 0) web_images_[web_active_image_]->Release();
    web_active_image_ = -1;
}

void CustomLcdDisplay::UpdateWebAnimation() {
    if (web_active_image_ >= 0) {
        web_images_[web_active_image_]->SetPlaying(display_mode_ == MODE_WEB);
    }
}

void CustomLcdDisplay::SetWebContent(const web_page::TextContent& content) {
    web_sections_.clear();
    int height = std::max(1, static_cast<int>(lv_obj_get_content_height(web_content_view_)));
    auto add_text = [&](size_t start, size_t length) {
        std::string text = content.text.substr(start, length);
        auto first = text.find_first_not_of(" \n");
        if (first == text.npos) return;
        text = text.substr(first, text.find_last_not_of(" \n") - first + 1);
        lv_label_set_text(web_content_label_, text.c_str());
        lv_obj_update_layout(web_content_label_);
        int pages = std::max(1, (static_cast<int>(lv_obj_get_height(web_content_label_)) + height - 1) / height);
        web_sections_.push_back({std::move(text), -1, pages});
    };
    size_t start = 0;
    for (size_t i = 0; i < content.images.size(); ++i) {
        add_text(start, content.images[i].text_offset - start);
        web_sections_.push_back({{}, static_cast<int>(i), 1});
        start = content.images[i].text_offset;
    }
    add_text(start, content.text.size() - start);
    web_page_index_ = 0;
}

void CustomLcdDisplay::UpdateWebPagination() {
    lv_obj_update_layout(web_content_view_);
    int height = std::max(1, static_cast<int>(lv_obj_get_content_height(web_content_view_)));
    int pages = 0;
    for (const auto& section : web_sections_) pages += section.pages;
    // Initial/offline message still uses the original text label.
    if (pages == 0) pages = std::max(1, (static_cast<int>(lv_obj_get_height(web_content_label_)) + height - 1) / height);
    web_page_index_ %= pages;
    int offset = web_page_index_;
    const WebSection* current = nullptr;
    for (const auto& section : web_sections_) {
        if (offset < section.pages) { current = &section; break; }
        offset -= section.pages;
    }
    int image_index = current ? current->image_index : -1;
    if (web_active_image_ != image_index) ClearWebImage();
    lv_obj_set_y(web_content_label_, -offset * height);
    lv_obj_set_style_text_align(web_content_label_, LV_TEXT_ALIGN_LEFT, 0);
    if (image_index >= 0) {
        auto& media = *web_images_[image_index];
        const lv_image_dsc_t* descriptor = media.Decode();
        web_active_image_ = image_index;
        if (descriptor) {
            lv_image_set_src(web_image_obj_, descriptor);
            lv_obj_remove_flag(web_image_obj_, LV_OBJ_FLAG_HIDDEN);
            media.SetFrameCallback([this]() { lv_obj_invalidate(web_image_obj_); });
            UpdateWebAnimation();
            lv_obj_set_y(web_content_label_, lv_obj_get_height(web_image_obj_));
            lv_obj_set_style_text_align(web_content_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(web_content_label_, media.alt.c_str());
        } else {
            std::string message = media.alt.empty() ? "图片" : media.alt;
            message += "\n" + media.error;
            lv_label_set_text(web_content_label_, message.c_str());
        }
    } else if (current) {
        lv_label_set_text(web_content_label_, current->text.c_str());
    }
    char hint[128];
    snprintf(hint, sizeof(hint), "%d/%d  长按翻页  双击刷新  单击切页",
             web_page_index_ + 1, pages);
    lv_label_set_text(web_hint_label_, hint);
}

void CustomLcdDisplay::NextWebPage() {
    DisplayLockGuard lock(this);
    if (!web_content_label_) return;
    ++web_page_index_;
    UpdateWebPagination();
}

void CustomLcdDisplay::SwitchToWebPage() {
    DisplayLockGuard lock(this);
    display_mode_ = MODE_WEB;
    ApplyDisplayMode();
}

void CustomLcdDisplay::RefreshWebPage() {
    DisplayLockGuard lock(this);
    StartWebLoad();
}

void CustomLcdDisplay::StartWebLoad() {
    if (!web_status_label_ || web_stopping_ || web_loading_.exchange(true)) return;
    if (!WifiManager::GetInstance().IsConnected()) {
        lv_label_set_text(web_status_label_, "未联网，双击重试");
        if (!web_loaded_) {
            lv_label_set_text(web_content_label_, "请先连接 Wi-Fi，再双击 USER 加载网页。");
            UpdateWebPagination();
        }
        web_loading_ = false;
        return;
    }
    lv_label_set_text(web_status_label_, "正在加载...");
    // Never perform TLS/HTTP while holding the LVGL lock or on the button callback.
    if (xTaskCreate(WebLoadTask, "web_page_load", 8192, this, 1, nullptr) != pdPASS) {
        web_loading_ = false;
        lv_label_set_text(web_status_label_, "内存不足，双击重试");
    }
}

void CustomLcdDisplay::WebLoadTask(void* arg) {
    auto* self = static_cast<CustomLcdDisplay*>(arg);
    {
        // Destroy C++ objects before vTaskDelete(). Only downloads run off the LVGL thread.
        web_page::WebImage::Buffer buffer(
            static_cast<uint8_t*>(heap_caps_malloc(kMaxHtmlBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
            heap_caps_free);
        std::string error;
        web_page::TextContent content;
        std::vector<std::unique_ptr<web_page::WebImage>> images;
        bool media_failed = false;
        const std::string url = web_page::GetUrl();
        std::string final_url = url;
        {
            DisplayLockGuard lock(self);
            const auto host_start = url.find("://") + 3;
            const auto host = url.substr(host_start, url.find_first_of("/?#", host_start) - host_start);
            lv_label_set_text(self->web_title_label_, host.c_str());
        }
        if (!buffer) error = "内存不足，双击重试";
        else {
            size_t size = 0;
            error = Fetch(url, buffer.get(), kMaxHtmlBytes, size, self->web_stopping_, true, &final_url);
            if (error.empty()) {
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
            if (error.empty()) {
                self->ClearWebImage();
                self->web_images_ = std::move(images);
                self->SetWebContent(content);
                lv_label_set_text(self->web_status_label_, content.truncated ? "内容过长，已截短" :
                                  media_failed ? "正文已更新，部分图片失败" : "图文阅读 · 已更新");
                self->web_loaded_ = true;
                self->web_loaded_url_ = url;
            } else {
                lv_label_set_text(self->web_status_label_, error.c_str());
                if (!self->web_loaded_) {
                    lv_label_set_text(self->web_content_label_,
                                      "无法加载网页。\n请检查网络和配网时设置的网址，双击 USER 重试。");
                }
            }
            self->UpdateWebPagination();
        }
    }
    self->web_loading_ = false;
    vTaskDelete(nullptr);
}
