#pragma once

namespace oxymp::launcher::text {

/// Слова, которыми лаунчер разговаривает с человеком.
///
/// Взяты у alt:V дословно — из его таблицы переводов, английской её половины, —
/// и имена здесь повторяют тамошние ключи. Это не подражание ради подражания:
/// человек, у которого что-то не работает, ищет строку в поиске и находит ответ,
/// написанный за годы существования alt:V. Своя формулировка того же самого не
/// находит ничего.
///
/// Отсюда и правило для новых строк: **прежде чем придумывать свою, посмотрите,
/// нет ли такой у alt:V**. Ключи его лежат в `altv.exe` одной таблицей JSON, и
/// достать их оттуда — работа на минуту.
///
/// Многоязычия здесь нет и пока не нужно: у alt:V лаунчер говорит на языке из
/// настройки `lang`, у нас он говорит по-английски, как и журнал. Появится
/// надобность — таблица встанет на это же место, а имена констант останутся.

// Ход запуска. Порядок тот же, что у alt:V: проверки, копия, подмена, игра,
// клиент.
constexpr const char* kCheckingPreconditions = "Checking preconditions";
constexpr const char* kValidatingBackup = "Validating backup";
constexpr const char* kInjectingLauncherPatches = "Injecting launcher patches";
constexpr const char* kStartingGtav = "Starting Grand Theft Auto V";
constexpr const char* kLoadingClient = "Loading oxyMP client";
constexpr const char* kStartingTheGame = "Starting the game";

// Отказы. `{}` там, где alt:V пишет `{0}`.
constexpr const char* kErrGameStart = "Game start error";
constexpr const char* kErrGameStartTimeout = "Game start timed out";
constexpr const char* kErrFailedToLaunchPlatform = "Failed to launch {}";
constexpr const char* kErrFailedToPatchLauncher = "Failed to patch launcher";
constexpr const char* kErrFailedToInject = "Failed to inject oxyMP into GTA V";
constexpr const char* kErrFailedToAccessGtavProcess = "Failed to access GTA V process";
constexpr const char* kErrGameCrashed = "GTA V crashed on startup";
constexpr const char* kErrGameOutdated = "Outdated game version";
constexpr const char* kErrInvalidGamePlatform = "Selected invalid game platform";
constexpr const char* kErrNotGtavPath =
    "This doesn't look like a GTA V installation. Make sure you selected the right folder";
constexpr const char* kErrNotPlatformPath =
    "This doesn't look like a valid {} GTA V installation. Make sure you selected the right "
    "folder and have started singleplayer at least once";
constexpr const char* kErrPathDoesNotExist = "Selected path does not exist";
constexpr const char* kGtavAlreadyRunning = "GTA V is already running, do you want to close it?";
constexpr const char* kGtavAlreadyRunningHeader = "Already running";

// Что можно с этим сделать. У alt:V отказ никогда не приходит один: под ним
// стоит список того, что стоит попробовать, и это заметно лучше молчания.
constexpr const char* kPossibleSolutions = "Possible solutions:";
constexpr const char* kSolStartGameOnce = "Start singleplayer at least once";
constexpr const char* kSolRestartPlatform = "Restart the Rockstar Games Launcher and {}";
constexpr const char* kSolRestartPlatformRgl = "Restart the Rockstar Games Launcher";
constexpr const char* kSolSpecifyAnotherGameDir = "Specify another game directory";
constexpr const char* kSolVerifyGameFiles = "Verify integrity of your game files";
constexpr const char* kSolVerifyGtaNotRunning = "Verify that you don't have GTA V running";
constexpr const char* kSolIncreaseGameTimeout =
    "Increase the timeout value. More information can be found on our troubleshooting page";

// Окно выбора каталога игры — `gamepath.rml` у alt:V.
constexpr const char* kSelectGtavLocation = "Select your GTA V location";
constexpr const char* kSelectGtavFolder = "Select GTA V folder";
constexpr const char* kGtavLocation = "GTA V Location";
constexpr const char* kUnableToFindInstallation =
    "We are unable to automatically find your GTA V installation. "
    "Please select it manually from the list.";
constexpr const char* kChooseLocationMyself = "I want to choose it myself";
constexpr const char* kAlternativeDirectory = "Custom directory";
constexpr const char* kConfirm = "Confirm";
constexpr const char* kCancel = "Cancel";
constexpr const char* kLaunchNow = "Launch now";

} // namespace oxymp::launcher::text
