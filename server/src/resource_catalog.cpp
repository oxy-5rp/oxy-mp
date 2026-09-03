#include "resource_catalog.hpp"

#include "config_file.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <system_error>

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

/// Совпадает ли имя с шаблоном из `*` и `?`.
///
/// Своё сравнение, а не чужая библиотека: шаблоны здесь простые, а
/// std::regex стоил бы и зависимости, и заметного времени на каждом из двух
/// тысяч файлов.
///
/// Написано перебором с возвратом, а не рекурсией: шаблон приходит из файла
/// настроек, а строка вида `*a*a*a*a*b` на рекурсии разворачивается в дерево
/// глубиной в длину имени.
[[nodiscard]] bool matchesPattern(std::string_view name, std::string_view pattern) {
    std::size_t nameAt = 0;
    std::size_t patternAt = 0;

    // Куда возвращаться, если дальше не сошлось: последняя встреченная звёздочка
    // и то, сколько знаков она на тот момент съела.
    std::size_t starAt = std::string_view::npos;
    std::size_t starTook = 0;

    while (nameAt < name.size()) {
        if (patternAt < pattern.size() &&
            (pattern[patternAt] == '?' || pattern[patternAt] == name[nameAt])) {
            ++nameAt;
            ++patternAt;
        } else if (patternAt < pattern.size() && pattern[patternAt] == '*') {
            starAt = patternAt;
            starTook = nameAt;
            ++patternAt;
        } else if (starAt != std::string_view::npos) {
            // Не сошлось — отдаём звёздочке ещё один знак и пробуем снова.
            patternAt = starAt + 1;
            ++starTook;
            nameAt = starTook;
        } else {
            return false;
        }
    }

    // Хвост из одних звёздочек считается совпавшим: он вправе съесть пустоту.
    while (patternAt < pattern.size() && pattern[patternAt] == '*') {
        ++patternAt;
    }

    return patternAt == pattern.size();
}

/// Раскрывает шаблон в перечень файлов, лежащих внутри ресурса.
///
/// Обход рекурсивный, и звёздочка косую черту пересекает. Так это устроено в
/// alt:V: `client-files = ['client/*']` означает там весь каталог `client` со
/// всем, что в нём лежит, — а лежит в нём собранная страница интерфейса из
/// полутора тысяч файлов по десятку вложенных каталогов. Останавливай
/// звёздочка на косой черте — тот же перечень пришлось бы выписывать вручную
/// и дописывать после каждой пересборки страницы.
[[nodiscard]] std::vector<std::string> expandPattern(const std::filesystem::path& root,
                                                     std::string_view pattern) {
    std::vector<std::string> found;

    std::error_code failure;
    auto walk = std::filesystem::recursive_directory_iterator{
        root, std::filesystem::directory_options::skip_permission_denied, failure};

    if (failure) {
        return found;
    }

    for (const auto& entry : walk) {
        if (!entry.is_regular_file(failure) || failure) {
            continue;
        }

        const std::filesystem::path relative = std::filesystem::relative(entry.path(), root,
                                                                        failure);
        if (failure) {
            continue;
        }

        // Путь приводится к косым чертям вперёд: в описании их пишут так, а
        // filesystem на Windows отдаёт обратные, и сравнение не сошлось бы ни
        // разу.
        std::string name = relative.generic_string();

        if (matchesPattern(name, pattern)) {
            found.push_back(std::move(name));
        }
    }

    // Порядок обхода каталога системой не задан, а перечень уходит в отпечаток
    // раздачи. Без сортировки один и тот же ресурс давал бы разный список от
    // запуска к запуску.
    std::ranges::sort(found);

    return found;
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

/// Имя раздела, в котором alt:V перечисляет виды описаний.
///
/// С точкой на конце: разбор приставляет имя раздела к ключу через неё.
constexpr std::string_view kMetaSection = "meta.";

/// Тип ресурса, который ничего не исполняет, а только раздаёт файлы.
constexpr std::string_view kFileOnlyType = "dlc";

/// Читает отдельный список раздаваемого — тот, на который указывает `main` у
/// ресурса без кода.
///
/// У alt:V такой список лежит своим файлом (`stream.toml`, `dlc.toml`) и
/// содержит `files` и раздел `[meta]`. Класть их прямо в `resource.toml` он
/// тоже позволяет, поэтому прочитанное здесь **добавляется** к прочитанному
/// там, а не заменяет его.
[[nodiscard]] bool readManifest(const std::filesystem::path& path, ScriptResource& resource,
                                const std::string& name,
                                std::vector<std::string>& complaints) {
    config_file::Entries entries;
    std::string error;

    if (!config_file::read(path, entries, error)) {
        complaints.push_back(std::format("\"{}\": {}", name, error));
        return false;
    }

    for (const auto& [key, value] : entries) {
        if (key.starts_with(kMetaSection)) {
            resource.dataFiles.emplace(key.substr(kMetaSection.size()), value);
            continue;
        }

        // `files` — то же, что `client-files` в описании: список того, что
        // уходит клиенту. Имя другое, потому что список отдельный, а смысл тот
        // же, и складывать их в одно место — единственное верное решение.
        if (key == "files" || key == "client-files") {
            for (std::string& file : config_file::split(value)) {
                resource.clientFiles.push_back(std::move(file));
            }
        }
    }

    return true;
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
            if (key.starts_with(kMetaSection)) {
                // Раздел [meta]: путь к файлу — имя вида у игры. Ключ приходит
                // сюда приставленным к имени раздела, её и снимаем.
                resource.dataFiles.emplace(key.substr(kMetaSection.size()), value);
            } else if (key == "type") {
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

        // Ресурс, который ничего не исполняет.
        //
        // У alt:V это `type = "dlc"`: машины, карты, звуки — всё, что состоит из
        // одних файлов. `main` у такого указывает не на скрипт, а на список
        // раздаваемого, и читается он здесь же.
        resource.executes = resource.type != kFileOnlyType;

        if (!resource.executes && !resource.main.empty()) {
            const std::filesystem::path manifestPath = root / resource.main;

            // main приходит из собственного описания ресурса — то есть от его
            // автора, а не от нас, — и путь вида `../../server.cfg` вывел бы
            // чтение за пределы каталога ресурса точно так же, как это уже
            // запрещено для client-files ниже. Разница лишь в том, что здесь
            // файл не раздаётся клиенту, а читается самим сервером, — но читать
            // чужое по чужой указке нельзя и так.
            if (!staysInside(root, manifestPath)) {
                complaints.push_back(
                    std::format("\"{}\": main \"{}\" уводит за пределы ресурса", name,
                                resource.main));
                continue;
            }

            if (!readManifest(manifestPath, resource, name, complaints)) {
                continue;
            }

            // Дальше по коду `main` означает «что запускать», а запускать здесь
            // нечего. Оставленный, он завёл бы поиск машины для типа, которого
            // у сервера нет, — и ресурс отказал бы целиком.
            resource.main.clear();
        }

        // Ресурс без main — законный ресурс, а не описка.
        //
        // Так раздаются модели: каталог со `stream` внутри ничего не исполняет,
        // он только отдаёт файлы. Так же устроено и у alt:V, и требовать от
        // такого ресурса пустой скрипт значило бы заставлять хозяина сервера
        // класть файл ради того, чтобы его не запускали.
        //
        // А вот ресурс, которому нечего ни запустить, ни раздать, — это уже
        // описка: он не делает ничего, и сказать об этом стоит.
        if (resource.main.empty() && resource.clientFiles.empty() &&
            resource.clientMain.empty()) {
            complaints.push_back(
                std::format("\"{}\": ресурсу нечего ни запустить (main), ни раздать "
                            "(client-files)",
                            name));
            continue;
        }

        // Шаблоны раскрываются в настоящие имена.
        //
        // Делается это здесь, до всех проверок ниже, и потому дальше по коду
        // никакой разницы между шаблоном и именем уже нет: и проверка на выход
        // за пределы ресурса, и проверка на существование файла работают с
        // готовым перечнем.
        std::vector<std::string> expanded;

        for (const std::string& file : resource.clientFiles) {
            if (file.find_first_of("*?") == std::string::npos) {
                expanded.push_back(file);
                continue;
            }

            std::vector<std::string> matched = expandPattern(root, file);

            if (matched.empty()) {
                complaints.push_back(
                    std::format("\"{}\": под \"{}\" не подошёл ни один файл", name, file));
                continue;
            }

            expanded.insert(expanded.end(), std::make_move_iterator(matched.begin()),
                            std::make_move_iterator(matched.end()));
        }

        // Один файл мог подойти сразу под два шаблона — раздавать его дважды
        // незачем.
        std::ranges::sort(expanded);
        const auto duplicates = std::ranges::unique(expanded);
        expanded.erase(duplicates.begin(), duplicates.end());

        resource.clientFiles = std::move(expanded);

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
