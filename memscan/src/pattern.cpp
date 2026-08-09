#include <oxymp/memscan/pattern.hpp>

#include <algorithm>

namespace oxymp::memscan {
namespace {

constexpr bool isSeparator(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// Значение шестнадцатеричной цифры либо -1.
constexpr int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

std::optional<Pattern> Pattern::parse(std::string_view text) {
    Pattern result;

    std::size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && isSeparator(text[pos])) {
            ++pos;
        }
        if (pos >= text.size()) {
            break;
        }

        const std::size_t tokenBegin = pos;
        while (pos < text.size() && !isSeparator(text[pos])) {
            ++pos;
        }
        const std::string_view token = text.substr(tokenBegin, pos - tokenBegin);

        if (token == "?" || token == "??") {
            result.bytes_.push_back(0);
            result.mask_.push_back(0);
            continue;
        }

        // Токен обязан быть ровно парой шестнадцатеричных цифр. Требование строгое
        // намеренно: "48B" — это почти наверняка опечатка, а не сигнатура.
        if (token.size() != 2) {
            return std::nullopt;
        }
        const int high = hexDigit(token[0]);
        const int low = hexDigit(token[1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }

        result.bytes_.push_back(static_cast<std::uint8_t>(high << 4 | low));
        result.mask_.push_back(0xFF);
    }

    if (result.bytes_.empty()) {
        return std::nullopt;
    }

    const auto anchor = std::ranges::find(result.mask_, std::uint8_t{0xFF});
    if (anchor == result.mask_.end()) {
        return std::nullopt;
    }
    result.anchorIndex_ = static_cast<std::size_t>(anchor - result.mask_.begin());

    return result;
}

bool Pattern::matchesAt(const std::uint8_t* data) const noexcept {
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if ((data[i] & mask_[i]) != bytes_[i]) {
            return false;
        }
    }
    return true;
}

} // namespace oxymp::memscan
