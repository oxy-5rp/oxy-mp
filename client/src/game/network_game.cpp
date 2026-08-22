#include "network_game.hpp"

#include <spdlog/spdlog.h>

namespace oxymp::client::game {
namespace {

constexpr std::string_view kEstablishedId = "network_game_flag";
constexpr std::string_view kConnectingId = "netgame_state_flag";

/// Значение, которым признак выставляется. Игра хранит здесь обычный булев байт.
constexpr std::uint8_t kSet = 1;

} // namespace

const char* describe(NetworkGame::Flag flag) noexcept {
    switch (flag) {
    case NetworkGame::Flag::Established:
        return "сетевая игра";
    case NetworkGame::Flag::Connecting:
        return "сетевая игра с подключением";
    }
    return "?";
}

NetworkGame::NetworkGame(const EngineAddresses& addresses) noexcept {
    established_.address = addresses.pointerTo<volatile std::uint8_t*>(kEstablishedId);
    connecting_.address = addresses.pointerTo<volatile std::uint8_t*>(kConnectingId);

    for (const Flag flag : {Flag::Established, Flag::Connecting}) {
        const Byte& byte = byteFor(flag);

        if (byte.address == nullptr) {
            spdlog::warn("flag \"{}\" was not found: it cannot be faked", describe(flag));
            continue;
        }

        spdlog::debug("признак «{}» найден по {:#x}, игра держит в нём {}", describe(flag),
                     reinterpret_cast<std::uintptr_t>(byte.address), *byte.address);
    }
}

NetworkGame::Byte& NetworkGame::byteFor(Flag flag) noexcept {
    return flag == Flag::Established ? established_ : connecting_;
}

const NetworkGame::Byte& NetworkGame::byteFor(Flag flag) const noexcept {
    return flag == Flag::Established ? established_ : connecting_;
}

bool NetworkGame::available(Flag flag) const noexcept {
    return byteFor(flag).address != nullptr;
}

bool NetworkGame::ready() const noexcept {
    return available(Flag::Established) || available(Flag::Connecting);
}

const void* NetworkGame::addressOf(Flag flag) const noexcept {
    return const_cast<const std::uint8_t*>(byteFor(flag).address);
}

bool NetworkGame::value(Flag flag) const noexcept {
    const Byte& byte = byteFor(flag);
    return byte.address != nullptr && *byte.address != 0;
}

bool NetworkGame::write(Byte& byte, std::uint8_t value, Flag flag) {
    if (byte.address == nullptr || byte.writeFailed) {
        return false;
    }

    *byte.address = value;

    // Чтение обратно, а не доверие записи. Признак лежит в данных игры, и если
    // страница окажется защищённой от записи, обычное присваивание не бросит
    // исключения — оно просто ничего не изменит, и мы бы искали причину в
    // сигнатуре, которая на самом деле верна.
    if (*byte.address == value) {
        return true;
    }

    spdlog::error("flag \"{}\" at {:#x} does not stick: wrote {}, reads {}", describe(flag),
                  reinterpret_cast<std::uintptr_t>(byte.address), value, *byte.address);

    byte.writeFailed = true;
    return false;
}

void NetworkGame::beginHolding(Fake what) noexcept {
    if (holding_ || what == Fake::None) {
        return;
    }

    established_.held = true;
    connecting_.held = what == Fake::Both;

    for (const Flag flag : {Flag::Established, Flag::Connecting}) {
        Byte& byte = byteFor(flag);
        if (byte.address == nullptr || !byte.held || byte.originalKnown) {
            continue;
        }

        byte.original = *byte.address;
        byte.originalKnown = true;
    }

    holding_ = true;

    spdlog::warn("faking the network state is on ({}): this is a probe, not a session — "
                 "the game will take network branches without any session objects",
                 what == Fake::Both ? "both flags" : "the settled flag only");
}

void NetworkGame::hold() {
    if (!holding_) {
        return;
    }

    for (const Flag flag : {Flag::Established, Flag::Connecting}) {
        Byte& byte = byteFor(flag);

        if (byte.address == nullptr || !byte.held || byte.writeFailed || *byte.address == kSet) {
            continue;
        }

        write(byte, kSet, flag);
    }
}

void NetworkGame::endHolding() {
    if (!holding_) {
        return;
    }

    holding_ = false;

    for (const Flag flag : {Flag::Established, Flag::Connecting}) {
        Byte& byte = byteFor(flag);
        if (byte.address == nullptr || !byte.held || !byte.originalKnown) {
            continue;
        }

        byte.held = false;
        write(byte, byte.original, flag);
    }

    spdlog::debug("подделка сетевого состояния снята, признакам возвращены прежние значения");
}

void NetworkGame::reportChanges() {
    for (const Flag flag : {Flag::Established, Flag::Connecting}) {
        Byte& byte = byteFor(flag);
        if (byte.address == nullptr) {
            continue;
        }

        const std::uint8_t now = *byte.address;

        if (byte.lastSeenKnown && now == byte.lastSeen) {
            continue;
        }

        // Первое наблюдение — не переход, а точка отсчёта, и в журнале ему место
        // отдельное: иначе не отличить «игра выставила признак» от «мы впервые
        // посмотрели».
        if (!byte.lastSeenKnown) {
            spdlog::debug("признак «{}»: сейчас {}", describe(flag), now);
        } else {
            spdlog::debug("признак «{}»: {} → {}{}", describe(flag), byte.lastSeen, now,
                         holding_ ? " (при включённой подделке)" : "");
        }

        byte.lastSeen = now;
        byte.lastSeenKnown = true;
    }
}

} // namespace oxymp::client::game
