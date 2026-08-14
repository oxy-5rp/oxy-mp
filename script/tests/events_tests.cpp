// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include "fake_core.hpp"

#include <oxymp/script/events.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace oxymp;
using namespace oxymp::script;
using oxymp::script::testing::FakeCore;

namespace {

/// Слушатель, который только записывает пришедшее.
class Recorder : public Listener {
public:
    explicit Recorder(std::string name, bool allow = true)
        : name_(std::move(name)), allow_(allow) {}

    bool handle(const Event& event) override {
        seen.push_back(event.kind);
        return allow_;
    }

    [[nodiscard]] const std::string& name() const { return name_; }

    std::vector<EventKind> seen;

private:
    std::string name_;
    bool allow_ = true;
};

/// Слушатель, отписывающийся прямо в обработчике.
///
/// Так делает ресурс, остановленный по ошибке внутри собственного события, — и
/// это самый простой способ испортить обход списка изнутри него самого.
class Quitter : public Listener {
public:
    Quitter(Events& events, Recorder& also) : events_(&events), also_(&also) {}

    bool handle(const Event&) override {
        events_->unsubscribe(this);
        events_->unsubscribe(also_);
        return true;
    }

private:
    Events* events_ = nullptr;
    Recorder* also_ = nullptr;
};

} // namespace

TEST_CASE("events reach every listener in order", "[script]") {
    Events events;

    Recorder first{"первый"};
    Recorder second{"второй"};

    events.subscribe(&first);
    events.subscribe(&second);

    CHECK(events.dispatch(Event{.kind = EventKind::PlayerConnect}));
    CHECK(events.dispatch(Event{.kind = EventKind::Tick}));

    REQUIRE(first.seen.size() == 2);
    REQUIRE(second.seen.size() == 2);
    CHECK(first.seen[0] == EventKind::PlayerConnect);
    CHECK(second.seen[1] == EventKind::Tick);
}

TEST_CASE("subscribing twice does not double the delivery", "[script]") {
    Events events;
    Recorder once{"один"};

    events.subscribe(&once);
    events.subscribe(&once);

    CHECK(events.size() == 1);
    CHECK(events.dispatch(Event{.kind = EventKind::Tick}));
    CHECK(once.seen.size() == 1);
}

TEST_CASE("a refusing listener stops the delivery", "[script]") {
    Events events;

    Recorder refuses{"отменяющий", false};
    Recorder after{"следующий"};

    events.subscribe(&refuses);
    events.subscribe(&after);

    // Отменившее событие останавливает рассылку: сообщать следующему о реплике,
    // которой не будет, незачем.
    CHECK_FALSE(events.dispatch(Event{.kind = EventKind::PlayerChat}));

    CHECK(refuses.seen.size() == 1);
    CHECK(after.seen.empty());
}

TEST_CASE("a listener may unsubscribe from inside its own handler", "[script]") {
    Events events;

    Recorder later{"последний"};
    Quitter quitter{events, later};

    events.subscribe(&quitter);
    events.subscribe(&later);

    // Обход идёт по копии списка, поэтому отписка изнутри не портит его. Но
    // отписавшийся по ходу рассылки её больше не получает: он уже объявил, что
    // его нет.
    CHECK(events.dispatch(Event{.kind = EventKind::Tick}));

    CHECK(events.size() == 0);
    CHECK(later.seen.empty());
}

TEST_CASE("an unsubscribed listener hears nothing", "[script]") {
    Events events;
    Recorder gone{"ушедший"};

    events.subscribe(&gone);
    events.unsubscribe(&gone);

    // Отписка того, кого и не было, проходит молча: ресурс вправе отписаться
    // при остановке, не помня, успел ли он подписаться.
    events.unsubscribe(&gone);

    CHECK(events.dispatch(Event{.kind = EventKind::Tick}));
    CHECK(gone.seen.empty());
}

TEST_CASE("an event carries live references", "[script]") {
    FakeCore core;
    core.join(0, "жертва");
    core.join(1, "убийца");

    Event death;
    death.kind = EventKind::PlayerDeath;
    death.player = Player{core, 0};
    death.killer = Player{core, 1};
    death.weapon = 0x1B06D571;

    CHECK(death.player.nickname() == "жертва");
    CHECK(death.killer.nickname() == "убийца");

    // Обработчик, придержавший событие до следующего кадра, получит честное
    // «этого больше нет», а не мусор.
    core.leave(1);

    CHECK(death.player.valid());
    CHECK_FALSE(death.killer.valid());
}
