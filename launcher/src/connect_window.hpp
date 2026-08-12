#pragma once

#include "paths.hpp"
#include "session.hpp"

#include <string>

namespace oxymp::launcher {

/// Окно подключения: адрес сервера, имя игрока и ход запуска.
///
/// Отдельное окно, а не вопросы в консоли, потому что клиент раздаётся людям, а
/// не запускается из скрипта. Рисуется движком Edge — тем же, которым потом
/// рисуется слой поверх игры, чтобы оба выглядели одинаково и чинились в одном
/// месте.
class ConnectWindow {
public:
    /// Показывает окно и не возвращает управление, пока его не закроют.
    ///
    /// settings задают, что подставить в поля при открытии; адрес и имя игрок
    /// вправе изменить.
    ///
    /// Возвращает код завершения для main.
    [[nodiscard]] static int run(const Paths& paths, Session::Settings settings);
};

} // namespace oxymp::launcher
