#include <oxymp/client/interpolation.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oxymp::client::interpolation {
namespace {

/// Полный оборот и половина оборота в градусах.
constexpr float kFullTurn = 360.0F;
constexpr float kHalfTurn = 180.0F;

/// Сколько градусов в радиане. Угловая скорость приходит от игры в радианах, а
/// углы поворота — в градусах: игра отдаёт их в разных единицах, и складывать их
/// без пересчёта нельзя.
constexpr float kDegreesPerRadian = 180.0F / std::numbers::pi_v<float>;

/// Какая доля новой величины идёт в оценку промежутка и дрожания.
///
/// Восьмая и шестнадцатая — те же доли, по которым считает задержку TCP и по
/// которым считает дрожание RTP. Дрожание сглаживается вдвое сильнее промежутка
/// нарочно: промежутку положено следовать за отправителем, а дрожанию — за
/// сетью, и вздрогнувшая один раз сеть не повод удлинять отставание всем.
constexpr float kIntervalShare = 1.0F / 8.0F;
constexpr float kJitterShare = 1.0F / 16.0F;

/// Во сколько раз промежуток вправе разом удлиниться в глазах оценки.
///
/// Отправитель шлёт снимки не всегда: стоящий на месте человек не шлёт ничего
/// нового, и следующий его снимок приходит через четверть секунды вместо
/// тридцатой доли. Пропуск — это не «частота упала в восемь раз», а «сказать
/// было нечего», и принимать одно за другое нельзя: оценка ушла бы к своему
/// потолку с одного снимка, а спадала бы оттуда десяток секунд.
///
/// Вдвое за снимок — достаточно, чтобы за несколько снимков дойти до любой
/// настоящей частоты, и мало, чтобы одиночный пропуск сдвинул оценку заметно.
constexpr float kMaxIntervalGrowth = 2.0F;

/// Во сколько промежутков между отправками обходится запас на потерю.
///
/// Не два, как было раньше, а один с небольшим: на потерю пакета запас больше
/// не нужен целиком — потерянный снимок удлиняет сам промежуток, и оценка
/// следует за ним.
constexpr float kIntervalReserve = 1.2F;

/// Во сколько дрожаний обходится запас на дрожание.
///
/// Тройка — то же правило, по которому считает запас RTP: три отклонения
/// покрывают подавляющее большинство разбросов, а покрывать все до последнего
/// значит платить за них всем остальным.
constexpr float kJitterReserve = 3.0F;

/// Надбавка сверх всего посчитанного.
///
/// Оценка описывает сеть, а не нас: снимок, пришедший вовремя, всё ещё ждёт
/// ближайшего кадра игры, а кадры идут неровно. Пятнадцать миллисекунд — кадр
/// при шестидесяти в секунду.
constexpr float kFrameMargin = 0.015F;

/// Как быстро отставание растёт и как медленно спадает — в долях прошедшего
/// времени, а не в долях расхождения.
///
/// Разница здесь та же, что и у catchUp, и она принципиальна. Отставание — это
/// сдвиг между нашим временем и тем мгновением, которое мы показываем. Меняя
/// его, мы меняем ход показываемого времени: выросшее на десятую долю секунды
/// разом отматывает чужих на эту десятую назад, спавшее — бросает вперёд. И то
/// и другое видно ровно как рывок, ради устранения которого всё и затевалось.
///
/// Считая долей прошедшего времени, мы задаём не шаг, а скорость: одна десятая
/// — это «показываемое время идёт на десятую медленнее нашего», две сотых —
/// «на две сотых быстрее». Первое заметно как лёгкое замедление, второе не
/// заметно вовсе, и оба не рвут движение нигде.
///
/// Вверх быстрее, чем вниз, и это не симметрия ради симметрии. Не хватило
/// запаса — начинается достраивание вслепую, и тянуть с этим нельзя. Лишний же
/// запас не стоит ничего, кроме нескольких миллисекунд отставания, и спешить
/// избавляться от него незачем.
constexpr float kRise = 0.10F;
constexpr float kFall = 0.02F;

/// Приводит угол к промежутку от минус половины оборота до половины.
float wrap(float degrees) noexcept {
    return std::fmod(degrees + kHalfTurn + kFullTurn, kFullTurn) - kHalfTurn;
}

/// Кубическая кривая Эрмита в одном измерении.
///
/// Четыре множителя — те самые, что дают кривую, проходящую через оба конца с
/// заданными в них наклонами. Расписаны через степени доли пути, а не через
/// готовые множители: так видно, что кривая именно кубическая, и не приходится
/// держать в голове четыре многочлена.
float hermite(float from, float fromTangent, float to, float toTangent,
              float progress) noexcept {
    const float squared = progress * progress;
    const float cubed = squared * progress;

    const float atFrom = (2.0F * cubed) - (3.0F * squared) + 1.0F;
    const float slopeAtFrom = cubed - (2.0F * squared) + progress;
    const float atTo = (-2.0F * cubed) + (3.0F * squared);
    const float slopeAtTo = cubed - squared;

    return (atFrom * from) + (slopeAtFrom * fromTangent) + (atTo * to) + (slopeAtTo * toTangent);
}

/// Укорачивает наклон, если он расходится с самим отрезком.
///
/// Причина — в kMaxTangent: скорость приходит из игры и отрезку не подчиняется.
/// Отрезок нулевой длины — не оговорка, а самый частый случай: стоящий на месте
/// человек шлёт одно и то же положение, а скорость у него от нуля отличается.
/// Обнулённый наклон оставляет его стоять, а неограниченный увёл бы в сторону и
/// вернул обратно на каждом снимке.
float shorten(float tangent, float chord) noexcept {
    const float limit = kMaxTangent * std::abs(chord);

    return std::clamp(tangent, -limit, limit);
}

/// То же для тройки: длина считается по всей тройке разом, а не по осям.
///
/// По осям было бы неверно: укоротив одну ось из трёх, мы повернули бы
/// направление скорости, и кривая ушла бы не туда, куда двигалось тело.
shared::Vec3 shorten(const shared::Vec3& tangent, const shared::Vec3& chord) noexcept {
    const float lengthSquared =
        (tangent.x * tangent.x) + (tangent.y * tangent.y) + (tangent.z * tangent.z);
    const float limitSquared = kMaxTangent * kMaxTangent *
                               ((chord.x * chord.x) + (chord.y * chord.y) + (chord.z * chord.z));

    if (lengthSquared <= limitSquared) {
        return tangent;
    }

    if (lengthSquared <= 0.0F) {
        return shared::Vec3{};
    }

    const float share = std::sqrt(limitSquared / lengthSquared);

    return shared::Vec3{tangent.x * share, tangent.y * share, tangent.z * share};
}

/// Кривая Эрмита для одного угла, с приведением к кратчайшей дуге.
float curveAngle(float from, float fromRate, float to, float toRate, float span,
                 float progress) noexcept {
    // Считаем не по самим углам, а по разнице от начального: иначе кривая между
    // 350 и 10 градусами прошла бы через всю окружность обратно — та же беда,
    // от которой заведён mixAngle.
    const float delta = wrap(to - from);

    const float fromTangent = shorten(fromRate * kDegreesPerRadian * span, delta);
    const float toTangent = shorten(toRate * kDegreesPerRadian * span, delta);

    return wrap(from + hermite(0.0F, fromTangent, delta, toTangent, progress));
}

} // namespace

float catchUp(float rate, float seconds) noexcept {
    if (rate <= 0.0F || seconds <= 0.0F) {
        // Время не шло — закрывать нечего. Ноль здесь честнее любой доли: кадр
        // нулевой длины случается на первом же кадре, когда сравнивать не с чем.
        return 0.0F;
    }

    return std::clamp(1.0F - std::exp(-rate * seconds), 0.0F, 1.0F);
}

std::chrono::nanoseconds ShowClock::lag() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<float>{lag_});
}

void ShowClock::restart(std::chrono::milliseconds delay) noexcept {
    lag_ = std::chrono::duration<float>{delay}.count();
    started_ = true;
}

void ShowClock::slew(std::chrono::nanoseconds arrivalGap, std::chrono::milliseconds sentGap,
                     std::chrono::milliseconds delay) noexcept {
    const float wanted = std::chrono::duration<float>{delay}.count();

    if (!started_) {
        restart(delay);
        return;
    }

    const float elapsed = std::chrono::duration<float>{arrivalGap}.count();

    if (elapsed <= 0.0F) {
        // Снимок пришёл в то же мгновение, что и прошлый. Двигать часы не на
        // что, а поделив на такой промежуток, мы получили бы любую скорость.
        return;
    }

    // Часы идут вперёд сами: с прошлого прихода прошло elapsed, и на столько же
    // сократилось отставание. А опора сместилась вперёд на sentGap — свежий
    // снимок стал новым началом отсчёта, и отставание от него больше ровно на
    // столько, на сколько его отметка обогнала прежнюю.
    //
    // Разница между этими двумя и есть дрожание. Прежде она выпадала рывком:
    // отставание при всяком приходе ставилось равным оценке, а показываемое
    // мгновение прыгало на эту разницу.
    const float kept = lag_ - elapsed + std::chrono::duration<float>{sentGap}.count();

    const float off = wanted - kept;

    if (std::abs(off) > kMaxDrift) {
        // Это уже не дрожание, а перерыв: подтягивать такое по десятой доле
        // значило бы показывать вчерашний день ещё две секунды.
        lag_ = wanted;
        return;
    }

    // Подтягивание со связанной скоростью: за elapsed часы вправе уйти не
    // больше чем на kMaxSlew этого времени. Отсюда и берётся вся ровность —
    // расхождение растворяется в скорости показа, а не выпадает рывком.
    const float step = kMaxSlew * elapsed;

    lag_ = kept + std::clamp(off, -step, step);
}

void DelayEstimator::notice(std::chrono::milliseconds span,
                            std::chrono::nanoseconds gap) noexcept {
    const float sent = std::chrono::duration<float>{span}.count();
    const float arrived = std::chrono::duration<float>{gap}.count();

    if (sent <= 0.0F || arrived <= 0.0F) {
        // Промежутка нет: снимок первый у этого отправителя, пришёл дважды или
        // обогнал предыдущий. Оценивать по нему нечего, а поделив на него, мы
        // получили бы отставание любой величины.
        return;
    }

    // Расхождение между тем, как часто снимки уходили, и тем, как часто они
    // пришли, — это и есть дрожание сети. Знак не важен: пакет, задержавшийся
    // на сорок миллисекунд, и пакет, догнавший предыдущий, говорят об одном и
    // том же разбросе.
    const float wobble = std::abs(arrived - sent);

    if (!started_) {
        // Первый промежуток берётся целиком. Начни мы с восьмой его доли —
        // оценка подбиралась бы к правде десяток снимков, и всё это время
        // отставание держалось бы наугад.
        interval_ = sent;
        jitter_ = wobble;
        started_ = true;
    } else {
        // Промежуток, разом выросший втрое, — почти всегда пропуск, а не смена
        // частоты: см. kMaxIntervalGrowth.
        const float measured = std::min(sent, interval_ * kMaxIntervalGrowth);

        interval_ += (measured - interval_) * kIntervalShare;
        jitter_ += (wobble - jitter_) * kJitterShare;
    }

    const float wanted = std::clamp((interval_ * kIntervalReserve) + (jitter_ * kJitterReserve) +
                                        kFrameMargin,
                                    std::chrono::duration<float>{kMinDelay}.count(),
                                    std::chrono::duration<float>{kMaxDelay}.count());

    // Шаг ограничен долей того времени, что прошло с прошлого снимка: так
    // отставание меняется со скоростью, а не скачком.
    const float step = wanted - delay_;
    delay_ += std::clamp(step, -kFall * arrived, kRise * arrived);
}

void DelayEstimator::forget() noexcept {
    interval_ = 0.0F;
    jitter_ = 0.0F;
    delay_ = std::chrono::duration<float>{kStartDelay}.count();
    started_ = false;
}

std::chrono::milliseconds DelayEstimator::interval() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<float>{interval_});
}

std::chrono::milliseconds DelayEstimator::jitter() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<float>{jitter_});
}

std::chrono::milliseconds DelayEstimator::delay() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<float>{delay_});
}

shared::Vec3 mix(const shared::Vec3& from, const shared::Vec3& to, float progress) noexcept {
    return shared::Vec3{
        std::lerp(from.x, to.x, progress),
        std::lerp(from.y, to.y, progress),
        std::lerp(from.z, to.z, progress),
    };
}

shared::Vec3 curve(const shared::Vec3& from, const shared::Vec3& fromVelocity,
                   const shared::Vec3& to, const shared::Vec3& toVelocity, float span,
                   float progress) noexcept {
    if (span <= 0.0F) {
        // Отрезка нет — наклонам не на что опереться: касательная измеряется в
        // пути за весь отрезок, а его длина нулевая. Остаётся прямая.
        return mix(from, to, progress);
    }

    const shared::Vec3 chord{to.x - from.x, to.y - from.y, to.z - from.z};

    const shared::Vec3 fromTangent =
        shorten(shared::Vec3{fromVelocity.x * span, fromVelocity.y * span, fromVelocity.z * span},
                chord);
    const shared::Vec3 toTangent =
        shorten(shared::Vec3{toVelocity.x * span, toVelocity.y * span, toVelocity.z * span},
                chord);

    return shared::Vec3{
        hermite(from.x, fromTangent.x, to.x, toTangent.x, progress),
        hermite(from.y, fromTangent.y, to.y, toTangent.y, progress),
        hermite(from.z, fromTangent.z, to.z, toTangent.z, progress),
    };
}

shared::Vec3 curveAngles(const shared::Vec3& from, const shared::Vec3& fromAngularVelocity,
                         const shared::Vec3& to, const shared::Vec3& toAngularVelocity,
                         float span, float progress) noexcept {
    if (span <= 0.0F) {
        return mixAngles(from, to, progress);
    }

    return shared::Vec3{
        curveAngle(from.x, fromAngularVelocity.x, to.x, toAngularVelocity.x, span, progress),
        curveAngle(from.y, fromAngularVelocity.y, to.y, toAngularVelocity.y, span, progress),
        curveAngle(from.z, fromAngularVelocity.z, to.z, toAngularVelocity.z, span, progress),
    };
}

shared::Vec3 advance(const shared::Vec3& position, const shared::Vec3& velocity,
                     float seconds) noexcept {
    return shared::Vec3{
        position.x + velocity.x * seconds,
        position.y + velocity.y * seconds,
        position.z + velocity.z * seconds,
    };
}

shared::Vec3 seamBetween(const shared::Vec3& shown, const shared::Vec3& fresh) noexcept {
    const shared::Vec3 gap{shown.x - fresh.x, shown.y - fresh.y, shown.z - fresh.z};

    if (shared::distanceSquared(shared::Vec3{}, gap) > kMaxSeam * kMaxSeam) {
        // Это уже не шов, а перенос: тянуть его плавно значит везти тело по
        // земле стоя через полгорода.
        return shared::Vec3{};
    }

    return gap;
}

shared::Vec3 healed(const shared::Vec3& position, const shared::Vec3& seam,
                    float seconds) noexcept {
    const float left = std::exp(-kSeamRate * std::max(seconds, 0.0F));

    return shared::Vec3{
        position.x + seam.x * left,
        position.y + seam.y * left,
        position.z + seam.z * left,
    };
}

float mixAngle(float from, float to, float progress) noexcept {
    return wrap(from + wrap(to - from) * progress);
}

shared::Vec3 mixAngles(const shared::Vec3& from, const shared::Vec3& to,
                       float progress) noexcept {
    return shared::Vec3{
        mixAngle(from.x, to.x, progress),
        mixAngle(from.y, to.y, progress),
        mixAngle(from.z, to.z, progress),
    };
}

shared::Vec3 advanceAngles(const shared::Vec3& angles, const shared::Vec3& angularVelocity,
                           float seconds) noexcept {
    return shared::Vec3{
        wrap(angles.x + angularVelocity.x * kDegreesPerRadian * seconds),
        wrap(angles.y + angularVelocity.y * kDegreesPerRadian * seconds),
        wrap(angles.z + angularVelocity.z * kDegreesPerRadian * seconds),
    };
}

} // namespace oxymp::client::interpolation
