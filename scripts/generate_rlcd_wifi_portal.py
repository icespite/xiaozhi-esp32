#!/usr/bin/env python3
"""Extend esp-wifi-connect in the build directory, leaving managed sources intact."""

import argparse
from pathlib import Path


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"esp-wifi-connect changed; expected exactly one {old!r}")
    return text.replace(old, new, 1)


def generate(component, fragment, output):
    source = (component / "wifi_configuration_ap.cc").read_text()
    source = '#include "web_url_provisioning.h"\n' + source
    source = replace_once(source, "_binary_wifi_configuration_html_start",
                          "_binary_rlcd_wifi_configuration_html_start")
    source = replace_once(source, '    ESP_LOGI(TAG, "Web server started");',
                          '    web_page::RegisterUrlHandlers(server_);\n'
                          '    ESP_LOGI(TAG, "Web server started");')
    source = replace_once(source, "config.max_uri_handlers = 24;", "config.max_uri_handlers = 26;")
    html = (component / "assets/wifi_configuration.html").read_text()
    marker = '<div id="wifi-tab" class="tab-content active">'
    html = replace_once(html, marker, marker + "\n" + fragment.read_text())
    marker = "const response = await fetch('/submit', {"
    html = replace_once(html, marker, "await saveWebUrl();\n                " + marker)
    output.mkdir(parents=True, exist_ok=True)
    for name, content in (("wifi_configuration_ap.cc", source), ("rlcd_wifi_configuration.html", html)):
        path = output / name
        if not path.exists() or path.read_text() != content:
            path.write_text(content)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, required=True)
    parser.add_argument("--fragment", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.component, args.fragment, args.output)
