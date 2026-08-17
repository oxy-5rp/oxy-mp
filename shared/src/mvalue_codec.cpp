#include <oxymp/shared/script/mvalue_codec.hpp>

#include <cstring>

namespace oxymp::shared {
namespace {

/// Предел числа доводов у одного события.
///
/// Отдельно от предела длины списка, потому что смысл другой: список — это
/// данные, а доводы — подпись обработчика. Обработчика на сто с лишним доводов
/// не бывает, а вот события, собранного из мусора, — сколько угодно.
constexpr std::size_t kMaxArgs = 128;

/// Двойное число пишется своими байтами, а не строкой.
///
/// Побайтно и от младшего к старшему — тем же порядком, каким протокол пишет всё
/// остальное. Приведением к целому нельзя: дробная часть потерялась бы, а
/// значения вида 0.5 приходят из скриптов постоянно. Записью через строку тоже
/// нельзя — она теряет последние разряды и стоит вдесятеро дороже.
void writeDouble(ByteWriter& out, double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t),
                  "двойное число иной ширины этим способом не пишется");

    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    out.writeU64(bits);
}

[[nodiscard]] double readDouble(ByteReader& in) {
    const std::uint64_t bits = in.readU64();

    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));

    return value;
}

void writeVec2(ByteWriter& out, const Vec2& value) {
    out.writeFloat(value.x);
    out.writeFloat(value.y);
}

[[nodiscard]] Vec2 readVec2(ByteReader& in) {
    // Оси читаются по одной: порядок вычисления доводов не задан, и собранные в
    // одно выражение они разъехались бы местами на другом компиляторе. Так же
    // сделано и в readVelocity.
    const float x = in.readFloat();
    const float y = in.readFloat();

    return Vec2{x, y};
}

void writeRgba(ByteWriter& out, const Rgba& value) {
    out.writeU8(value.r);
    out.writeU8(value.g);
    out.writeU8(value.b);
    out.writeU8(value.a);
}

[[nodiscard]] Rgba readRgba(ByteReader& in) {
    const std::uint8_t r = in.readU8();
    const std::uint8_t g = in.readU8();
    const std::uint8_t b = in.readU8();
    const std::uint8_t a = in.readU8();

    return Rgba{r, g, b, a};
}

/// Разбор с учётом глубины.
///
/// Глубина — довод, а не поле читателя, потому что она про разбираемое дерево, а
/// не про поток байтов: два значения в одном сообщении не складывают свою
/// вложенность.
[[nodiscard]] MValue readAt(ByteReader& in, std::size_t depth) {
    if (depth > MValue::kMaxDepth) {
        in.fail();
        return MValue{};
    }

    const auto kind = static_cast<MValue::Type>(in.readU8());

    // Проверка после чтения номера, но до чтения тела: у испорченного пакета
    // байты могли кончиться уже на самом номере, и читать по нему дальше значило
    // бы разбирать нули как данные.
    if (!in.ok()) {
        return MValue{};
    }

    switch (kind) {
    case MValue::Type::None:
        return MValue{};

    case MValue::Type::Nil:
        return MValue::nil();

    case MValue::Type::Bool:
        return MValue::boolean(in.readU8() != 0);

    case MValue::Type::Int:
        return MValue::integer(static_cast<std::int64_t>(in.readU64()));

    case MValue::Type::UInt:
        return MValue::unsignedInteger(in.readU64());

    case MValue::Type::Double:
        return MValue::number(readDouble(in));

    case MValue::Type::String:
        return MValue::string(in.readText());

    case MValue::Type::List: {
        const std::uint32_t length = in.readU32();

        if (!in.ok() || length > MValue::kMaxListLength) {
            in.fail();
            return MValue{};
        }

        MValue::List_ items;

        // Без резервирования по присланной длине: length проверен по предделу, но
        // предел — восемь тысяч, и выделять их под список, за которым в пакете
        // лежит один элемент, незачем.
        for (std::uint32_t i = 0; i < length; ++i) {
            items.push_back(readAt(in, depth + 1));

            // Выход по первой неудаче: без него испорченная длина заставила бы
            // читать пустоту восемь тысяч раз.
            if (!in.ok()) {
                return MValue{};
            }
        }

        return MValue::list(std::move(items));
    }

    case MValue::Type::Dict: {
        const std::uint32_t length = in.readU32();

        if (!in.ok() || length > MValue::kMaxListLength) {
            in.fail();
            return MValue{};
        }

        MValue::Dict_ pairs;

        for (std::uint32_t i = 0; i < length; ++i) {
            std::string key = in.readString();
            MValue value = readAt(in, depth + 1);

            if (!in.ok()) {
                return MValue{};
            }

            pairs.emplace_back(std::move(key), std::move(value));
        }

        return MValue::dict(std::move(pairs));
    }

    case MValue::Type::Entity: {
        const auto entityKind = static_cast<EntityKind>(in.readU8());
        const std::uint32_t id = in.readU32();

        if (entityKind != EntityKind::Player && entityKind != EntityKind::Vehicle &&
            entityKind != EntityKind::Object) {
            in.fail();
            return MValue{};
        }

        return MValue::entity(EntityRef{entityKind, id});
    }

    case MValue::Type::Vector3:
        return MValue::vector3(in.readVec3());

    case MValue::Type::Vector2:
        return MValue::vector2(readVec2(in));

    case MValue::Type::Rgba:
        return MValue::rgba(readRgba(in));

    case MValue::Type::ByteArray: {
        const std::uint32_t length = in.readU32();

        if (!in.ok() || length > MValue::kMaxByteArrayLength || length > in.remaining()) {
            in.fail();
            return MValue{};
        }

        MValue::Bytes bytes;
        bytes.reserve(length);

        for (std::uint32_t i = 0; i < length; ++i) {
            bytes.push_back(in.readU8());
        }

        return MValue::byteArray(std::move(bytes));
    }
    }

    // Неизвестный номер типа. Он же — единственный способ отличить пакет от
    // новой сборки: она пришлёт тип, которого здесь ещё нет.
    in.fail();
    return MValue{};
}

} // namespace

void writeMValue(ByteWriter& out, const MValue& value) {
    out.writeU8(static_cast<std::uint8_t>(value.type()));

    switch (value.type()) {
    case MValue::Type::None:
    case MValue::Type::Nil:
        return;

    case MValue::Type::Bool:
        out.writeU8(*value.asBool() ? 1U : 0U);
        return;

    case MValue::Type::Int:
        out.writeU64(static_cast<std::uint64_t>(*value.asInt()));
        return;

    case MValue::Type::UInt:
        out.writeU64(*value.asUInt());
        return;

    case MValue::Type::Double:
        writeDouble(out, *value.asDouble());
        return;

    case MValue::Type::String:
        out.writeText(*value.asString());
        return;

    case MValue::Type::List: {
        const MValue::List_& items = *value.asList();

        out.writeU32(static_cast<std::uint32_t>(items.size()));

        for (const MValue& item : items) {
            writeMValue(out, item);
        }
        return;
    }

    case MValue::Type::Dict: {
        const MValue::Dict_& pairs = *value.asDict();

        out.writeU32(static_cast<std::uint32_t>(pairs.size()));

        for (const auto& [key, item] : pairs) {
            out.writeString(key);
            writeMValue(out, item);
        }
        return;
    }

    case MValue::Type::Entity: {
        const EntityRef& reference = *value.asEntity();

        out.writeU8(static_cast<std::uint8_t>(reference.kind));
        out.writeU32(reference.id);
        return;
    }

    case MValue::Type::Vector3:
        out.writeVec3(*value.asVector3());
        return;

    case MValue::Type::Vector2:
        writeVec2(out, *value.asVector2());
        return;

    case MValue::Type::Rgba:
        writeRgba(out, *value.asRgba());
        return;

    case MValue::Type::ByteArray: {
        const MValue::Bytes& bytes = *value.asByteArray();

        out.writeU32(static_cast<std::uint32_t>(bytes.size()));

        for (const std::uint8_t byte : bytes) {
            out.writeU8(byte);
        }
        return;
    }
    }
}

MValue readMValue(ByteReader& in) {
    return readAt(in, 0);
}

void writeMValueArgs(ByteWriter& out, const MValueArgs& args) {
    out.writeU8(static_cast<std::uint8_t>(args.size() > kMaxArgs ? kMaxArgs : args.size()));

    for (std::size_t i = 0; i < args.size() && i < kMaxArgs; ++i) {
        writeMValue(out, args[i]);
    }
}

MValueArgs readMValueArgs(ByteReader& in) {
    const std::uint8_t count = in.readU8();

    if (!in.ok() || count > kMaxArgs) {
        in.fail();
        return {};
    }

    MValueArgs args;

    for (std::uint8_t i = 0; i < count; ++i) {
        args.push_back(readMValue(in));

        if (!in.ok()) {
            return {};
        }
    }

    return args;
}

} // namespace oxymp::shared
