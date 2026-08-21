#pragma once

#include "native_table.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace oxymp::client::game {

/// Предметы сессии в игровом мире.
///
/// Устроены заметно проще машин, и это следует из того, чем они являются. У
/// машины есть ведущий — клиент, считающий её физику, — потому что машина едет,
/// мнётся и переворачивается. Предмет стоит там, где его поставили, и всё, что о
/// нём нужно знать, приходит один раз при появлении.
///
/// Отсюда и весь этот класс: список, заведение по модели, уборка. Ни снимков, ни
/// интерполяции, ни ведущего — сравнивать здесь нечего и догонять нечего.
///
/// Замораживаются намеренно и все. Предмет, оставленный на попечение физики,
/// сползёт по уклону, а от столкновения с машиной укатится — и укатится у
/// каждого по-своему, потому что считать его будет каждый у себя. Неподвижный
/// одинаков у всех без всякой пересылки.
///
/// Вызывать можно только изнутри скриптового тика.
class Objects {
public:
    explicit Objects(const NativeTable& table) noexcept;
    ~Objects();

    Objects(const Objects&) = delete;
    Objects& operator=(const Objects&) = delete;

    [[nodiscard]] bool ready() const noexcept;

    /// Заводит предмет, о котором объявил сервер.
    ///
    /// Может не удаться с первого раза — модель грузится не мгновенно. Тогда
    /// предмет остаётся в списке незаведённым, и попытка повторится в следующем
    /// кадре: sync для того и вызывается каждый кадр.
    void add(const shared::ObjectAdded& object);

    /// Убирает предмет.
    void remove(shared::ObjectId id);

    /// Доводит до конца то, что не удалось раньше. Вызывать раз в кадр.
    void sync();

    /// Убирает все предметы. Нужно при выходе.
    void clear();

    /// Номер предмета в игре. Ноль — предмета здесь нет: модель ещё грузится
    /// либо о нём не объявляли вовсе.
    ///
    /// Нужен привязке: она связывает две сущности, а игра знает их описателями.
    [[nodiscard]] int handleFor(shared::ObjectId id) const;

    [[nodiscard]] std::size_t shown() const noexcept { return objects_.size(); }

private:
    /// Предмет и всё, что мы о нём помним.
    struct Entry {
        std::uint32_t model = 0;
        shared::Vec3 position;
        shared::Vec3 rotation;

        /// Номер в игре. Ноль — модель ещё не загрузилась, предмета пока нет.
        int handle = 0;
    };

    /// Заводит предмет в мире. Ноль, если модель ещё не загрузилась.
    [[nodiscard]] int spawn(const Entry& entry);

    void destroy(int handle) const;

    NativeHandler requestModel_ = nullptr;
    NativeHandler hasModelLoaded_ = nullptr;
    NativeHandler modelNoLongerNeeded_ = nullptr;
    NativeHandler createObject_ = nullptr;
    NativeHandler deleteObject_ = nullptr;
    NativeHandler doesExist_ = nullptr;
    NativeHandler setRotation_ = nullptr;
    NativeHandler freezePosition_ = nullptr;
    NativeHandler asMissionEntity_ = nullptr;
    NativeHandler lodDistance_ = nullptr;

    std::unordered_map<shared::ObjectId, Entry> objects_;
};

} // namespace oxymp::client::game
