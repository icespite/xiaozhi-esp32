#pragma once

#include <esp_http_server.h>

namespace web_page {
void RegisterUrlHandlers(httpd_handle_t server);
}
