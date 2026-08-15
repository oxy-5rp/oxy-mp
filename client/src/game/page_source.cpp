#include "page_source.hpp"

#include <array>
#include <cstddef>
#include <string_view>

#include "loading_art.hpp"
#include "overlay.hpp"

namespace oxymp::client::game {
namespace {

/// Место в разметке, куда встаёт картинка.
///
/// Обычное примечание HTML: без подстановки страница остаётся страницей и
/// открывается в любом браузере — просто с пустым фоном экрана загрузки. Так её
/// и правят, глазами, а не вслепую.
constexpr std::string_view kArtMarker = "<!--art-->";

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Переводит байты в base64.
///
/// Своё, а не чужое, потому что чужого здесь нет: единственная библиотека рядом,
/// которая это умеет, — Chromium, а он к тому мгновению, когда страница
/// собирается, ещё не обязан быть поднят.
std::string toBase64(std::string_view bytes) {
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);

    for (std::size_t at = 0; at < bytes.size(); at += 3) {
        const std::size_t left = bytes.size() - at;

        const auto first = static_cast<unsigned char>(bytes[at]);
        const auto second = left > 1 ? static_cast<unsigned char>(bytes[at + 1]) : 0;
        const auto third = left > 2 ? static_cast<unsigned char>(bytes[at + 2]) : 0;

        const unsigned int triple = (static_cast<unsigned int>(first) << 16U) |
                                    (static_cast<unsigned int>(second) << 8U) | third;

        encoded += kBase64Alphabet[(triple >> 18U) & 0x3FU];
        encoded += kBase64Alphabet[(triple >> 12U) & 0x3FU];

        // Хвост короче трёх байт добивается знаками равенства: столько их,
        // сколько байт не хватило.
        encoded += left > 1 ? kBase64Alphabet[(triple >> 6U) & 0x3FU] : '=';
        encoded += left > 2 ? kBase64Alphabet[triple & 0x3FU] : '=';
    }

    return encoded;
}

} // namespace

std::string composePage() {
    const std::size_t marker = ui::overlayPage.find(kArtMarker);
    if (marker == std::string_view::npos) {
        return std::string{ui::overlayPage};
    }

    std::string style = R"(<style>:root{--art:url("data:image/jpeg;base64,)";
    style += toBase64(ui::loadingArt);
    style += R"(")}</style>)";

    std::string page;
    page.reserve(ui::overlayPage.size() + style.size());

    page += ui::overlayPage.substr(0, marker);
    page += style;
    page += ui::overlayPage.substr(marker + kArtMarker.size());

    return page;
}

} // namespace oxymp::client::game
