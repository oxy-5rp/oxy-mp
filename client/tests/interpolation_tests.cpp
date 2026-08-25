// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oxymp/client/interpolation.hpp>

#include <cmath>

using namespace oxymp::client::interpolation;
using Catch::Approx;

namespace {

/// Снимок, у которого есть только отметка времени и опознавательный знак.
///
/// Ленте больше ничего и не нужно: она раскладывает снимки по времени отправки
/// и находит пару, между которой лежит показываемое мгновение. Что внутри
/// снимка — дело того, кто его читает.
struct Mark {
    oxymp::shared::Timestamp sentAt = 0;
    int tag = 0;
};

/// Сколько прошло у нас с прихода последнего снимка.
std::chrono::milliseconds since(int milliseconds) {
    return std::chrono::milliseconds{milliseconds};
}

/// На сколько показываем позади настоящего времени.
///
/// В проверках отставание задаётся числом, а не берётся из оценки: проверяется
/// расчёт доли, и подмешивать к нему ещё и оценку значило бы проверять сразу
/// двоих.
std::chrono::milliseconds delay(int milliseconds) {
    return std::chrono::milliseconds{milliseconds};
}

} // namespace

TEST_CASE("a timeline shows the instant we ask for", "[interpolation]") {
    // Главная проверка и причина, по которой лента вообще появилась. Пары
    // «последний и предыдущий» здесь не хватило бы: показываемое мгновение
    // отстаёт на сто миллисекунд, а снимки идут через пятьдесят — значит оно
    // лежит не между двумя последними, а на отрезок раньше. Пара упёрлась бы в
    // край и замерла там до прихода следующего снимка, а движение шло бы
    // ступеньками по целому промежутку.
    Timeline<Mark> line;

    for (int number = 0; number < 5; ++number) {
        line.accept(Mark{.sentAt = static_cast<oxymp::shared::Timestamp>(1000 + number * 50),
                         .tag = number});
    }

    // Последний снимок — номер 4, отметка 1200. Только что пришёл, а показываем
    // мы мгновение на сто миллисекунд позади него: отметку 1100, то есть ровно
    // снимок номер 2.
    const auto sample = line.at(since(0), delay(100));

    REQUIRE(sample.from != nullptr);
    CHECK(sample.from->tag == 2);
    CHECK(sample.to->tag == 3);
    CHECK(sample.progress == Approx(0.0F));
    CHECK(sample.ahead == 0.0F);
}

TEST_CASE("a timeline sweeps the whole segment", "[interpolation]") {
    // И второе следствие того же: за время между снимками показываемое
    // мгновение обязано пройти отрезок целиком, от начала до конца. Пара
    // проходила его на пятую часть.
    Timeline<Mark> line;

    for (int number = 0; number < 5; ++number) {
        line.accept(Mark{.sentAt = static_cast<oxymp::shared::Timestamp>(1000 + number * 50),
                         .tag = number});
    }

    const auto begun = line.at(since(0), delay(100));
    const auto middle = line.at(since(25), delay(100));
    const auto ending = line.at(since(49), delay(100));
    const auto ended = line.at(since(50), delay(100));

    CHECK(begun.progress == Approx(0.0F));
    CHECK(begun.from->tag == 2);

    CHECK(middle.progress == Approx(0.5F));
    CHECK(middle.from->tag == 2);
    CHECK(middle.to->tag == 3);

    // К концу промежутка отрезок пройден целиком.
    CHECK(ending.from->tag == 2);
    CHECK(ending.progress == Approx(0.98F));

    // А ровно на границе показываемое мгновение совпадает со снимком, и
    // следующий отрезок начинается с того же места — без рывка.
    CHECK(ended.from->tag == 3);
    CHECK(ended.progress == Approx(0.0F));
}

TEST_CASE("a timeline gives the length of the segment it found", "[interpolation]") {
    // Длину отрезка спрашивает кривая: касательная измеряется в пути за весь
    // отрезок, а скорость приходит в пути за секунду.
    Timeline<Mark> line;
    line.accept(Mark{.sentAt = 1000, .tag = 0});
    line.accept(Mark{.sentAt = 1080, .tag = 1});
    line.accept(Mark{.sentAt = 1120, .tag = 2});

    const auto sample = line.at(since(0), delay(60));

    CHECK(sample.from->tag == 0);
    CHECK(sample.to->tag == 1);
    CHECK(sample.span == Approx(0.08F));
}

TEST_CASE("a timeline extrapolates when snapshots stop coming", "[interpolation]") {
    // С прихода последнего снимка прошло 250 мс при отставании в 100: свежих
    // нет полтораста миллисекунд, и движение приходится достраивать.
    Timeline<Mark> line;
    line.accept(Mark{.sentAt = 1000, .tag = 0});
    line.accept(Mark{.sentAt = 1050, .tag = 1});

    const auto sample = line.at(since(250), delay(100));

    CHECK(sample.ahead == Approx(0.15F));
    CHECK(sample.to->tag == 1);
}

TEST_CASE("a timeline refuses to extrapolate forever", "[interpolation]") {
    // Связь пропала надолго. Без предела чужой игрок уехал бы в бесконечность
    // по последней известной скорости.
    Timeline<Mark> line;
    line.accept(Mark{.sentAt = 1000, .tag = 0});
    line.accept(Mark{.sentAt = 1050, .tag = 1});

    const auto sample = line.at(since(60'000), delay(100));

    const float limit = std::chrono::duration<float>{kMaxExtrapolation}.count();
    CHECK(sample.ahead == Approx(limit));
}

TEST_CASE("a timeline survives a single snapshot", "[interpolation]") {
    // Первый снимок игрока: смешивать не с чем, а показать его нужно.
    Timeline<Mark> line;
    line.accept(Mark{.sentAt = 1000, .tag = 7});

    const auto sample = line.at(since(0), delay(100));

    REQUIRE(sample.from != nullptr);
    CHECK(sample.from == sample.to);
    CHECK(sample.to->tag == 7);
    CHECK(sample.ahead == 0.0F);
}

TEST_CASE("a timeline that does not reach back shows its oldest", "[interpolation]") {
    // Сущность появилась только что, а показываем мы мгновение на сто
    // миллисекунд позади: столько истории у ленты ещё нет. Самый старый снимок
    // ближе всего к правде — и он же не даёт показать её в начале координат.
    Timeline<Mark> line;
    line.accept(Mark{.sentAt = 1000, .tag = 0});
    line.accept(Mark{.sentAt = 1020, .tag = 1});

    const auto sample = line.at(since(0), delay(100));

    CHECK(sample.from == sample.to);
    CHECK(sample.to->tag == 0);
}

TEST_CASE("a snapshot that overtook its predecessor keeps its place", "[interpolation]") {
    // Канал ненадёжный и порядка не обещает. Прежде обогнанный снимок
    // выбрасывался целиком — и отрезок вместе с ним; теперь он встаёт на своё
    // место по времени отправки.
    Timeline<Mark> line;
    line.accept(Mark{.sentAt = 1000, .tag = 0});
    line.accept(Mark{.sentAt = 1100, .tag = 2});

    CHECK(line.accept(Mark{.sentAt = 1050, .tag = 1}));
    REQUIRE(line.size() == 3);

    const auto sample = line.at(since(0), delay(50));

    CHECK(sample.from->tag == 1);
    CHECK(sample.to->tag == 2);
}

TEST_CASE("a timeline refuses what it already has", "[interpolation]") {
    // Тот же снимок пришёл дважды. Промежуток нулевой длины ничего не
    // описывает, а второй такой на ленте дал бы деление на ноль в расчёте доли.
    Timeline<Mark> line;
    line.accept(Mark{.sentAt = 1000, .tag = 0});
    line.accept(Mark{.sentAt = 1050, .tag = 1});

    CHECK_FALSE(line.accept(Mark{.sentAt = 1050, .tag = 9}));
    CHECK(line.size() == 2);
}

TEST_CASE("a timeline refuses what is older than it remembers", "[interpolation]") {
    // Снимок отстал за начало ленты: показывать его уже негде.
    Timeline<Mark> line;
    line.accept(Mark{.sentAt = 1000, .tag = 0});
    line.accept(Mark{.sentAt = 1050, .tag = 1});

    CHECK_FALSE(line.accept(Mark{.sentAt = 900, .tag = 9}));
    CHECK(line.size() == 2);
}

TEST_CASE("a timeline holds only so much", "[interpolation]") {
    // Память эта — на каждого чужого игрока и на каждую машину сессии, и расти
    // ей нельзя. Выбрасывается при этом самый старый: показывать его уже
    // незачем, отставание до него не достаёт.
    Timeline<Mark> line;

    for (int number = 0; number < 40; ++number) {
        line.accept(Mark{.sentAt = static_cast<oxymp::shared::Timestamp>(1000 + number * 30),
                         .tag = number});
    }

    CHECK(line.size() == kTimelineDepth);
    CHECK(line.newest().tag == 39);
}

TEST_CASE("a full timeline still takes what overtook its neighbour", "[interpolation]") {
    // Самая редкая ветвь ленты и оттого самая опасная: место снимку нашлось в
    // середине, а класть его некуда — лента полна. Тогда выбрасывается самый
    // старый, и место сдвигается вместе с остальными.
    Timeline<Mark> line;

    // Лента наполняется через один: между соседями остаётся щель, в которую
    // потом и придёт обогнанный снимок.
    for (int number = 0; number < static_cast<int>(kTimelineDepth); ++number) {
        line.accept(Mark{.sentAt = static_cast<oxymp::shared::Timestamp>(1000 + number * 40),
                         .tag = number});
    }

    REQUIRE(line.size() == kTimelineDepth);

    const int last = static_cast<int>(kTimelineDepth) - 1;
    const auto middle = static_cast<oxymp::shared::Timestamp>(1000 + (last - 1) * 40 + 20);

    CHECK(line.accept(Mark{.sentAt = middle, .tag = 99}));

    // Лента не выросла, самый свежий остался самым свежим, а пришедший встал
    // между двумя последними.
    CHECK(line.size() == kTimelineDepth);
    CHECK(line.newest().tag == last);

    const auto sample = line.at(since(0), delay(20));

    CHECK(sample.from->tag == 99);
    CHECK(sample.to->tag == last);
}

TEST_CASE("a full timeline refuses what belongs at its very start", "[interpolation]") {
    // Место нашлось только в самом начале, а начало и выбрасывается: класть
    // снимок, который тут же уедет, незачем — и уж точно нельзя выбрасывать
    // ради него настоящий.
    Timeline<Mark> line;

    for (int number = 0; number < static_cast<int>(kTimelineDepth); ++number) {
        line.accept(Mark{.sentAt = static_cast<oxymp::shared::Timestamp>(1000 + number * 40),
                         .tag = number});
    }

    CHECK_FALSE(line.accept(Mark{.sentAt = 1020, .tag = 99}));
    CHECK(line.size() == kTimelineDepth);
}

TEST_CASE("a timeline survives the timestamp going round", "[interpolation]") {
    // Отметки времени ходят по кругу за сорок девять суток. Сравнение как чисел
    // решило бы, что весь дальнейший поток пришёл из прошлого, и движение
    // встало бы намертво.
    Timeline<Mark> line;

    constexpr auto kNearlyRound = static_cast<oxymp::shared::Timestamp>(0xFFFF'FFD0U);

    line.accept(Mark{.sentAt = kNearlyRound, .tag = 0});
    CHECK(line.accept(Mark{.sentAt = static_cast<oxymp::shared::Timestamp>(kNearlyRound + 50),
                           .tag = 1}));
    CHECK(line.newest().tag == 1);

    const auto sample = line.at(since(0), delay(50));

    CHECK(sample.from->tag == 0);
    CHECK(sample.to->tag == 1);
}

TEST_CASE("a change of sender leaves one snapshot", "[interpolation]") {
    // Машина меняет ведущего по ходу игры, и отметки нового с отметками
    // прежнего несравнимы — часы у них свои. Забыть всё нельзя: показывать
    // машину до первого снимка нового ведущего было бы нечем.
    Timeline<Mark> line;

    for (int number = 0; number < 5; ++number) {
        line.accept(Mark{.sentAt = static_cast<oxymp::shared::Timestamp>(1000 + number * 50),
                         .tag = number});
    }

    line.keepNewest();

    CHECK(line.size() == 1);
    CHECK(line.newest().tag == 4);
}

TEST_CASE("a restarted timeline holds what it was given", "[interpolation]") {
    // Объявленное сервером состояние машины снимком не считается: часы его — не
    // часы ведущего. Первый настоящий снимок заводит ленту заново.
    Timeline<Mark> line;
    line.accept(Mark{.sentAt = 1000, .tag = 0});
    line.accept(Mark{.sentAt = 1050, .tag = 1});

    line.restart(Mark{.sentAt = 7, .tag = 42});

    CHECK(line.size() == 1);
    CHECK(line.newest().tag == 42);
}

TEST_CASE("catching up does not depend on the frame rate", "[interpolation]") {
    // Главное свойство и причина, по которой расчёт вообще появился. Раньше за
    // кадр съедалась постоянная доля расхождения, и при шестидесяти кадрах оно
    // закрывалось вдвое быстрее, чем при тридцати: одна и та же машина у двух
    // зрителей ехала по-разному, и виновата в этом была не сеть.
    constexpr float kRate = 26.0F;

    // Что осталось от расхождения за секунду при тридцати кадрах и при
    // шестидесяти. Ответ обязан быть одним и тем же.
    float slow = 1.0F;
    for (int frame = 0; frame < 30; ++frame) {
        slow *= 1.0F - catchUp(kRate, 1.0F / 30.0F);
    }

    float fast = 1.0F;
    for (int frame = 0; frame < 60; ++frame) {
        fast *= 1.0F - catchUp(kRate, 1.0F / 60.0F);
    }

    CHECK(slow == Approx(fast).margin(1e-6F));
}

TEST_CASE("catching up stays within its bounds", "[interpolation]") {
    // Времени не прошло — закрывать нечего. Кадр нулевой длины случается на
    // самом первом, когда сравнивать не с чем.
    CHECK(catchUp(26.0F, 0.0F) == 0.0F);

    // И назад время не идёт: счётчик игры переполняется, и разница уходит в
    // минус. Отрицательная доля тянула бы тело прочь от снимка.
    CHECK(catchUp(26.0F, -1.0F) == 0.0F);

    // Долгий кадр закрывает расхождение почти целиком, но не больше него:
    // доля свыше единицы означала бы промах мимо цели с перелётом.
    CHECK(catchUp(26.0F, 10.0F) <= 1.0F);
    CHECK(catchUp(26.0F, 10.0F) == Approx(1.0F));
}

TEST_CASE("a new sender starts at the cautious delay", "[interpolation]") {
    // О новом отправителе не известно ничего, и отставание берётся с запасом:
    // взятое впритык, оно обернулось бы рывками до первой же оценки.
    const DelayEstimator fresh;

    CHECK(fresh.delay() == kStartDelay);
}

TEST_CASE("a steady line is shown closer to now", "[interpolation]") {
    // Ради чего оценка и заведена. Раньше отставание было одним и тем же
    // всегда: сто миллисекунд и на игре по соседней комнате, и на игре через
    // полмира. Первому эти сто миллисекунд — чистый убыток.
    DelayEstimator steady;

    // Сеть, доставляющая ровно: промежуток прихода в точности повторяет
    // промежуток отправки.
    for (int snapshot = 0; snapshot < 200; ++snapshot) {
        steady.notice(std::chrono::milliseconds{50}, std::chrono::milliseconds{50});
    }

    CHECK(steady.delay() < kStartDelay);
    CHECK(steady.delay() >= kMinDelay);
}

TEST_CASE("a jittery line is shown further behind", "[interpolation]") {
    // И обратное: сеть, то придерживающая пакеты, то отпускающая их пачкой,
    // просит запаса тем больше, чем сильнее её мотает. Без этого запаса чужие
    // на ней дёргаются.
    DelayEstimator steady;
    DelayEstimator jittery;

    for (int snapshot = 0; snapshot < 200; ++snapshot) {
        steady.notice(std::chrono::milliseconds{50}, std::chrono::milliseconds{50});

        // Пакеты приходят то вдвое раньше, то вдвое позже срока.
        jittery.notice(std::chrono::milliseconds{50},
                       std::chrono::milliseconds{snapshot % 2 == 0 ? 10 : 90});
    }

    CHECK(jittery.delay() > steady.delay());
}

TEST_CASE("the delay rises quickly and falls slowly", "[interpolation]") {
    // Растёт быстро: не хватило запаса — это рывок, и медлить с ним нельзя.
    // Спадает медленно, и это важнее: уменьшение отставания — прыжок вперёд по
    // времени, чужие разом оказываются там, где они будут, а не там, где были.
    // Сделанный разом, он выглядит рывком.
    DelayEstimator estimator;

    for (int snapshot = 0; snapshot < 100; ++snapshot) {
        estimator.notice(std::chrono::milliseconds{50}, std::chrono::milliseconds{50});
    }

    const auto calm = estimator.delay();

    // Сеть испортилась: десяти снимков хватает, чтобы отставание выросло.
    for (int snapshot = 0; snapshot < 10; ++snapshot) {
        estimator.notice(std::chrono::milliseconds{50},
                         std::chrono::milliseconds{snapshot % 2 == 0 ? 0 : 200});
    }

    const auto shaken = estimator.delay();
    CHECK(shaken > calm);

    // Сеть выправилась — а отставание за те же десять снимков почти не
    // изменилось: спад растянут на секунды.
    for (int snapshot = 0; snapshot < 10; ++snapshot) {
        estimator.notice(std::chrono::milliseconds{50}, std::chrono::milliseconds{50});
    }

    CHECK(estimator.delay() > calm);
}

TEST_CASE("the delay stays within its bounds", "[interpolation]") {
    // Ни идеальная сеть не сводит запас к нулю, ни безнадёжная не уводит его в
    // полсекунды: и то и другое хуже середины.
    DelayEstimator perfect;
    DelayEstimator hopeless;

    for (int snapshot = 0; snapshot < 500; ++snapshot) {
        perfect.notice(std::chrono::milliseconds{5}, std::chrono::milliseconds{5});
        hopeless.notice(std::chrono::milliseconds{50},
                        std::chrono::milliseconds{snapshot % 2 == 0 ? 0 : 2000});
    }

    CHECK(perfect.delay() >= kMinDelay);
    CHECK(hopeless.delay() <= kMaxDelay);
}

TEST_CASE("a skipped snapshot is not a slower sender", "[interpolation]") {
    // Отправитель шлёт снимки не всегда: стоящий на месте человек не шлёт
    // ничего нового, и следующий его снимок приходит через четверть секунды
    // вместо тридцатой доли. Это «сказать было нечего», а не «частота упала в
    // восемь раз».
    //
    // Без защиты одиночный пропуск уводил бы отставание к потолку с одного
    // снимка, а спадало бы оно оттуда десяток секунд — то есть чужой игрок
    // после каждой остановки полминуты показывался бы вдвое позади правды.
    DelayEstimator estimator;

    for (int snapshot = 0; snapshot < 200; ++snapshot) {
        estimator.notice(std::chrono::milliseconds{33}, std::chrono::milliseconds{33});
    }

    const auto steady = estimator.delay();

    // Человек постоял четверть секунды и снова пошёл. Дрожания при этом нет:
    // снимок пришёл ровно тогда, когда его отправили.
    estimator.notice(std::chrono::milliseconds{250}, std::chrono::milliseconds{250});

    CHECK(estimator.delay() < steady + std::chrono::milliseconds{10});
}

TEST_CASE("a sender that really slowed down is followed", "[interpolation]") {
    // Обратная сторона той же защиты: сеть, на которой снимки и правда пошли
    // втрое реже, обязана получить втрое больший запас. Иначе защита от
    // пропусков превратилась бы в слепоту к настоящей беде.
    DelayEstimator estimator;

    for (int snapshot = 0; snapshot < 200; ++snapshot) {
        estimator.notice(std::chrono::milliseconds{33}, std::chrono::milliseconds{33});
    }

    const auto quick = estimator.delay();

    for (int snapshot = 0; snapshot < 200; ++snapshot) {
        estimator.notice(std::chrono::milliseconds{100}, std::chrono::milliseconds{100});
    }

    CHECK(estimator.delay() > quick);
    CHECK(estimator.delay() >= std::chrono::milliseconds{100});
}

TEST_CASE("a forgotten estimate starts over", "[interpolation]") {
    // Машина меняет ведущего по ходу игры, а с ним меняется и сеть, по которой
    // приходят её снимки. Накопленное о прежнем к новому не относится ничем.
    DelayEstimator estimator;

    for (int snapshot = 0; snapshot < 200; ++snapshot) {
        estimator.notice(std::chrono::milliseconds{50}, std::chrono::milliseconds{50});
    }

    CHECK(estimator.delay() != kStartDelay);

    estimator.forget();

    CHECK(estimator.delay() == kStartDelay);
}

TEST_CASE("a repeated snapshot does not move the estimate", "[interpolation]") {
    // Промежутка нет: снимок пришёл дважды или обогнал предыдущий. Поделив на
    // такой промежуток, мы получили бы отставание любой величины.
    DelayEstimator estimator;

    estimator.notice(std::chrono::milliseconds{0}, std::chrono::milliseconds{50});
    estimator.notice(std::chrono::milliseconds{50}, std::chrono::milliseconds{0});

    CHECK(estimator.delay() == kStartDelay);
}

TEST_CASE("the curve keeps both of its ends", "[interpolation]") {
    // Главное свойство: кривая обязана проходить ровно через оба снимка. Не
    // проходящая — это уже не сглаживание, а смещение, и чужой игрок стоял бы
    // рядом с тем местом, где он есть на самом деле.
    const oxymp::shared::Vec3 from{10.0F, 20.0F, 30.0F};
    const oxymp::shared::Vec3 to{12.0F, 24.0F, 30.0F};
    const oxymp::shared::Vec3 fromVelocity{40.0F, 0.0F, 0.0F};
    const oxymp::shared::Vec3 toVelocity{0.0F, 80.0F, 0.0F};

    const auto start = curve(from, fromVelocity, to, toVelocity, 0.05F, 0.0F);
    const auto finish = curve(from, fromVelocity, to, toVelocity, 0.05F, 1.0F);

    CHECK(start.x == Approx(from.x));
    CHECK(start.y == Approx(from.y));
    CHECK(finish.x == Approx(to.x));
    CHECK(finish.y == Approx(to.y));
}

TEST_CASE("the curve leaves along the velocity it was given", "[interpolation]") {
    // Второе главное свойство и вся причина, по которой кривая появилась. На
    // стыке двух отрезков скорость обязана совпасть с обеих сторон: прямая
    // ломалась на каждом снимке, и этот излом видно как рывок на каждом из них.
    //
    // Проверяется численно: за малую долю пути тело обязано уйти туда, куда
    // указывает скорость, и настолько, насколько она велика.
    const oxymp::shared::Vec3 from{0.0F, 0.0F, 0.0F};
    const oxymp::shared::Vec3 to{1.0F, 1.0F, 0.0F};
    const oxymp::shared::Vec3 fromVelocity{20.0F, 0.0F, 0.0F};

    constexpr float kSpan = 0.05F;
    constexpr float kStep = 0.001F;

    const auto stepped = curve(from, fromVelocity, to, oxymp::shared::Vec3{}, kSpan, kStep);

    // Путь за kStep доли отрезка — это скорость, умноженная на то время, что
    // эта доля занимает.
    CHECK(stepped.x == Approx(fromVelocity.x * kSpan * kStep).epsilon(0.01F));
    CHECK(stepped.y == Approx(0.0F).margin(1e-3F));
}

TEST_CASE("the curve does not cut the corner", "[interpolation]") {
    // Ради чего всё и затевалось: тело, входящее в поворот, идёт по дуге, а
    // прямая между снимками режет её насквозь. На машине это метр с лишним
    // среза за каждый снимок.
    const oxymp::shared::Vec3 from{0.0F, 0.0F, 0.0F};
    const oxymp::shared::Vec3 to{1.0F, 1.0F, 0.0F};

    const auto curved = curve(from, oxymp::shared::Vec3{10.0F, 0.0F, 0.0F}, to,
                              oxymp::shared::Vec3{0.0F, 10.0F, 0.0F}, 0.1F, 0.5F);
    const auto straight = mix(from, to, 0.5F);

    // Кривая держится снаружи хорды — со стороны, куда тело двигалось сначала.
    CHECK(curved.x > straight.x);
    CHECK(curved.y < straight.y);
}

TEST_CASE("the curve refuses a velocity that fights the segment", "[interpolation]") {
    // Скорость приходит из игры и отрезку не подчиняется: между снимками игрок
    // мог упереться в стену или получить перенос от сервера. Ничем не
    // ограниченная касательная в таком случае выбрасывает тело далеко в сторону
    // и возвращает обратно — заметнее любого рывка, который кривая убирает.
    const oxymp::shared::Vec3 from{0.0F, 0.0F, 0.0F};
    const oxymp::shared::Vec3 to{1.0F, 0.0F, 0.0F};

    const auto curved = curve(from, oxymp::shared::Vec3{0.0F, 0.0F, 1000.0F}, to,
                              oxymp::shared::Vec3{}, 0.05F, 0.5F);

    // Отрезок длиной в метр, и дальше метра в сторону от него кривая не уходит.
    CHECK(std::abs(curved.z) < 1.0F);
}

TEST_CASE("a segment of no length leaves the body where it stands", "[interpolation]") {
    // Самый частый случай в сессии: человек стоит на месте и шлёт одно и то же
    // положение, а скорость у него от нуля отличается — игра отдаёт остаточную.
    // Кривая по такой скорости уводила бы стоящего в сторону и возвращала
    // двадцать раз в секунду.
    const oxymp::shared::Vec3 standing{5.0F, 5.0F, 5.0F};

    const auto curved = curve(standing, oxymp::shared::Vec3{3.0F, 3.0F, 0.0F}, standing,
                              oxymp::shared::Vec3{3.0F, 3.0F, 0.0F}, 0.05F, 0.5F);

    CHECK(curved.x == Approx(standing.x));
    CHECK(curved.y == Approx(standing.y));
    CHECK(curved.z == Approx(standing.z));
}

TEST_CASE("curved angles keep their ends and the short way", "[interpolation]") {
    // Та же кривая для поворота машины. Через ноль она обязана идти короткой
    // дугой — иначе машина в развороте прокручивалась бы в обратную сторону на
    // целый оборот.
    const oxymp::shared::Vec3 from{0.0F, 0.0F, 350.0F};
    const oxymp::shared::Vec3 to{0.0F, 0.0F, 10.0F};

    const auto start = curveAngles(from, oxymp::shared::Vec3{}, to, oxymp::shared::Vec3{}, 0.05F,
                                   0.0F);
    const auto middle = curveAngles(from, oxymp::shared::Vec3{}, to, oxymp::shared::Vec3{}, 0.05F,
                                    0.5F);

    CHECK(start.z == Approx(-10.0F)); // те же 350 градусов
    CHECK(middle.z == Approx(0.0F));
}

TEST_CASE("curved angles take the angular velocity in radians", "[interpolation]") {
    // Та же ловушка, что и у advanceAngles: игра отдаёт поворот в градусах, а
    // угловую скорость — в радианах в секунду. Сложи их без пересчёта — и
    // машина уйдёт по кривой в пятьдесят семь раз круче, чем на самом деле.
    //
    // Проверяется тем же приёмом, что и кривая положения: за малую долю пути
    // угол обязан уйти ровно на столько, на сколько за это время поворачивает
    // угловая скорость.
    constexpr float kSpan = 0.1F;
    constexpr float kStep = 0.001F;
    constexpr float kRate = 1.0F; // радиан в секунду
    constexpr float kDegreesPerRadian = 57.2957F;

    const auto stepped =
        curveAngles(oxymp::shared::Vec3{}, oxymp::shared::Vec3{0.0F, 0.0F, kRate},
                    oxymp::shared::Vec3{0.0F, 0.0F, 30.0F}, oxymp::shared::Vec3{}, kSpan, kStep);

    CHECK(stepped.z == Approx(kRate * kDegreesPerRadian * kSpan * kStep).epsilon(0.05F));
}

TEST_CASE("curved angles lead when the turn starts faster than it ends", "[interpolation]") {
    // Ради чего кривая для углов и заведена: машина в заносе разворачивается за
    // десятые доли секунды, и прямая между двумя снимками поворота проходит
    // совсем не там, где машина была. Разворот, начатый быстро и законченный
    // медленно, обязан к середине отрезка уйти дальше половины.
    const auto curved =
        curveAngles(oxymp::shared::Vec3{}, oxymp::shared::Vec3{0.0F, 0.0F, 6.0F},
                    oxymp::shared::Vec3{0.0F, 0.0F, 30.0F}, oxymp::shared::Vec3{}, 0.1F, 0.5F);

    CHECK(curved.z > mixAngle(0.0F, 30.0F, 0.5F));
}

TEST_CASE("the seam is what the new segment left behind", "[interpolation]") {
    // Пока свежих снимков нет, движение достраивается вперёд по скорости;
    // пришедший снимок начинает новый отрезок с той точки, где тело было на
    // самом деле, — то есть позади достроенного. Разница и есть шов.
    const oxymp::shared::Vec3 shown{10.0F, 0.0F, 0.0F};
    const oxymp::shared::Vec3 fresh{9.0F, 0.0F, 0.0F};

    const auto gap = seamBetween(shown, fresh);

    CHECK(gap.x == Approx(1.0F));
    CHECK(gap.y == Approx(0.0F));
}

TEST_CASE("a jump is not a seam", "[interpolation]") {
    // Сервер переставил игрока, тот сел в машину или связь пропадала так долго,
    // что достраивать было нечего. Затянутый плавно, такой разрыв выглядит как
    // человек, едущий по земле стоя.
    const auto gap = seamBetween(oxymp::shared::Vec3{500.0F, 0.0F, 0.0F}, oxymp::shared::Vec3{});

    CHECK(gap.x == 0.0F);
    CHECK(gap.y == 0.0F);
    CHECK(gap.z == 0.0F);
}

TEST_CASE("the seam is whole at the moment it appears", "[interpolation]") {
    // В само мгновение смены отрезка показываемое положение обязано остаться
    // тем же самым — в этом весь смысл шва. Съешь мы хоть часть его сразу, и
    // рывок вернулся бы, только меньше.
    const oxymp::shared::Vec3 fresh{9.0F, 0.0F, 0.0F};
    const auto gap = seamBetween(oxymp::shared::Vec3{10.0F, 0.0F, 0.0F}, fresh);

    const auto shown = healed(fresh, gap, 0.0F);

    CHECK(shown.x == Approx(10.0F));
}

TEST_CASE("the seam fades and does not come back", "[interpolation]") {
    // Четверть секунды — и от шва остаётся десятая доля. Дольше значит возить
    // чужого позади правды дольше, чем он того стоит.
    const oxymp::shared::Vec3 fresh{};
    const oxymp::shared::Vec3 gap{1.0F, 0.0F, 0.0F};

    const auto quarter = healed(fresh, gap, 0.25F);
    const auto second = healed(fresh, gap, 1.0F);

    CHECK(quarter.x < 0.1F);
    CHECK(quarter.x > 0.0F);
    CHECK(second.x < quarter.x);
}

TEST_CASE("a frame of no length leaves the seam alone", "[interpolation]") {
    // Кадр нулевой длины случается на первом же; отрицательный — на
    // переполнении счётчика игры. Ни тот, ни другой не повод усилить шов.
    const oxymp::shared::Vec3 gap{1.0F, 2.0F, 3.0F};

    const auto still = healed(oxymp::shared::Vec3{}, gap, 0.0F);
    const auto backwards = healed(oxymp::shared::Vec3{}, gap, -1.0F);

    CHECK(still.x == Approx(backwards.x));
    CHECK(still.y == Approx(backwards.y));
    CHECK(still.z == Approx(backwards.z));
}

TEST_CASE("angles turn the short way around", "[interpolation]") {
    // Главное свойство расчёта углов, и то самое, из-за отсутствия которого
    // чужие игроки разворачивались не в ту сторону: с 350 градусов на 10 ближе
    // идти через ноль, а не через всю окружность обратно.
    CHECK(mixAngle(350.0F, 10.0F, 0.5F) == Approx(0.0F));
    CHECK(mixAngle(10.0F, 350.0F, 0.5F) == Approx(0.0F));

    // И то же в обычном случае, без перехода через ноль.
    CHECK(mixAngle(0.0F, 90.0F, 0.5F) == Approx(45.0F));
}

TEST_CASE("angle blending keeps its ends", "[interpolation]") {
    // Доля ноль — начало, доля единица — конец. Иначе поворот приходил бы не
    // туда, куда смотрит хозяин, а рядом.
    CHECK(mixAngle(30.0F, 200.0F, 0.0F) == Approx(30.0F));
    CHECK(mixAngle(30.0F, 200.0F, 1.0F) == Approx(-160.0F)); // те же 200 градусов

    // Угол приводится к промежутку от минус половины оборота до половины: игра
    // принимает и такой, а сравнивать их между собой так проще.
    CHECK(mixAngle(180.0F, 180.0F, 0.5F) == Approx(-180.0F));
}

TEST_CASE("angular velocity comes in radians", "[interpolation]") {
    // Поворот игра отдаёт в градусах, а угловую скорость — в радианах в
    // секунду. Сложи их без пересчёта — и машина за секунду довернётся на
    // пятьдесят семь градусов вместо одного.
    const auto turned = advanceAngles(oxymp::shared::Vec3{0.0F, 0.0F, 0.0F},
                                      oxymp::shared::Vec3{0.0F, 0.0F, 1.0F}, 1.0F);

    CHECK(turned.z == Approx(57.2957F).margin(0.01F));
}

TEST_CASE("positions move along their velocity", "[interpolation]") {
    const auto moved = advance(oxymp::shared::Vec3{10.0F, 0.0F, 5.0F},
                               oxymp::shared::Vec3{2.0F, -4.0F, 0.0F}, 0.5F);

    CHECK(moved.x == Approx(11.0F));
    CHECK(moved.y == Approx(-2.0F));
    CHECK(moved.z == Approx(5.0F));
}
