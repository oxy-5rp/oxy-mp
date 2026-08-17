#include "resource_catalog.hpp"

#include "config_file.hpp"

#include <algorithm>
#include <format>

namespace oxymp::server {
namespace {

/// Годится ли это в имя ресурса.
///
/// Проверка не от взломщика: файл настроек пишет сам хозяин сервера, и вредить
/// себе ему никто не мешает. Она от описки — «resources/dlcpacks» в перечне имён
/// или «../../windows» из перенесённого откуда-то примера. Ошибка в пути
/// молчалива: каталог просто не находится, и почему — по сообщению не понять.
[[nodiscard]] bool nameLooksValid(std::string_view name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }

    return name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos;
}

/// Известна ли эта настройка alt:V.
///
/// Перечислена явно, а не угадывается по началу имени: перечень — это ровно то,
/// что мы обещаем однажды исполнить, и он должен таять по мере того, как обещание
/// исполняется. Угадывание же молчало бы и о том, чего мы не собираемся делать
/// вовсе.
[[nodiscard]] bool isAltOnlyKey(std::string_view key) {
    // Секции описания (`[inspector]`) приходят сюда с приставкой; отладчик
    // ресурса — целиком чужое хозяйство, и разбирать его по ключам незачем.
    if (key.starts_with("inspector.")) {
        return true;
    }

    return key == "required-permissions" || key == "optional-permissions" ||
           key == "client-type" || key == "keep-alive" || key == "config";
}

/// Остаётся ли путь внутри корня ресурса.
///
/// Раздаётся то, что здесь перечислено, и перечисляет это описание ресурса.
/// Путь вида `../../server.cfg` увёл бы раздачу за пределы ресурса — то есть
/// отдал бы клиентам файл, который им не предназначался.
[[nodiscard]] bool staysInside(const std::filesystem::path& root,
                               const std::filesystem::path& file) {
    std::error_code failure;

    const std::filesystem::path resolvedRoot = std::filesystem::weakly_canonical(root, failure);
    if (failure) {
        return false;
    }

    const std::filesystem::path resolved = std::filesystem::weakly_canonical(file, failure);
    if (failure) {
        return false;
    }

    // Сравнение по началу пути, а не по строкам: «resources/commands2» начинается
    // с «resources/commands», но лежит не внутри него.
    const auto rootParts = std::distance(resolvedRoot.begin(), resolvedRoot.end());

    if (std::distance(resolved.begin(), resolved.end()) < rootParts) {
        return false;
    }

    return std::equal(resolvedRoot.begin(), resolvedRoot.end(), resolved.begin());
}

} // namespace

std::vector<std::string> ResourceCatalog::load(const std::filesystem::path& directory,
                                               const std::vector<std::string>& names) {
    resources_.clear();

    std::vector<std::string> complaints;

    for (const std::string& name : names) {
        if (!nameLooksValid(name)) {
            complaints.push_back(std::format("\"{}\": негодное имя ресурса", name));
            continue;
        }

        // Дважды один и тот же не поднимается: он получил бы каждое событие по
        // два раза, а остановка убрала бы только одну из его половин.
        if (std::ranges::find(resources_, name, &ScriptResource::name) != resources_.end()) {
            complaints.push_back(std::format("\"{}\": назван дважды", name));
            continue;
        }

        const std::filesystem::path root = directory / name;

        if (!std::filesystem::is_directory(root)) {
            complaints.push_back(std::format("\"{}\": нет каталога {}", name, root.string()));
            continue;
        }

        // Описание ресурса ищется под двумя именами.
        //
        // `resource.toml` — то, как называет его alt:V, и под этим именем лежат
        // описания всех готовых игровых режимов; `resource.cfg` — как называли
        // его здесь раньше. Порядок таков, что своё имя проверяется первым: в
        // каталоге, где по недосмотру оказались оба, побеждает написанное для
        // нас, а не притащенное вместе с чужим режимом.
        config_file::Entries entries;
        std::string error;

        const std::filesystem::path ownDescription = root / "resource.cfg";
        const std::filesystem::path altDescription = root / "resource.toml";

        const std::filesystem::path description = std::filesystem::is_regular_file(ownDescription)
                                                      ? ownDescription
                                                      : altDescription;

        if (!config_file::read(description, entries, error)) {
            complaints.push_back(std::format("\"{}\": {}", name, error));
            continue;
        }

        ScriptResource resource;
        resource.name = name;
        resource.root = root;

        for (const auto& [key, value] : entries) {
            if (key == "type") {
                resource.type = value;
            } else if (key == "main") {
                resource.main = value;
            } else if (key == "client-main") {
                resource.clientMain = value;
            } else if (key == "client-files") {
                resource.clientFiles = config_file::split(value);
            } else if (key == "deps") {
                resource.dependencies = config_file::split(value);
            } else if (isAltOnlyKey(key)) {
                // Настройки alt:V, до которых у нас ещё не дошло.
                //
                // Молча пропускать их нельзя — хозяин сервера вправе знать, что
                // объявленное им не соблюдается, — но и жаловаться на них как на
                // опечатку неверно: написаны они правильно, просто здесь пока не
                // исполняются. Отсюда отдельная, спокойная формулировка.
                complaints.push_back(
                    std::format("\"{}\": настройка \"{}\" из alt:V пока не исполняется", name,
                                key));
            } else {
                complaints.push_back(std::format("\"{}\": настройка \"{}\" не понята", name, key));
            }
        }

        if (resource.type.empty()) {
            complaints.push_back(std::format("\"{}\": не сказано, чем запускать (type)", name));
            continue;
        }

        if (resource.main.empty()) {
            complaints.push_back(std::format("\"{}\": не сказано, что запускать (main)", name));
            continue;
        }

        // Клиентский вход раздаётся сам собой: перечислять его ещё и в списке
        // файлов — лишняя работа для хозяина и лишний повод забыть.
        if (!resource.clientMain.empty() &&
            std::ranges::find(resource.clientFiles, resource.clientMain) ==
                resource.clientFiles.end()) {
            resource.clientFiles.push_back(resource.clientMain);
        }

        // Пропавший файл убирается из списка, а ресурс остаётся. Раздать то, чего
        // нет, всё равно нельзя, а отменять из-за одной картинки весь игровой
        // режим — наказание не по вине.
        std::erase_if(resource.clientFiles, [&](const std::string& file) {
            const std::filesystem::path path = root / file;

            if (!staysInside(root, path)) {
                complaints.push_back(
                    std::format("\"{}\": файл \"{}\" уводит за пределы ресурса", name, file));
                return true;
            }

            if (!std::filesystem::is_regular_file(path)) {
                complaints.push_back(std::format("\"{}\": нет файла \"{}\"", name, file));
                return true;
            }

            return false;
        });

        resources_.push_back(std::move(resource));
    }

    return complaints;
}

} // namespace oxymp::server
