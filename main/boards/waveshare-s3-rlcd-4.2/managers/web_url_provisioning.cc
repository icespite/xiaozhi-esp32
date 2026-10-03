#include "web_url_provisioning.h"
#include "web_url.h"

#include <cJSON.h>
#include <nvs.h>

namespace web_page {
namespace {

esp_err_t SendUrl(httpd_req_t* req) {
    auto* json = cJSON_CreateObject();
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    if (!cJSON_AddStringToObject(json, "url", GetUrl().c_str())) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    char* body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    auto result = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return result;
}

esp_err_t SaveUrl(httpd_req_t* req) {
    // Plain UTF-8 body avoids JSON escaping expanding the URL size limit.
    if (req->content_len > kMaxUrlBytes) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "URL is too long (maximum 1024 bytes)");
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        int count = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (count <= 0) {
            if (count == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            else httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete URL");
            return ESP_FAIL;
        }
        received += count;
    }
    std::string url;
    if (!NormalizeUrl(body, url)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enter a valid http:// or https:// URL without credentials");
    }
    nvs_handle_t handle;
    auto result = nvs_open("web", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "url", url.c_str());
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save URL; please retry");
    }
    return SendUrl(req);
}

}  // namespace

void RegisterUrlHandlers(httpd_handle_t server) {
    const httpd_uri_t get = {.uri = "/web/config", .method = HTTP_GET, .handler = SendUrl, .user_ctx = nullptr};
    const httpd_uri_t post = {.uri = "/web/config", .method = HTTP_POST, .handler = SaveUrl, .user_ctx = nullptr};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post));
}

}  // namespace web_page
