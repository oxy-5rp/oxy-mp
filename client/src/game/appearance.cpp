#include "appearance.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

#include <utility>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Сетевая мужская заготовка. Та же, с которой начинают в RAGE MP.
/// Голос, который выдаётся сетевой заготовке.
///
/// Голос не следует за моделью: заменив персонажа, мы получаем заготовку,
/// говорящую голосом того, кем игрок был раньше. Отсюда крик Франклина при
/// падении с высоты.
constexpr const char* kFreemodeVoice = "A_M_M_BEVHILLS_01_WHITE_FULL_01";

constexpr const char* kFreemodeModel = "mp_m_freemode_01";

/// Сколько кадров ждать загрузки модели, прежде чем сдаться.
///
/// Заметно больше разумного: модель лежит в основных архивах игры и грузится за
/// доли секунды, а этот предел нужен лишь на случай, когда что-то пошло совсем
/// не так, — чтобы клиент не остался на загрузочном экране навсегда.
constexpr unsigned int kLoadFrameLimit = 900;

/// Сколько кадров дать игре на то, чтобы довести нового персонажа до ума.
constexpr unsigned int kSettleFrames = 10;

/// Каноническое значение joaat от имени модели.
///
/// Служит сверкой для GET_HASH_KEY: хеши нативов в этой сборке перетасованы, и
/// разойдись наше представление о том, какой натив мы зовём, с настоящим — мы
/// получили бы не ошибку, а вызов чужой функции с чужими доводами. Совпадение
/// этих двух чисел означает, что зовём мы именно то, что думаем.
constexpr std::uint32_t kFreemodeModelHash = 0x705E61F2;

} // namespace

Appearance::Appearance(const NativeTable& table) noexcept
    : hashKey_(table.handlerFor(natives::kGetHashKey)),
      requestModel_(table.handlerFor(natives::kRequestModel)),
      hasModelLoaded_(table.handlerFor(natives::kHasModelLoaded)),
      setPlayerModel_(table.handlerFor(natives::kSetPlayerModel)),
      defaultVariation_(table.handlerFor(natives::kSetPedDefaultComponentVariation)),
      releaseModel_(table.handlerFor(natives::kSetModelAsNoLongerNeeded)),
      playerPedId_(table.handlerFor(natives::kPlayerPedId)),
      ambientVoice_(table.handlerFor(natives::kSetAmbientVoiceName)) {}

void Appearance::describeHandlers() const {
    // Адреса обработчиков в журнал — не для красоты. Замена персонажа
    // единственная просит игру пересоздать то, чем игрок управляет, и вылет
    // внутри неё виден только как смещение в GTA5.exe. Сопоставить это смещение
    // с нативом можно лишь по такому списку.
    const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));

    const std::pair<const char*, NativeHandler> handlers[] = {
        {"GET_HASH_KEY", hashKey_},
        {"REQUEST_MODEL", requestModel_},
        {"HAS_MODEL_LOADED", hasModelLoaded_},
        {"SET_PLAYER_MODEL", setPlayerModel_},
        {"SET_PED_DEFAULT_COMPONENT_VARIATION", defaultVariation_},
        {"SET_MODEL_AS_NO_LONGER_NEEDED", releaseModel_},
        {"PLAYER_PED_ID", playerPedId_},
    };

    for (const auto& [name, handler] : handlers) {
        spdlog::debug("натив внешности {} → rva {:#x}", name,
                      reinterpret_cast<std::uintptr_t>(handler) - base);
    }
}

bool Appearance::ready() const noexcept {
    return hashKey_ != nullptr && requestModel_ != nullptr && hasModelLoaded_ != nullptr &&
           setPlayerModel_ != nullptr && defaultVariation_ != nullptr && releaseModel_ != nullptr &&
           playerPedId_ != nullptr;
}

std::uint32_t Appearance::modelHash() {
    if (model_ != 0) {
        return model_;
    }

    // Хеш считает сама игра, а не мы: свой расчёт пришлось бы держать в
    // согласии с её алгоритмом, ничего не выигрывая взамен.
    model_ = invokeNative<std::uint32_t>(hashKey_, kFreemodeModel);

    if (model_ != kFreemodeModelHash) {
        spdlog::error("GET_HASH_KEY вернул {:#010x} вместо {:#010x} — хеш натива указывает не туда",
                      model_, kFreemodeModelHash);

        // Дальше идти нельзя: если этот натив не тот, за кого себя выдаёт, то и
        // соседние по списку под подозрением, а вызов не того натива с чужими
        // доводами — это вылет, а не сообщение об ошибке.
        model_ = 0;
        return 0;
    }

    spdlog::debug("хеш модели {}: {:#010x}", kFreemodeModel, model_);
    return model_;
}

Appearance::Progress Appearance::advance(int player) {
    if (!ready()) {
        return Progress::Failed;
    }

    const std::uint32_t model = modelHash();
    if (model == 0) {
        return Progress::Failed;
    }

    if (step_ == Step::LoadModel) {
        // Заказ повторяется каждый кадр намеренно: игра вправе выгрузить то, что
        // у неё никто не держит, и однократная просьба этого не предотвращает.
        invokeNative<void>(requestModel_, model);

        if (!invokeNative<bool>(hasModelLoaded_, model)) {
            if (++waitedFrames_ >= kLoadFrameLimit) {
                spdlog::error("модель {} не загрузилась за {} кадров", kFreemodeModel,
                              waitedFrames_);
                return Progress::Failed;
            }
            return Progress::Loading;
        }

        // Журнал ведётся по шагам намеренно: замена модели — единственное место,
        // где клиент просит игру пересоздать персонажа игрока, и если она на
        // этом падает, последняя строка обязана назвать шаг, а не оставить
        // гадать по адресу вылета.
        spdlog::info("модель загружена за {} кадров, заменяем персонажа", waitedFrames_);

        invokeNative<void>(setPlayerModel_, player, model);
        step_ = Step::DressUp;

        spdlog::info("персонаж заменён, ждём его появления");
        return Progress::Loading;
    }

    // Персонаж не только должен появиться, но и устояться: игра доводит его до
    // рабочего состояния не за один кадр, и обращаться к нему раньше — верный
    // способ получить вылет вместо одетого персонажа.
    if (++settledFrames_ < kSettleFrames) {
        return Progress::Loading;
    }

    // Замена создала нового персонажа, поэтому его номер спрашивается заново:
    // прежний уже недействителен.
    const int ped = invokeNative<int>(playerPedId_);
    if (ped == 0) {
        return Progress::Loading;
    }

    spdlog::info("новый персонаж {}, одеваем", ped);

    // Без этого персонаж стоит голым: заготовка приходит без одежды вовсе.
    invokeNative<void>(defaultVariation_, ped, false);

    silenceStoryVoice(ped);

    invokeNative<void>(releaseModel_, model);

    spdlog::info("модель игрока заменена на {}", kFreemodeModel);
    return Progress::Done;
}

void Appearance::silenceStoryVoice(int ped) const {
    if (ambientVoice_ == nullptr || ped == 0) {
        return;
    }

    // Голос сетевой заготовки, а не сюжетного персонажа. Не подойди он — хуже
    // молчания не будет, а молчание всё равно лучше крика Франклина.
    invokeNative<void>(ambientVoice_, ped, kFreemodeVoice);

    spdlog::info("голос персонажа заменён на {}", kFreemodeVoice);
}

} // namespace oxymp::client::game
