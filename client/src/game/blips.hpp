#pragma once

#include "native_table.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <unordered_map>
#include <vector>

namespace oxymp::client::game {

/// Метки на карте, поставленные сервером.
///
/// Метка — не сущность мира, а строчка в списке игры: у неё нет ни тела, ни
/// положения в физическом смысле, и живёт она ровно до тех пор, пока её не
/// уберут. Отсюда и устройство: словарь «номер сессии → описатель игры», и
/// ничего больше.
///
/// Расстоянием метки не отбираются, в отличие от машин и предметов: метка на то
/// и метка, что видна на карте целиком. Поэтому сервер шлёт их разом при входе,
/// а дальше — по изменению.
///
/// Вызывать можно только изнутри скриптового тика.
class Blips {
public:
    explicit Blips(const NativeTable& table) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    /// Заводит метку или поправляет уже заведённую.
    ///
    /// Одним действием на то и другое, потому что таким его и присылает сервер:
    /// помнить, знаем ли мы уже эту метку, ему негде, а нам — просто.
    void apply(const shared::BlipState& state);

    /// Убирает метку. Молча, если её и не было: сообщение о снятой метке
    /// вправе прийти дважды.
    void remove(shared::BlipId id);

    /// Убирает все метки разом. Нужно при разрыве: метки прежнего сервера на
    /// карте следующего — мусор.
    void clear();

private:
    /// Подпись метке ставится не одним вызовом, а тремя: игра собирает строку
    /// из кусков, как и всякий свой текст.
    void rename(int blip, const std::string& name) const;

    NativeHandler add_ = nullptr;
    NativeHandler removeBlip_ = nullptr;
    NativeHandler exists_ = nullptr;
    NativeHandler setSprite_ = nullptr;
    NativeHandler setColour_ = nullptr;
    NativeHandler setAlpha_ = nullptr;
    NativeHandler setScale_ = nullptr;
    NativeHandler setDisplay_ = nullptr;
    NativeHandler setShortRange_ = nullptr;
    NativeHandler beginName_ = nullptr;
    NativeHandler addNamePart_ = nullptr;
    NativeHandler endName_ = nullptr;

    /// Номер сессии — описатель игры. Описатель у каждого игрока свой, и
    /// хранить его может только тот, у кого метка заведена.
    std::unordered_map<shared::BlipId, int> shown_;
};

} // namespace oxymp::client::game
