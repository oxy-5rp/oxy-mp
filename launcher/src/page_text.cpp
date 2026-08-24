#include "page_text.hpp"

#include <windows.h>

namespace oxymp::launcher {

std::string escapeJson(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 16);

    for (const char symbol : text) {
        switch (symbol) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += symbol;
            break;
        }
    }

    return escaped;
}

std::string jsonField(std::string_view json, std::string_view name) {
    const std::string key = '"' + std::string{name} + "\"";

    const std::size_t at = json.find(key);
    if (at == std::string_view::npos) {
        return {};
    }

    const std::size_t colon = json.find(':', at + key.size());
    if (colon == std::string_view::npos) {
        return {};
    }

    const std::size_t open = json.find('"', colon);
    if (open == std::string_view::npos) {
        return {};
    }

    std::string value;
    for (std::size_t i = open + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            value += json[++i];
            continue;
        }
        if (json[i] == '"') {
            break;
        }
        value += json[i];
    }

    return value;
}

std::string fillPage(std::string page, std::string_view token, std::string_view value) {
    const std::size_t at = page.find(token);
    if (at == std::string::npos) {
        return page;
    }

    page.replace(at, token.size(), value);
    return page;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          size);

    return wide;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string narrowed(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrowed.data(),
                          size, nullptr, nullptr);

    return narrowed;
}

} // namespace oxymp::launcher
