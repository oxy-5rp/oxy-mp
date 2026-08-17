#include "ped_appearance.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <array>

namespace oxymp::client::game {
namespace {

/// Какими числами игра называет слоты аксессуаров.
///
/// Их у неё восемь, а заняты пять, и номера идут с пропуском: после серёг сразу
/// часы. Пропуски передавать незачем — игра и так знает, что в них ничего нет, —
/// поэтому в сообщении они лежат подряд, а здесь переводятся обратно.
constexpr std::array<int, shared::kPedPropCount> kPropSlots{0, 1, 2, 6, 7};

/// Слой лица, которого нет.
constexpr std::uint8_t kNoOverlay = 255;

/// Цвета у слоя нет вовсе.
constexpr std::uint8_t kNoOverlayColour = 2;

} // namespace

PedAppearance::PedAppearance(const NativeTable& table) noexcept
    : getDrawable_(table.handlerFor(natives::kGetPedDrawableVariation)),
      getTexture_(table.handlerFor(natives::kGetPedTextureVariation)),
      getPalette_(table.handlerFor(natives::kGetPedPaletteVariation)),
      setComponent_(table.handlerFor(natives::kSetPedComponentVariation)),
      getProp_(table.handlerFor(natives::kGetPedPropIndex)),
      getPropTexture_(table.handlerFor(natives::kGetPedPropTextureIndex)),
      setProp_(table.handlerFor(natives::kSetPedPropIndex)),
      clearProp_(table.handlerFor(natives::kClearPedProp)),
      setHeadBlend_(table.handlerFor(natives::kSetPedHeadBlendData)),
      setOverlay_(table.handlerFor(natives::kSetPedHeadOverlay)),
      setOverlayColour_(table.handlerFor(natives::kSetPedHeadOverlayColor)),
      setHairColour_(table.handlerFor(natives::kSetPedHairColor)),
      setEyeColour_(table.handlerFor(natives::kSetPedEyeColor)),
      entityModel_(table.handlerFor(natives::kGetEntityModel)) {}

bool PedAppearance::ready() const noexcept {
    return getDrawable_ != nullptr && getTexture_ != nullptr && getPalette_ != nullptr &&
           setComponent_ != nullptr && getProp_ != nullptr && getPropTexture_ != nullptr &&
           setProp_ != nullptr && clearProp_ != nullptr && entityModel_ != nullptr;
}

shared::PlayerAppearance PedAppearance::read(int ped) const {
    shared::PlayerAppearance appearance;

    if (!ready() || ped == 0) {
        return appearance;
    }

    appearance.model = invokeNative<std::uint32_t>(entityModel_, ped);

    for (std::size_t slot = 0; slot < shared::kPedComponentCount; ++slot) {
        const auto component = static_cast<int>(slot);

        appearance.components[slot] = shared::PedComponent{
            .drawable = static_cast<std::uint8_t>(invokeNative<int>(getDrawable_, ped, component)),
            .texture = static_cast<std::uint8_t>(invokeNative<int>(getTexture_, ped, component)),
            .palette = static_cast<std::uint8_t>(invokeNative<int>(getPalette_, ped, component)),
        };
    }

    for (std::size_t slot = 0; slot < shared::kPedPropCount; ++slot) {
        const int prop = kPropSlots[slot];

        // Игра отвечает минус единицей, когда в слоте пусто, и это же значение
        // мы передаём дальше: оно означает «снять», а не «надеть нулевую вещь».
        appearance.props[slot] = shared::PedProp{
            .drawable = static_cast<std::int8_t>(invokeNative<int>(getProp_, ped, prop)),
            .texture = static_cast<std::int8_t>(invokeNative<int>(getPropTexture_, ped, prop)),
        };
    }

    // Лицо и цвета не читаются: у игры на них только запись. Остаются
    // умолчаниями — см. заголовок.
    return appearance;
}

void PedAppearance::apply(int ped, const shared::PlayerAppearance& appearance) const {
    if (!ready() || ped == 0) {
        return;
    }

    for (std::size_t slot = 0; slot < shared::kPedComponentCount; ++slot) {
        const shared::PedComponent& component = appearance.components[slot];

        invokeNative<void>(setComponent_, ped, static_cast<int>(slot),
                           static_cast<int>(component.drawable),
                           static_cast<int>(component.texture),
                           static_cast<int>(component.palette));
    }

    for (std::size_t slot = 0; slot < shared::kPedPropCount; ++slot) {
        const shared::PedProp& prop = appearance.props[slot];
        const int game = kPropSlots[slot];

        if (prop.drawable < 0) {
            invokeNative<void>(clearProp_, ped, game);
            continue;
        }

        // Последний довод — «прикрепить сразу»: без него вещь появится только
        // после следующей перезагрузки персонажа.
        invokeNative<void>(setProp_, ped, game, static_cast<int>(prop.drawable),
                           static_cast<int>(prop.texture), true);
    }

    // Лицо. Ставится целиком одним вызовом: доли смешения и родители — одно
    // описание, и половина его смысла не имеет.
    if (setHeadBlend_ != nullptr) {
        invokeNative<void>(setHeadBlend_, ped, static_cast<int>(appearance.shapeFirst),
                           static_cast<int>(appearance.shapeSecond),
                           static_cast<int>(appearance.shapeThird),
                           static_cast<int>(appearance.skinFirst),
                           static_cast<int>(appearance.skinSecond),
                           static_cast<int>(appearance.skinThird), appearance.shapeMix,
                           appearance.skinMix, appearance.thirdMix, false);
    }

    if (setOverlay_ != nullptr) {
        for (std::size_t slot = 0; slot < shared::kPedOverlayCount; ++slot) {
            const shared::PedOverlay& overlay = appearance.overlays[slot];

            invokeNative<void>(setOverlay_, ped, static_cast<int>(slot),
                               static_cast<int>(overlay.index), overlay.opacity);

            // Цвет ставится отдельно и только тем слоям, у которых он бывает:
            // у веснушек и морщин его нет, и попытка покрасить их ничего не
            // сделает, зато собьёт слой с настроенного.
            if (overlay.index != kNoOverlay && overlay.colourType != kNoOverlayColour &&
                setOverlayColour_ != nullptr) {
                invokeNative<void>(setOverlayColour_, ped, static_cast<int>(slot),
                                   static_cast<int>(overlay.colourType),
                                   static_cast<int>(overlay.colour),
                                   static_cast<int>(overlay.secondColour));
            }
        }
    }

    if (setHairColour_ != nullptr) {
        invokeNative<void>(setHairColour_, ped, static_cast<int>(appearance.hairColour),
                           static_cast<int>(appearance.hairHighlight));
    }

    if (setEyeColour_ != nullptr) {
        invokeNative<void>(setEyeColour_, ped, static_cast<int>(appearance.eyeColour));
    }
}

} // namespace oxymp::client::game
