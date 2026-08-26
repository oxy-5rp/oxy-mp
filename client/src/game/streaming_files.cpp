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

/// Порция, пока игрок не в мире.
///
/// Вдесятеро больше обычной, и это не небрежность, а исправление по журналу.
/// Порция в четыре миллисекунды хороша в игре и губительна на загрузке: там
/// кадры идут по сотне-другой миллисекунд, и объявление трёх тысяч файлов чужой
/// карты растягивалось на **тридцать девять секунд**. Всё это время игра
/// грузила мир сама — и к нашему файлу успевала завести свой с тем же именем.
/// Дальше она отвечала отказом, и модель молча не появлялась.
///
/// Отказы эти и были «через раз»: в быстрых заходах объявление укладывалось в
/// секунду и не терялось ни одного файла, в медленных терялся десяток. Разница
/// между заходами — только в том, насколько игре хватало кадров.
///
/// Полсотни миллисекунд — предел, за которым перестаёт шевелиться наш экран
/// загрузки: он рисуется тем же кадром, и двадцати в секунду ему довольно.
/// Игрока за ним всё равно нет — он ещё не в мире, и подвисать нечему.
constexpr auto kLoadingBudget = std::chrono::milliseconds{50};

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

std::size_t StreamingFiles::pump(bool playerInWorld) {
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

    const auto budget = playerInWorld ? kFrameBudget : kLoadingBudget;
    const auto started = std::chrono::steady_clock::now();

    for (const Wanted& file : batch) {
        // Начальное значение обязано быть «нет номера»: игра пишет сюда только
        // при удаче, а при неудаче оставляет как было. Оставь мы здесь ноль — и
        // отказ выглядел бы как выданный нулевой номер, который у неё законен.
        std::uint32_t slot = kNoSlot;

        register_(&slot, file.path.c_str(), true, file.name.c_str(), false);
        ++done;

        if (slot == kNoSlot) {
            ++refused_;

            // Отказ у этого вызова один и тот же на все причины, и разобрать
            // его нечем. Но причина у него по опыту одна: имя уже занято — либо
            // файлом самой игры, либо тем, что она успела завести, пока мы
            // объявляли. Поэтому в строке названо и имя, и путь: по имени видно,
            // с чем оно столкнулось, по пути — чей файл потерялся.
            spdlog::warn("the game did not take file {} under the name {}: the name is most likely "
                         "taken by the game's own asset",
                         file.path, file.name);
        } else {
            ++taken;

            spdlog::debug("игре объявлен файл {} под именем {} (место {})", file.path, file.name,
                          slot);
        }

        // Хотя бы один файл за кадр объявляется всегда, даже если срок вышел
        // ещё до начала: иначе очередь не тронулась бы с места на слабой машине.
        if (std::chrono::steady_clock::now() - started >= budget) {
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

std::size_t StreamingFiles::refused() const noexcept {
    const std::lock_guard guard{mutex_};
    return refused_;
}

} // namespace oxymp::client::game
