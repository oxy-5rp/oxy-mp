#pragma once

#include <cstdint>
#include <string_view>

namespace oxymp::shared {

/// Хеш имени в нумерации игры.
///
/// Тот самый Jenkins one-at-a-time, которым GTA V называет всё подряд: модели
/// машин, оружие, погоду, анимации. Игра зовёт его joaat, и она же считает его
/// нативом GET_HASH_KEY — но нативы есть только у клиента, а называть машину по
/// имени нужно серверу.
///
/// Отсюда и эта функция. Хозяин сервера пишет в скрипте «adder», а не
/// 0xB779A091: второе он берёт из чужой таблицы, ошибается в одном знаке и
/// получает машину, которой нет, без всякого объяснения.
///
/// Имя приводится к нижнему регистру, как это делает и сама игра: ADDER, Adder
/// и adder — одна и та же машина.
[[nodiscard]] constexpr std::uint32_t joaat(std::string_view name) noexcept {
    std::uint32_t hash = 0;

    for (const char symbol : name) {
        const auto letter = static_cast<unsigned char>(symbol);

        // Приведение к нижнему регистру только для латиницы, и это не упрощение:
        // имена игры латинские, а трогать байты чужой кодировки значило бы
        // портить их. Заодно так функция остаётся независимой от локали —
        // std::tolower в русской локали ведёт себя иначе.
        const std::uint32_t lowered =
            letter >= 'A' && letter <= 'Z' ? letter + ('a' - 'A') : letter;

        hash += lowered;
        hash += hash << 10U;
        hash ^= hash >> 6U;
    }

    hash += hash << 3U;
    hash ^= hash >> 11U;
    hash += hash << 15U;

    return hash;
}

} // namespace oxymp::shared
