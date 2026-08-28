#include "console.hpp"

#include <iostream>
#include <utility>

namespace oxymp::server {

Console::Console() : shared_(std::make_shared<Shared>()) {
    reader_ = std::thread{&Console::read, shared_};
}

Console::~Console() {
    shared_->wanted.store(false);

    // Поток отсоединяется, а не дожидается. Дождаться его нельзя: он стоит
    // внутри чтения строки, и прервать это ожидание стандартными средствами
    // нечем — ни закрытием потока ввода, ни признаком. Разбудит его разве что
    // нажатие Enter, а ждать его при выходе сервера значило бы не выйти вовсе.
    //
    // Безопасно это потому, что всё общее с ним живёт под общим владением:
    // переживший нас поток по-прежнему держит свою кучку и складывает в неё
    // строки, которых уже никто не заберёт. Ни замка, ни списка Console к тому
    // мгновению не касается.
    if (reader_.joinable()) {
        reader_.detach();
    }
}

void Console::read(std::shared_ptr<Shared> shared) {
    std::string line;

    while (shared->wanted.load() && std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        const std::lock_guard<std::mutex> held{shared->guard};
        shared->lines.push_back(line);
    }
}

std::vector<std::string> Console::take() {
    const std::lock_guard<std::mutex> held{shared_->guard};

    std::vector<std::string> taken;
    taken.swap(shared_->lines);
    return taken;
}

std::vector<std::string> splitCommand(std::string_view line) {
    std::vector<std::string> parts;

    std::size_t at = 0;

    while (at < line.size()) {
        // Пробелы подряд пропускаются: набранное человеком редко бывает ровным,
        // а пустой довод посреди команды режим принял бы за настоящий.
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
            ++at;
        }

        if (at >= line.size()) {
            break;
        }

        const std::size_t from = at;

        while (at < line.size() && line[at] != ' ' && line[at] != '\t') {
            ++at;
        }

        parts.emplace_back(line.substr(from, at - from));
    }

    // Возврат каретки, доставшийся от чужого перевода строки, обрезается у
    // последнего куска: набранное в Windows и переданное через канал приходит с
    // ним, и команда «stop\r» не совпала бы со «stop» ни разу.
    if (!parts.empty() && !parts.back().empty() && parts.back().back() == '\r') {
        parts.back().pop_back();

        if (parts.back().empty()) {
            parts.pop_back();
        }
    }

    return parts;
}

} // namespace oxymp::server
