#include "streaming_files.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <iterator>

namespace oxymp::client::game {
namespace {

/// Сколько времени за кадр отдаётся объявлению файлов.
///
/// Мерой служит время, а не число файлов, и это существенно: файлы разной
/// тяжести — модель игра только открывает, а описание разбирает, — и одно и то
/// же число вышло бы то мгновенным, то на секунды.
///
/// Порция вообще нужна вот почему. Объявляемый файл игра открывает тут же,
/// чтобы прочесть его размер и раскладку страниц; на чужой карте таких файлов
/// тысячи, и вся пачка в одном кадре — это кадр длиной в секунды. Со стороны
/// это ровно то, что человек называет «игра зависла».
///
/// Четыре миллисекунды — четверть кадра при шестидесяти в секунду: подгрузка
/// чужой карты растягивается на доли секунды и не отнимает ни одного кадра
/// целиком.
constexpr auto kFrameBudget = std::chrono::milliseconds{4};

} // namespace

std::unique_ptr<StreamingFiles> StreamingFiles::create(const EngineAddresses& addresses,
                                                       std::string& error) {
    const auto entry = addresses.pointerTo<RegisterRawFile>("streaming_register_raw_file");
    if (entry == nullptr) {
        error = "адрес объявления файлов стриминга не разрешился";
        return nullptr;
    }

    std::unique_ptr<StreamingFiles> owner{new StreamingFiles};
    owner->register_ = entry;
    return owner;
}

void StreamingFiles::add(std::string path, std::string name) {
    const std::lock_guard guard{mutex_};

    // Отмечаем и спрашиваем одним движением: вставка отвечает, было ли имя
    // здесь раньше, и второго прохода по множеству не нужно.
    if (!known_.insert(name).second) {
        return;
    }

    pending_.push_back(Wanted{.path = std::move(path), .name = std::move(name)});
}

std::size_t StreamingFiles::pump() {
    if (register_ == nullptr) {
        return 0;
    }

    std::vector<Wanted> batch;

    {
        const std::lock_guard guard{mutex_};
        batch.swap(pending_);
    }

    std::size_t taken = 0;
    std::size_t done = 0;

    const auto started = std::chrono::steady_clock::now();

    for (const Wanted& file : batch) {
        // Начальное значение обязано быть «нет номера»: игра пишет сюда только
        // при удаче, а при неудаче оставляет как было. Оставь мы здесь ноль — и
        // отказ выглядел бы как выданный нулевой номер, который у неё законен.
        std::uint32_t slot = kNoSlot;

        register_(&slot, file.path.c_str(), true, file.name.c_str(), false);
        ++done;

        if (slot == kNoSlot) {
            spdlog::warn("the game did not take file {} under the name {}", file.path, file.name);
        } else {
            ++taken;

            spdlog::debug("игре объявлен файл {} под именем {} (место {})", file.path, file.name,
                          slot);
        }

        // Хотя бы один файл за кадр объявляется всегда, даже если срок вышел
        // ещё до начала: иначе очередь не тронулась бы с места на слабой машине.
        if (std::chrono::steady_clock::now() - started >= kFrameBudget) {
            break;
        }
    }

    // Недоделанное возвращается в начало очереди: порядок объявления задан не
    // нами, а зависимостями внутри чужого ресурса, и переставлять его нельзя.
    if (done < batch.size()) {
        const std::lock_guard guard{mutex_};

        pending_.insert(pending_.begin(), std::make_move_iterator(batch.begin() + done),
                        std::make_move_iterator(batch.end()));
    }

    return taken;
}

} // namespace oxymp::client::game
