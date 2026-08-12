#pragma once

#include <filesystem>

namespace oxymp::launcher {

/// Раскладка каталогов клиента.
///
/// Клиент раздаётся друзьям одним каталогом, и в нём должно быть понятно, что
/// откуда взялось. Поэтому чужое хозяйство живёт отдельно от нашего: движок
/// интерфейса со своим кешем — в одном месте, ресурсы сервера — в другом,
/// журналы — в третьем.
///
/// Пути считаются от каталога с oxymp.exe, а не от текущего: лаунчер запускают
/// откуда угодно, в том числе ярлыком.
struct Paths {
    /// Каталог с oxymp.exe. Всё остальное лежит внутри него.
    std::filesystem::path root;

    /// Модуль, внедряемый в игру.
    [[nodiscard]] std::filesystem::path clientModule() const { return root / "oxymp-client.dll"; }

    /// Модуль, внедряемый в Rockstar Games Launcher.
    ///
    /// Лежит рядом с клиентским, а не в отдельном каталоге: это две половины
    /// одного дела — довести игру до запуска так, чтобы в неё можно было войти.
    [[nodiscard]] std::filesystem::path launcherPatch() const {
        return root / "oxymp-rglpatch.dll";
    }

    /// Chromium и его хозяйство.
    ///
    /// Назван по обычаю мультиплееров: у alt:V и RAGE MP в каталоге cef лежит
    /// ровно то же самое, и знакомое имя стоит дороже точности.
    [[nodiscard]] std::filesystem::path browserDirectory() const { return root / "cef"; }
    [[nodiscard]] std::filesystem::path browserCache() const {
        return browserDirectory() / "cache";
    }

    /// Кеш ресурсов, полученных от сервера.
    [[nodiscard]] std::filesystem::path resourceCache() const { return root / "cache"; }

    /// Копии файлов игры, которые пришлось бы изменить.
    ///
    /// Каталог заводится пустым и таким остаётся: oxyMP не трогает файлы игры
    /// вовсе — модуль внедряется в уже запущенный процесс. Место оставлено на
    /// случай, когда без правки файла обойтись не выйдет: тогда оригинал ляжет
    /// сюда прежде, чем что-либо будет изменено.
    [[nodiscard]] std::filesystem::path backup() const { return root / "backup"; }

    [[nodiscard]] std::filesystem::path logs() const { return root / "logs"; }

    /// Каталог, в котором лежит текущий исполняемый файл.
    [[nodiscard]] static Paths beside();

    /// Создаёт недостающие каталоги. Молча: отсутствие права на запись — не
    /// повод не запускать игру, а повод не иметь кеша.
    void ensure() const;
};

} // namespace oxymp::launcher
