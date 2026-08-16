#include "custom_text.hpp"

#include "code_patch.hpp"

#include <oxymp/shared/math/joaat.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace oxymp::client::game {

struct CustomText::State {
    /// Свои надписи по хешу имени.
    ///
    /// Читает поток игры на каждой надписи, пишем мы — редко и не в кадре.
    /// Отсюда и разделяемая блокировка: чтения не мешают друг другу.
    std::shared_mutex mutex;
    std::unordered_map<std::uint32_t, std::string> texts;

    /// Настоящий словарь игры. Всё, чего у нас нет, спрашивается у него.
    const char* (*original)(void*, std::uint32_t) = nullptr;

    /// Сказали ли уже, что подмена сработала.
    std::atomic<bool> told{false};
};

CustomText::State* CustomText::active_ = nullptr;

const char* CustomText::lookUp(void* dictionary, std::uint32_t hash) {
    State* const state = active_;
    if (state == nullptr) {
        return nullptr;
    }

    {
        const std::shared_lock guard{state->mutex};

        if (const auto found = state->texts.find(hash); found != state->texts.end()) {
            // Одна запись за запуск, и она не лишняя: проверить подмену глазами
            // можно только открыв меню паузы, а до него игрок доходит не сразу.
            // Без этой строки «в меню по-прежнему GTA ONLINE» одинаково означало
            // бы и «перехват не встал», и «встал, но не туда».
            if (!state->told.exchange(true)) {
                spdlog::info("подмена надписей работает: игра спросила первую нашу");
            }

            return found->second.c_str();
        }
    }

    // Всё, чего мы не подменяли, отдаёт сама игра. Ответить своим на всё —
    // значит стереть весь её текст разом.
    return state->original != nullptr ? state->original(dictionary, hash) : nullptr;
}

std::unique_ptr<CustomText> CustomText::install(const EngineAddresses& addresses,
                                                std::string& error) {
    if (active_ != nullptr) {
        error = "подмена надписей уже стоит";
        return nullptr;
    }

    auto* const first = addresses.pointerTo<std::uint8_t*>("game_text_lookup_call");
    auto* const second = addresses.pointerTo<std::uint8_t*>("game_text_lookup_call_alt");

    if (first == nullptr || second == nullptr) {
        error = "места, где игра спрашивает свои надписи, не разрешены";
        return nullptr;
    }

    auto state = std::make_unique<State>();

    // Указатель ставится до правки: с этого мгновения игра вправе спросить
    // надпись в любой миг, и спросить ей нужно у готового.
    active_ = state.get();

    void* original = nullptr;
    if (!code::redirectCall(first, reinterpret_cast<const void*>(&CustomText::lookUp), &original,
                            error)) {
        active_ = nullptr;
        return nullptr;
    }

    state->original = reinterpret_cast<const char* (*)(void*, std::uint32_t)>(original);

    // Второе место — без перехвата прежней цели: она та же самая, и записывать
    // её дважды значило бы поверить, что сигнатуры указывают в одно место, не
    // проверив этого.
    void* secondOriginal = nullptr;
    if (!code::redirectCall(second, reinterpret_cast<const void*>(&CustomText::lookUp),
                            &secondOriginal, error)) {
        // Первое место уже уведено, и вернуть его нечем — но беды в этом нет:
        // подмена работает, просто не везде.
        spdlog::warn("второе место подмены надписей не уведено: {}", error);
    } else if (secondOriginal != original) {
        spdlog::warn("два места спрашивают надписи по разным адресам: {:#x} и {:#x}",
                     reinterpret_cast<std::uintptr_t>(original),
                     reinterpret_cast<std::uintptr_t>(secondOriginal));
    }

    spdlog::info("подмена надписей игры поставлена, словарь игры на {:#x}",
                 reinterpret_cast<std::uintptr_t>(original));

    auto text = std::unique_ptr<CustomText>{new CustomText};
    text->state_ = std::move(state);
    return text;
}

CustomText::~CustomText() {
    // Правки не снимаются, и это намеренно. Вернуть вызовы на место можно, но
    // разрушается подмена вместе с клиентом — то есть при закрытии игры, — а
    // между снятием и выгрузкой поток игры успел бы спросить надпись у уже
    // мёртвой карты. Оставленная правка в этот миг честнее: карта пуста, и
    // всё уходит в игру.
    active_ = nullptr;
}

void CustomText::set(std::string_view key, std::string value) {
    const std::uint32_t hash = shared::joaat(key);

    {
        const std::unique_lock guard{state_->mutex};
        state_->texts[hash] = std::move(value);
    }

    spdlog::debug("надпись {} подменена", key);
}

} // namespace oxymp::client::game
