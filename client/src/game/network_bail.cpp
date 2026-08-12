#include "network_bail.hpp"

#include "hook.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Доводы выхода из сессии, как они приходят в регистрах.
///
/// Именно так, четырьмя безымянными числами, а не разобранной подписью — и это
/// исправление, оплаченное падением игры.
///
/// Подпись бралась из CitizenFX: для сборок 2372 и новее там `void(int[5], bool)`
/// — указатель на пять чисел, первое из которых причина. Мы её разыменовали, и
/// на нашей сборке 3889 в этом месте оказался не указатель: игра позвала выход
/// через сто семьдесят миллисекунд после подъёма сессии, и клиент упал на чтении
/// по адресу 0x143f3083.
///
/// Вывод общий, а не про этот случай: подпись чужой функции — такое же
/// предположение, как и её адрес, и проверять её нужно так же осторожно.
/// Разыменовывать чужой указатель до того, как выяснено, что он указатель, —
/// значит менять молчаливую неизвестность на вылет.
///
/// Поэтому доводы теперь просто записываются. Что из них причина, выяснится по
/// журналу, а до тех пор мы ничего о них не предполагаем.
using BailFn = void(__fastcall*)(std::uintptr_t first, std::uintptr_t second,
                                 std::uintptr_t third, std::uintptr_t fourth);

/// Перехват один на процесс: подменённая функция — простая функция без
/// состояния, и связать её с объектом иначе нельзя.
struct State {
    Hook hook;

    std::atomic<bool> holding{false};
    std::atomic<std::uint64_t> attempts{0};
};

State* g_state = nullptr;

/// Читает пять чисел по адресу, если по нему вообще можно читать.
///
/// Проверка обязательна: адрес пришёл из чужого регистра, и указателем он может
/// не быть вовсе. Спрашивается это у Windows — она одна знает, отображена ли
/// страница и разрешено ли её читать.
bool peek(std::uintptr_t address, std::array<std::int32_t, 5>& values) {
    if (address < 0x10000) {
        return false;
    }

    MEMORY_BASIC_INFORMATION region{};
    if (::VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) == 0) {
        return false;
    }

    if (region.State != MEM_COMMIT) {
        return false;
    }

    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY;

    if ((region.Protect & kReadable) == 0 || (region.Protect & PAGE_GUARD) != 0) {
        return false;
    }

    // Хватает ли места до конца области: чтение, начатое в конце последней
    // страницы, ушло бы за неё.
    const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (end - address < sizeof(values)) {
        return false;
    }

    std::memcpy(values.data(), reinterpret_cast<const void*>(address), sizeof(values));
    return true;
}

void __fastcall bailDetour(std::uintptr_t first, std::uintptr_t second, std::uintptr_t third,
                           std::uintptr_t fourth) {
    State* const state = g_state;
    if (state == nullptr) {
        return;
    }

    const std::uint64_t attempt = state->attempts.fetch_add(1) + 1;

    // Первые попытки записываются каждая, дальше — каждая сотая. Причина у них
    // почти всегда одна и та же, и повторять её тысячу раз в журнале незачем; а
    // совсем замолчать нельзя — по этим строкам видно, сдалась игра или
    // продолжает ломиться наружу.
    const bool interesting = attempt <= 5 || attempt % 100 == 0;

    if (interesting) {
        spdlog::info("игра выходит из сессии (попытка {}): доводы {:#x} {:#x} {:#x} {:#x}",
                     attempt, first, second, third, fourth);

        // Если первый довод всё-таки указатель — покажем, что по нему лежит.
        // Не для того, чтобы на это полагаться, а чтобы разобрать подпись по
        // журналу: причина выхода где-то среди этих чисел.
        if (std::array<std::int32_t, 5> values{}; peek(first, values)) {
            spdlog::info("  по первому доводу лежит: {} {} {} {} {}", values[0], values[1],
                         values[2], values[3], values[4]);
        }
    }

    if (!state->holding.load()) {
        state->hook.original<BailFn>()(first, second, third, fourth);
        return;
    }

    if (interesting) {
        spdlog::info("  не выпускаем: держим игру в сессии");
    }
}

} // namespace

std::unique_ptr<NetworkBail> NetworkBail::install(const EngineAddresses& addresses,
                                                  std::string& error) {
    if (g_state != nullptr) {
        error = "перехват выхода из сессии уже стоит";
        return nullptr;
    }

    void* const target = addresses.pointerTo<void*>("network_bail");
    if (target == nullptr) {
        error = "адрес выхода из сессии не разрешился";
        return nullptr;
    }

    std::unique_ptr<NetworkBail> bail{new NetworkBail};
    auto state = std::make_unique<State>();

    // Порядок обязателен: подменённая функция вправе быть вызвана сразу же.
    g_state = state.get();

    if (!state->hook.install(target, reinterpret_cast<void*>(&bailDetour), error)) {
        g_state = nullptr;
        return nullptr;
    }

    // Состояние переживает объект намеренно: перехват снимается в разрушителе,
    // но подменённая функция может быть в этот миг на середине работы.
    state.release();

    spdlog::info("перехват выхода из сессии поставлен: {:#x}",
                 reinterpret_cast<std::uintptr_t>(target));

    return bail;
}

NetworkBail::~NetworkBail() {
    if (g_state == nullptr) {
        return;
    }

    // Держать больше не нужно, а перехват остаётся стоять: снимут его вместе со
    // всем механизмом перехвата, который умеет останавливать потоки и выносить
    // их из подменённого кода. Сними мы его здесь — адрес исходной функции
    // обнулился бы под ногами у того, кто прямо сейчас внутри неё.
    g_state->holding.store(false);

    spdlog::info("игру больше не держим в сессии, попыток выйти было: {}",
                 g_state->attempts.load());
}

void NetworkBail::hold(bool holding) {
    if (g_state == nullptr) {
        return;
    }

    if (g_state->holding.exchange(holding) == holding) {
        return;
    }

    spdlog::info("удержание в сессии: {}", holding ? "включено" : "выключено");
}

std::uint64_t NetworkBail::attempts() const noexcept {
    return g_state != nullptr ? g_state->attempts.load() : 0;
}

} // namespace oxymp::client::game
