#include "pointer_runtime.h"
#include <shobjidl.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetCurrentProcessExplicitAppUserModelID(L"QInEX.Pointer");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!qpointer::desktop::Initialize(instance, nullptr, show)) return 1;
    MSG message{};
    int result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!qpointer::desktop::ProcessMessage(message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    qpointer::desktop::Shutdown();
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}
