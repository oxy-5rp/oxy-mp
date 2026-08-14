#include <oxymp/script/events.hpp>

#include <algorithm>

namespace oxymp::script {

void Events::subscribe(Listener* listener) {
    if (listener == nullptr) {
        return;
    }

    // Дважды один и тот же не подписывается: иначе он получил бы каждое событие
    // по два раза, а отписка убрала бы только одну из записей.
    if (std::ranges::find(listeners_, listener) != listeners_.end()) {
        return;
    }

    listeners_.push_back(listener);
}

void Events::unsubscribe(Listener* listener) {
    std::erase(listeners_, listener);
}

bool Events::dispatch(const Event& event) {
    // Обход по копии списка. Слушатель вправе отписаться прямо в обработчике —
    // ресурс, остановленный по ошибке внутри собственного события, делает
    // именно это, — а изменение списка во время обхода портит обход.
    const std::vector<Listener*> listening = listeners_;

    for (Listener* listener : listening) {
        // Отписавшийся по ходу рассылки её больше не получает: он уже объявил,
        // что его нет.
        if (std::ranges::find(listeners_, listener) == listeners_.end()) {
            continue;
        }

        if (!listener->handle(event)) {
            // Отменившее событие останавливает рассылку: сообщать следующему о
            // реплике, которой не будет, незачем.
            return false;
        }
    }

    return true;
}

} // namespace oxymp::script
