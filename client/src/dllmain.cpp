// Точка входа модуля, внедряемого в процесс игры.
//
// Здесь нет никакой логики намеренно: точка входа выполняется под блокировкой
// загрузчика, где нельзя ни загружать модули, ни ждать потоки, ни делать
// сколько-нибудь заметную работу. Всё, что нужно, — завести рабочий поток
// и уйти.

#include "client_app.hpp"

#include <windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        // Уведомления о потоках нам не нужны, а игра их создаёт во множестве.
        ::DisableThreadLibraryCalls(module);
        oxymp::client::ClientApp::requestStart();
        break;

    case DLL_PROCESS_DETACH:
        oxymp::client::ClientApp::requestStop();
        break;

    default:
        break;
    }

    return TRUE;
}
