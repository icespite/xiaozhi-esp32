#include "custom_lcd_display.h"
#include "managers/web_page_parser.h"

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
constexpr const char* kUrl = "https://icespite.top/";
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

std::string ReadHtml(esp_http_client_handle_t client, char* buffer, size_t& size,
                     const ResponseHeaders& headers, const std::atomic<bool>& stopping) {
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
        if (!headers.is_html || headers.compressed) return "暂不支持此网页格式";
        if (length > static_cast<int64_t>(kMaxHtmlBytes)) return "网页过大，无法加载";

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
            if (bytes > kMaxHtmlBytes - size) return "网页过大，无法加载";
            memcpy(buffer + size, chunk, bytes);
            size += bytes;
        }
    }
    return "网页重定向失败";
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

    auto title = lv_label_create(web_page_);
    lv_obj_set_style_text_font(title, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "icespite.top");
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

    web_hint_label_ = lv_label_create(web_page_);
    lv_obj_set_style_text_font(web_hint_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(web_hint_label_, lv_color_white(), 0);
    lv_obj_set_pos(web_hint_label_, 10, 279);
    UpdateWebPagination();
}

void CustomLcdDisplay::UpdateWebPagination() {
    lv_obj_update_layout(web_content_view_);
    int height = std::max(1, static_cast<int>(lv_obj_get_content_height(web_content_view_)));
    int text_height = lv_obj_get_height(web_content_label_);
    int pages = std::max(1, (text_height + height - 1) / height);
    web_page_index_ %= pages;
    lv_obj_set_y(web_content_label_, -web_page_index_ * height);
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
        // All C++ objects must be destroyed before vTaskDelete().
        std::unique_ptr<char, decltype(&heap_caps_free)> buffer(
            static_cast<char*>(heap_caps_malloc(kMaxHtmlBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
            heap_caps_free);
        std::string error;
        web_page::TextContent content;
        if (!buffer) {
            error = "内存不足，双击重试";
        } else {
            ResponseHeaders response;
            esp_http_client_config_t config = {};
            config.url = kUrl;
            config.timeout_ms = 10000;
            config.crt_bundle_attach = esp_crt_bundle_attach;
            config.event_handler = OnHttpEvent;
            config.user_data = &response;
            config.disable_auto_redirect = true;
            config.max_redirection_count = 3;
            auto client = esp_http_client_init(&config);
            if (!client) {
                error = "连接失败，双击重试";
            } else {
                esp_http_client_set_header(client, "Accept", "text/html, application/xhtml+xml");
                esp_http_client_set_header(client, "Accept-Encoding", "identity");
                size_t size = 0;
                error = ReadHtml(client, buffer.get(), size, response, self->web_stopping_);
                esp_http_client_cleanup(client);
                if (error.empty()) {
                    content = web_page::ExtractText(std::string_view(buffer.get(), size));
                    if (content.text.empty()) error = "网页没有可显示的正文";
                }
            }
        }
        if (!self->web_stopping_) {
            DisplayLockGuard lock(self);
            if (error.empty()) {
                lv_label_set_text(self->web_content_label_, content.text.c_str());
                lv_label_set_text(self->web_status_label_,
                                  content.truncated ? "内容过长，已截短" : "网页阅读 · 已更新");
                self->web_loaded_ = true;
                self->web_page_index_ = 0;
            } else {
                // Preserve the previous successful page when refreshing fails.
                lv_label_set_text(self->web_status_label_, error.c_str());
                if (!self->web_loaded_) {
                    lv_label_set_text(self->web_content_label_,
                                      "无法加载 icespite.top。\n请检查网络，双击 USER 重试。");
                }
            }
            self->UpdateWebPagination();
        }
    }
    self->web_loading_ = false;
    vTaskDelete(nullptr);
}
