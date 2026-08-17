#include <oxymp/shared/script/mvalue.hpp>

#include <algorithm>
#include <type_traits>

namespace oxymp::shared {

MValue MValue::nil() {
    return MValue{Body{NilTag{}}};
}

MValue MValue::boolean(bool value) {
    return MValue{Body{value}};
}

MValue MValue::integer(std::int64_t value) {
    return MValue{Body{value}};
}

MValue MValue::unsignedInteger(std::uint64_t value) {
    return MValue{Body{value}};
}

MValue MValue::number(double value) {
    return MValue{Body{value}};
}

MValue MValue::string(std::string value) {
    return MValue{Body{std::move(value)}};
}

MValue MValue::list(List_ value) {
    return MValue{Body{std::move(value)}};
}

MValue MValue::dict(Dict_ value) {
    return MValue{Body{std::move(value)}};
}

MValue MValue::entity(EntityRef value) {
    return MValue{Body{value}};
}

MValue MValue::vector3(const Vec3& value) {
    return MValue{Body{value}};
}

MValue MValue::vector2(const Vec2& value) {
    return MValue{Body{value}};
}

MValue MValue::rgba(const Rgba& value) {
    return MValue{Body{value}};
}

MValue MValue::byteArray(Bytes value) {
    return MValue{Body{std::move(value)}};
}

MValue::Type MValue::type() const noexcept {
    // Порядок в Body совпадает с числами Type, и это не совпадение, а условие.
    //
    // Совпадение позволяет обойтись одной строкой вместо переключателя на
    // четырнадцать ветвей — того самого, который правят при каждом новом типе и
    // однажды забывают поправить. Взамен порядок в Body становится частью
    // протокола: переставив в нём два типа, вы поменяете их номера в сети.
    // Проверки ниже стоят затем, чтобы такая правка не собралась.
    static_assert(std::variant_size_v<Body> == 14,
                  "у Body появился тип — добавьте его и в Type, и в разбор значения");

    static_assert(std::is_same_v<std::variant_alternative_t<0, Body>, std::monostate>);
    static_assert(std::is_same_v<std::variant_alternative_t<1, Body>, NilTag>);
    static_assert(std::is_same_v<std::variant_alternative_t<2, Body>, bool>);
    static_assert(std::is_same_v<std::variant_alternative_t<3, Body>, std::int64_t>);
    static_assert(std::is_same_v<std::variant_alternative_t<4, Body>, std::uint64_t>);
    static_assert(std::is_same_v<std::variant_alternative_t<5, Body>, double>);
    static_assert(std::is_same_v<std::variant_alternative_t<6, Body>, std::string>);
    static_assert(std::is_same_v<std::variant_alternative_t<7, Body>, List_>);
    static_assert(std::is_same_v<std::variant_alternative_t<8, Body>, Dict_>);
    static_assert(std::is_same_v<std::variant_alternative_t<9, Body>, EntityRef>);
    static_assert(std::is_same_v<std::variant_alternative_t<10, Body>, Vec3>);
    static_assert(std::is_same_v<std::variant_alternative_t<11, Body>, Vec2>);
    static_assert(std::is_same_v<std::variant_alternative_t<12, Body>, Rgba>);
    static_assert(std::is_same_v<std::variant_alternative_t<13, Body>, Bytes>);

    return static_cast<Type>(body_.index());
}

const bool* MValue::asBool() const noexcept {
    return std::get_if<bool>(&body_);
}

const std::int64_t* MValue::asInt() const noexcept {
    return std::get_if<std::int64_t>(&body_);
}

const std::uint64_t* MValue::asUInt() const noexcept {
    return std::get_if<std::uint64_t>(&body_);
}

const double* MValue::asDouble() const noexcept {
    return std::get_if<double>(&body_);
}

const std::string* MValue::asString() const noexcept {
    return std::get_if<std::string>(&body_);
}

const MValue::List_* MValue::asList() const noexcept {
    return std::get_if<List_>(&body_);
}

const MValue::Dict_* MValue::asDict() const noexcept {
    return std::get_if<Dict_>(&body_);
}

const EntityRef* MValue::asEntity() const noexcept {
    return std::get_if<EntityRef>(&body_);
}

const Vec3* MValue::asVector3() const noexcept {
    return std::get_if<Vec3>(&body_);
}

const Vec2* MValue::asVector2() const noexcept {
    return std::get_if<Vec2>(&body_);
}

const Rgba* MValue::asRgba() const noexcept {
    return std::get_if<Rgba>(&body_);
}

const MValue::Bytes* MValue::asByteArray() const noexcept {
    return std::get_if<Bytes>(&body_);
}

const MValue* MValue::find(std::string_view key) const noexcept {
    const Dict_* const dict = asDict();
    if (dict == nullptr) {
        return nullptr;
    }

    // Поиск перебором, а не по дереву, и это не небрежность. Словари здесь —
    // это объекты, пришедшие из скрипта: в них единицы ключей, и обход десятка
    // строк дешевле обхода дерева, не говоря уже о его построении.
    const auto found = std::ranges::find(*dict, key, &Dict_::value_type::first);

    return found == dict->end() ? nullptr : &found->second;
}

} // namespace oxymp::shared
