#pragma once

#include "signature.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace oxymp::gamesig {

/// Сборка игры, под которую выверен каталог.
///
/// Сигнатуры привязаны к конкретному исполняемому файлу. На другой сборке они
/// либо не совпадут вовсе, либо совпадут неоднозначно — и то и другое проверяется
/// утилитой sigcheck до того, как что-либо будет внедрено в процесс игры.
inline constexpr std::string_view kTargetGameVersion = "1.0.3889.0";

/// Дата сборки, зашитая в исполняемый файл целевой версии игры.
///
/// Точнее номера версии отвечает на вопрос «та ли это сборка»: номер меняется не
/// при каждом обновлении, а дата — при каждом. Сверяется на старте, чтобы
/// расхождение обнаруживалось сразу, а не превращалось в загадочный сбой позже.
inline constexpr std::string_view kVerifiedBuildDate = "Jul  9 2026";

namespace detail {

inline constexpr std::array kSignatures = std::to_array<Signature>({
    // ---------------------------------------------------------------------
    // Опознание сборки
    // ---------------------------------------------------------------------
    {
        .id = "build_date_string",
        .description = "Строка с датой сборки исполняемого файла. Точнее номера версии "
                       "отвечает на вопрос, та ли это сборка, под которую выверен каталог.",
        .pattern = "48 8D 8E 09 02 00 00 44 8B C5 33 D2",
        .offset = 20,
        .resolution = Resolution::RipRelative,
        .expectedMatches = 1,
    },

    // ---------------------------------------------------------------------
    // Скриптовый движок: вызов нативных функций
    // ---------------------------------------------------------------------
    {
        .id = "native_registration_table",
        .description = "Таблица регистрации нативных функций: 256 корзин по младшему байту "
                       "хеша, внутри — связный список с обфусцированными указателями. "
                       "Самая важная сигнатура проекта: без неё нельзя вызвать ни один натив.",
        .pattern = "76 32 48 8B 53 40",
        .offset = 9,
        .resolution = Resolution::RipRelative,
        .expectedMatches = 1,
    },

    // ---------------------------------------------------------------------
    // Скриптовый движок: потоки
    // ---------------------------------------------------------------------
    {
        .id = "script_thread_collection",
        .description = "Коллекция скриптовых потоков игры. Чтобы наш код тикал на главном "
                       "потоке в корректном скриптовом контексте, свой поток нужно "
                       "зарегистрировать именно здесь.",
        .pattern = "48 8B C8 EB ? 33 C9 48 8B 05",
        .offset = 10,
        .resolution = Resolution::RipRelative,
        .expectedMatches = 1,
    },
    {
        .id = "active_thread_tls_offset",
        .description = "Смещение в TLS, по которому лежит указатель на активный скриптовый "
                       "поток. Нативы читают его, поэтому перед вызовом поток нужно подменять.",
        .pattern = "48 8B 04 D0 4A 8B 14 00 48 8B 01 F3 44 0F 2C 42 20",
        .offset = -4,
        .resolution = Resolution::Immediate32,
        .expectedMatches = 1,
    },
    {
        .id = "script_thread_id_counter",
        .description = "Счётчик идентификаторов скриптовых потоков. Свой поток обязан взять "
                       "отсюда уникальный id, иначе игра спутает его со штатным.",
        .pattern = "8B 15 ? ? ? ? 48 8B 05 ? ? ? ? FF C2 89 15 ? ? ? ? 48 8B 0C F8",
        .offset = 2,
        .resolution = Resolution::RipRelative,
        .expectedMatches = 1,
    },
    {
        .id = "script_thread_count",
        .description = "Число живых скриптовых потоков.",
        .pattern = "FF 0D ? ? ? ? 48 8B D9 75",
        .offset = 2,
        .resolution = Resolution::RipRelative,
        .expectedMatches = 1,
    },
    {
        .id = "script_handler_manager",
        .description = "Менеджер обработчиков скриптов. К нему привязывается поток, чтобы "
                       "созданные им сущности корректно учитывались и подчищались.",
        .pattern = "74 17 48 8B C8 E8 ? ? ? ? 48 8D 0D",
        .offset = 13,
        .resolution = Resolution::RipRelative,
        .expectedMatches = 1,
    },

    // ---------------------------------------------------------------------
    // Скриптовый движок: методы потока
    // ---------------------------------------------------------------------
    {
        .id = "script_thread_tick",
        .description = "Штатный тик скриптового потока. Свой поток вызывает его напрямую, "
                       "чтобы не повторять внутреннюю логику игры.",
        .pattern = "80 B9 ? 01 00 00 00 8B FA 48 8B D9 74 05",
        .offset = -0x0F,
        .resolution = Resolution::Address,
        .expectedMatches = 1,
    },
    {
        .id = "script_thread_kill",
        .description = "Штатное завершение скриптового потока.",
        .pattern = "48 83 EC 20 48 83 B9 ? 01 00 00 00 48 8B D9 74 14",
        .offset = -6,
        .resolution = Resolution::Address,
        .expectedMatches = 1,
    },
    {
        .id = "script_thread_init",
        .description = "Обнуление служебных полей потока при сбросе. Вызывается при "
                       "инициализации своего потока, чтобы он выглядел как штатный.",
        .pattern = "83 89 ? 01 00 00 FF 83 A1 ? 01 00 00 F0",
        .offset = 0,
        .resolution = Resolution::Address,
        .expectedMatches = 1,
    },

    // ---------------------------------------------------------------------
    // Точки вмешательства в жизненный цикл скриптов
    // ---------------------------------------------------------------------
    {
        .id = "script_startup_init",
        .description = "Запуск стартовых скриптов игры. Точка, где нужно поднять свой поток, "
                       "иначе он не переживёт перезагрузку сессии.",
        .pattern = "83 FB FF 0F 84 D6 00 00 00",
        .offset = -0x37,
        .resolution = Resolution::Address,
        .expectedMatches = 1,
    },
    {
        .id = "scripts_shutdown",
        .description = "Остановка скриптовой подсистемы. Нужна, чтобы снять свой поток "
                       "до того, как игра начнёт освобождать его память.",
        .pattern = "48 8B 01 FF 50 30 E8 ? ? ? ? E8",
        .offset = -0x61,
        .resolution = Resolution::Address,
        .expectedMatches = 1,
    },
    {
        .id = "script_id_permission_check",
        .description = "Проверка прав скрипта на работу с объектом. Штатно чужие скрипты "
                       "получают отказ, поэтому для своего потока ответ подменяется.",
        .pattern = "74 41 48 8B 01 FF 50 10 84 C0",
        .offset = -0x1A,
        .resolution = Resolution::Address,
        .expectedMatches = 1,
    },
});

} // namespace detail

/// Полный каталог сигнатур.
[[nodiscard]] inline constexpr std::span<const Signature> catalog() noexcept {
    return detail::kSignatures;
}

/// Поиск сигнатуры по идентификатору. Возвращает nullptr, если такой нет.
[[nodiscard]] inline constexpr const Signature* find(std::string_view id) noexcept {
    const auto it = std::ranges::find(detail::kSignatures, id, &Signature::id);
    return it == detail::kSignatures.end() ? nullptr : &*it;
}

} // namespace oxymp::gamesig
