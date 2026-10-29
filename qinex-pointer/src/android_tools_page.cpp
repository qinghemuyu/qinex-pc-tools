#include "app.h"
#include "android_cursor_package.h"
#include "embedded_runtime.h"

namespace {
namespace at = qp::android;
struct WirelessDialog {
    Application* application{}; HWND window{}, pairAddress{}, code{}, connectAddress{}; HFONT font{};
};
LRESULT CALLBACK WirelessProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* dialog = reinterpret_cast<WirelessDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { dialog = static_cast<WirelessDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog)); dialog->window = window; }
    if (!dialog) return DefWindowProcW(window, message, wp, lp);
    auto* controller = dialog->application;
    const auto px = [&](int n) { return MulDiv(n, static_cast<int>(GetDpiForWindow(window)), 96); };
    if (message == WM_CREATE) {
        dialog->font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        const auto add = [&](const wchar_t* cls, const wchar_t* label, DWORD style, int id, int x, int y, int w, int h) {
            HWND control = CreateWindowExW(wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls, label, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), controller->instance, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog->font), TRUE); return control;
        };
        add(L"STATIC", W(L"在手机的“开发者选项 → 无线调试”查看地址。配对端口与连接端口不同。", L"在手机无线调试页面查看地址喵。配对和连接端口不同。"), SS_LEFT, 0, 20, 17, 516, 45);
        add(L"STATIC", W(L"配对地址", L"配对地址喵"), 0, 0, 20, 78, 104, 24);
        dialog->pairAddress = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 10, 127, 73, 302, 27);
        add(L"STATIC", W(L"六位配对码", L"六位配对码喵"), 0, 0, 20, 121, 104, 24);
        dialog->code = add(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, 11, 127, 116, 188, 27);
        SendMessageW(dialog->code, EM_SETLIMITTEXT, 6, 0);
        add(L"BUTTON", W(L"配对", L"配对喵"), BS_PUSHBUTTON | WS_TABSTOP, 20, 328, 116, 101, 28);
        add(L"STATIC", W(L"连接地址", L"连接地址喵"), 0, 0, 20, 173, 104, 24);
        dialog->connectAddress = add(L"EDIT", Wide(controller->androidEndpoint).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 12, 127, 168, 302, 27);
        add(L"BUTTON", W(L"连接", L"连接喵"), BS_PUSHBUTTON | WS_TABSTOP, 21, 441, 167, 91, 28);
        add(L"STATIC", W(L"先配对，再使用无线调试主页的连接地址。重启后端口可能变化。", L"先配对，再用无线调试主页的连接地址喵。重启后端口可能变化。"), SS_LEFT, 0, 20, 216, 516, 42);
        add(L"BUTTON", W(L"关闭", L"关闭喵"), BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 417, 267, 114, 30); return 0;
    }
    if (message == WM_COMMAND && (LOWORD(wp) == 20 || LOWORD(wp) == 21)) {
        at::Request request;
        const bool pair = LOWORD(wp) == 20;
        request.action = pair ? at::Action::Pair : at::Action::Connect;
        request.endpoint = Utf8(Text(pair ? dialog->pairAddress : dialog->connectAddress));
        request.pairingCode = Utf8(Text(dialog->code));
        if (!at::ValidEndpoint(request.endpoint)) { controller->Error(W(L"请输入地址:端口，例如 192.168.1.8:37123。", L"请输入地址:端口喵，例如 192.168.1.8:37123。")); return 0; }
        if (!pair) controller->androidEndpoint = request.endpoint;
        if (controller->androidTools.Start(controller->main, std::move(request))) { SetWindowTextW(dialog->code, L""); DestroyWindow(window); controller->UpdateAndroidTools(); }
        return 0;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { if (dialog->font) DeleteObject(dialog->font); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
void ShowWireless(Application& controller) {
    WirelessDialog dialog; dialog.application = &controller;
    WNDCLASSW cls{}; cls.hInstance = controller.instance; cls.lpfnWndProc = WirelessProc; cls.lpszClassName = L"QPointerWireless"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    const int dpi = static_cast<int>(GetDpiForWindow(controller.main));
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, W(L"无线 ADB", L"无线 ADB 喵"), WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(572, dpi, 96), MulDiv(353, dpi, 96), controller.main, nullptr, controller.instance, &dialog);
    if (!window) return;
    EnableWindow(controller.main, FALSE); ShowWindow(window, SW_SHOW);
    MSG message{};
    while (IsWindow(window)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) { DestroyWindow(window); if (!result) PostQuitMessage(static_cast<int>(message.wParam)); break; }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    EnableWindow(controller.main, TRUE); SetForegroundWindow(controller.main);
}
void PresetImage(int shape, const std::wstring& path) {
    Gdiplus::Bitmap image(96, 96, PixelFormat32bppARGB); Gdiplus::Graphics graphics(&image); graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen outline(Gdiplus::Color(255, 16, 22, 30), 8), stroke(Gdiplus::Color(255, 255, 255, 255), 4);
    if (shape == 0) {
        for (auto* pen : {&outline, &stroke}) { graphics.DrawLine(pen, 48, 10, 48, 86); graphics.DrawLine(pen, 10, 48, 86, 48); }
    } else if (shape == 1) {
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 255, 255, 255)); graphics.FillEllipse(&fill, 20, 20, 56, 56); graphics.DrawEllipse(&outline, 20, 20, 56, 56);
    } else {
        Gdiplus::Point points[] = {{10, 8}, {12, 81}, {32, 63}, {48, 88}, {63, 79}, {47, 54}, {76, 50}};
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 255, 255, 255)); graphics.FillPolygon(&fill, points, 7); graphics.DrawPolygon(&stroke, points, 7); graphics.DrawPolygon(&outline, points, 7);
    }
    UINT count = 0, bytes = 0; Gdiplus::GetImageEncodersSize(&count, &bytes); std::vector<BYTE> storage(bytes); auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (!bytes || Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) throw std::runtime_error("PNG 编码器不可用");
    for (UINT i = 0; i < count; ++i) if (wcscmp(codecs[i].MimeType, L"image/png") == 0 && image.Save(path.c_str(), &codecs[i].Clsid, nullptr) == Gdiplus::Ok) return;
    throw std::runtime_error("预设图片保存失败");
}
}
void Application::RememberAndroidDraft() {
    if (!Has(ID_ANDROID_SIZE)) return;
    const auto index = Selected(ID_ANDROID_DEVICE);
    if (index >= 0 && index < static_cast<int>(androidDevices.size())) androidSerial = androidDevices[index].serial;
    androidShape = std::max(0, Selected(ID_ANDROID_SHAPE)); androidProfile = std::max(0, Selected(ID_ANDROID_PROFILE));
    androidVisible = Checked(ID_ANDROID_VISIBLE); androidUseRoot = Checked(ID_ANDROID_USE_ROOT);
    try { const auto value = Text(Item(ID_ANDROID_SIZE)); size_t used = 0; const int size = std::stoi(value, &used); if (used == value.size() && size >= 16 && size <= 96) androidSizeDp = size; } catch (...) {}
}
void Application::BuildAndroidTools() {
    androidDevicesNeedRefresh = true;
    if (!androidInitialized) {
        try {
            const auto directory = at::DataDirectory(), prefs = directory / L"preferences.ini";
            androidSerial = Utf8(ReadIni(L"serial", L"", prefs.wstring())); androidEndpoint = Utf8(ReadIni(L"endpoint", L"", prefs.wstring()));
            androidShape = std::clamp(static_cast<int>(ReadNumber(L"shape", 0, prefs.wstring())), 0, 3);
            androidSizeDp = std::clamp(static_cast<int>(ReadNumber(L"sizeDp", 32, prefs.wstring())), 16, 96);
            androidImagePath = (directory / (androidShape == 3 ? L"custom_cursor.png" : L"preset.png")).wstring();
            if (androidShape != 3) PresetImage(androidShape, androidImagePath);
            auto hotspot = qp::LoadCustomCursorHotspot((directory / L"custom_cursor.ini").wstring());
            androidHotspotX = androidShape == 3 ? hotspot.x : androidShape == 2 ? 10.0 / 95 : .5;
            androidHotspotY = androidShape == 3 ? hotspot.y : androidShape == 2 ? 8.0 / 95 : .5;
            std::wstring error; androidPreview = qp::LoadCustomCursorImage(androidImagePath, error); androidInitialized = true;
        } catch (const std::exception& error) { androidDetails = Wide(error.what()); }
    }
    const int p = 5;
    Add(L"BUTTON", W(L"连接手机", L"连接手机喵"), BS_GROUPBOX, 0, 26, 102, 826, 109, p);
    Combo(ID_ANDROID_DEVICE, {W(L"点击刷新，选择已授权的 Android 手机", L"点刷新，选择已授权的手机喵")}, 0, 43, 127, 425, p);
    Button(ID_ANDROID_REFRESH, W(L"刷新", L"刷新喵"), 480, 126, 78, p);
    Button(ID_ANDROID_WIRELESS, W(L"无线连接…", L"无线连接喵…"), 570, 126, 124, p);
    Button(ID_ANDROID_INSPECT, W(L"检查手机", L"检查手机喵"), 706, 126, 129, p);
    Add(L"STATIC", W(L"手机需开启 USB 调试；数据线直连电脑、解锁并授权。", L"开启 USB 调试喵；数据线直连电脑、解锁并授权。"), SS_LEFT | SS_ENDELLIPSIS, ID_ANDROID_PHONE, 43, 171, 646, 26, p);
    Button(ID_ANDROID_ROOT, W(L"检查 ROOT", L"检查 ROOT 喵"), 706, 167, 129, p);
    Add(L"BUTTON", W(L"手写笔光标", L"手写笔光标喵"), BS_GROUPBOX, 0, 26, 222, 826, 170, p);
    Add(L"STATIC", W(L"先检查手机；开启后需重启并确认实际悬停图标。", L"先检查手机喵；开启后重启并确认实际悬停图标。"), SS_LEFT | SS_ENDELLIPSIS, ID_ANDROID_SETTING, 43, 247, 792, 25, p);
    Button(ID_ANDROID_ENABLE, W(L"开启手写笔光标", L"开启手写笔光标喵"), 43, 282, 166, p);
    Button(ID_ANDROID_RESTORE_SETTING, W(L"恢复开启前设置", L"恢复开启前设置喵"), 222, 282, 166, p);
    Button(ID_ANDROID_REBOOT, W(L"重启手机", L"重启手机喵"), 401, 282, 127, p);
    Check(ID_ANDROID_USE_ROOT, W(L"使用已授权的 ROOT", L"使用已授权 ROOT 喵"), androidUseRoot, 548, 283, 285, p);
    Add(L"STATIC", W(L"触摸反馈：尚未检查", L"触摸反馈：还没检查喵"), SS_LEFT | SS_ENDELLIPSIS, ID_ANDROID_TOUCH_STATE, 43, 330, 186, 24, p);
    Button(ID_ANDROID_TOUCH_ENABLE, W(L"开启触摸反馈", L"开启触摸反馈喵"), 241, 322, 146, p);
    Button(ID_ANDROID_TOUCH_DISABLE, W(L"关闭触摸反馈", L"关闭触摸反馈喵"), 401, 322, 146, p);
    Button(ID_ANDROID_TOUCH_RESTORE, W(L"恢复原反馈设置", L"恢复原反馈设置喵"), 561, 322, 166, p);
    Add(L"STATIC", W(L"触摸反馈可显示悬停圆点，也会显示手指触摸点；用 P4 移动确认。", L"触摸反馈可显示悬停圆点喵，也显示手指触点；用 P4 移动确认。"), SS_LEFT | SS_ENDELLIPSIS, ID_ANDROID_TOUCH_NOTE, 43, 362, 790, 24, p);
    Add(L"BUTTON", W(L"光标外观", L"光标外观喵"), BS_GROUPBOX, 0, 26, 405, 826, 173, p);
    Add(L"BUTTON", L"", BS_OWNERDRAW, ID_ANDROID_PREVIEW, 43, 435, 120, 125, p);
    Combo(ID_ANDROID_SHAPE, {W(L"十字", L"十字喵"), W(L"圆点", L"圆点喵"), W(L"箭头", L"箭头喵"), W(L"自定义图片", L"自定义图片喵")}, androidShape, 182, 430, 145, p);
    Button(ID_ANDROID_EDIT, W(L"选择 / 编辑图片", L"选择 / 编辑图片喵"), 341, 430, 182, p);
    Button(ID_ANDROID_PC_IMAGE, W(L"使用 PC 自定义图片", L"使用 PC 自定义图片喵"), 537, 430, 296, p);
    Label(W(L"大小", L"大小喵"), 182, 472, 46, p); Edit(ID_ANDROID_SIZE, std::to_wstring(androidSizeDp), 232, 467, 61, p); Label(L"dp", 303, 472, 27, p);
    Combo(ID_ANDROID_PROFILE, {W(L"选择实际悬停路径", L"选择实际悬停路径喵"), W(L"系统十字图标", L"系统十字图标喵"), W(L"系统悬停圆点", L"系统悬停圆点喵")}, androidProfile, 341, 467, 222, p);
    Check(ID_ANDROID_VISIBLE, W(L"已确认手机显示该图标", L"已确认手机显示该图标喵"), androidVisible, 577, 467, 256, p);
    Add(L"STATIC", W(L"先分析手机资源；系统选择空图标时没有图片可覆盖。", L"先分析手机资源喵；系统选空图标时没有图片可覆盖。"), SS_LEFT | SS_ENDELLIPSIS, ID_ANDROID_POLICY, 182, 501, 650, 24, p);
    Button(ID_ANDROID_BUILD, W(L"生成安装包", L"生成安装包喵"), 182, 536, 146, p);
    Button(ID_ANDROID_EXPORT, W(L"导出模块…", L"导出模块喵…"), 342, 536, 146, p);
    Button(ID_ANDROID_INSTALL, W(L"应用到手机…", L"应用到手机喵…"), 502, 536, 156, p);
    Button(ID_ANDROID_RESTORE_IMAGE, W(L"恢复原光标", L"恢复原光标喵"), 672, 536, 161, p);
    Add(L"STATIC", W(L"选择手机后开始检查。", L"选好手机后开始检查喵。"), SS_LEFT, ID_ANDROID_PROGRESS, 26, 587, 826, 43, p);
    Button(ID_ANDROID_CANCEL, W(L"取消操作", L"取消操作喵"), 26, 643, 128, p);
    Button(ID_ANDROID_DETAILS, W(L"诊断详情", L"诊断详情喵"), 169, 643, 128, p);
    Button(ID_ANDROID_ANALYZE, W(L"分析手机资源", L"分析手机资源喵"), 312, 643, 169, p);
    Button(ID_ANDROID_VERIFY, W(L"检查覆盖状态", L"检查覆盖状态喵"), 496, 643, 169, p);
}
void Application::UpdateAndroidTools() {
    if (!Has(ID_ANDROID_DEVICE)) return;
    auto toolsState = androidTools.Snapshot(); androidDetails = toolsState.detail;
    if (!toolsState.busy && toolsState.generation != androidCompletedGeneration) {
        androidCompletedGeneration = toolsState.generation;
        if (!toolsState.failed && toolsState.action == at::Action::Root && toolsState.phone && toolsState.phone->root) {
            androidUseRoot = true; SendMessageW(Item(ID_ANDROID_USE_ROOT), BM_SETCHECK, BST_CHECKED, 0);
        }
        if (!toolsState.failed && toolsState.action == at::Action::Dependencies) {
            ShellExecuteW(main, L"open", at::RuntimeDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    bool changed = androidDevicesNeedRefresh || toolsState.devices.size() != androidDevices.size();
    for (size_t i = 0; !changed && i < androidDevices.size(); ++i) changed = toolsState.devices[i].serial != androidDevices[i].serial || toolsState.devices[i].state != androidDevices[i].state;
    if (changed || (androidDevices.empty() && toolsState.devices.empty())) {
        androidDevicesNeedRefresh = false;
        androidDevices = toolsState.devices; SendMessageW(Item(ID_ANDROID_DEVICE), CB_RESETCONTENT, 0, 0); int selection = -1;
        for (size_t i = 0; i < androidDevices.size(); ++i) {
            const auto& d = androidDevices[i]; std::wstring label = Wide(d.model.empty() ? d.serial : d.model + " · " + d.serial);
            label += d.state == "device" ? W(L" · 已授权", L" · 已授权喵") : d.state == "unauthorized" ? W(L" · 请在手机授权", L" · 请在手机授权喵") : W(L" · 离线或不可用", L" · 离线或不可用喵");
            SendMessageW(Item(ID_ANDROID_DEVICE), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())); if (d.serial == androidSerial) selection = static_cast<int>(i);
        }
        if (androidDevices.empty()) SendMessageW(Item(ID_ANDROID_DEVICE), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(L"未发现手机，请检查 USB 调试、数据线和 ADB 驱动", L"没找到手机，检查 USB 调试、数据线和 ADB 驱动喵")));
        if (androidDevices.empty()) selection = 0;
        // 多设备不自动选第一台；单设备也不把操作恢复到另一个已记住的身份。
        if (selection < 0 && androidSerial.empty() && androidDevices.size() == 1) { selection = 0; androidSerial = androidDevices[0].serial; }
        SendMessageW(Item(ID_ANDROID_DEVICE), CB_SETCURSEL, selection, 0);
    }
    const auto selection = Selected(ID_ANDROID_DEVICE);
    const bool device = selection >= 0 && selection < static_cast<int>(androidDevices.size()) && androidDevices[selection].state == "device";
    const bool checked = device && toolsState.phone && toolsState.phone->serial == androidDevices[selection].serial;
    for (int id = ID_ANDROID_DEVICE; id <= ID_ANDROID_VERIFY; ++id) Enable(id, !toolsState.busy);
    for (int id : {ID_ANDROID_INSPECT, ID_ANDROID_ROOT}) Enable(id, device && !toolsState.busy);
    for (int id : {ID_ANDROID_ENABLE, ID_ANDROID_RESTORE_SETTING, ID_ANDROID_REBOOT, ID_ANDROID_ANALYZE, ID_ANDROID_VERIFY}) Enable(id, checked && !toolsState.busy);
    for (int id : {ID_ANDROID_TOUCH_ENABLE, ID_ANDROID_TOUCH_DISABLE, ID_ANDROID_TOUCH_RESTORE}) Enable(id, checked && !toolsState.busy);
    Enable(ID_ANDROID_USE_ROOT, checked && toolsState.phone->root && !toolsState.busy);
    for (int id : {ID_ANDROID_BUILD, ID_ANDROID_EXPORT}) Enable(id, checked && !toolsState.phone->framework.empty() && !toolsState.busy);
    Enable(ID_ANDROID_INSTALL, checked && !toolsState.package.empty() && !toolsState.busy);
    Enable(ID_ANDROID_RESTORE_IMAGE, checked && !toolsState.busy); Enable(ID_ANDROID_CANCEL, toolsState.busy);
    if (checked) {
        const auto& p = *toolsState.phone;
        Set(ID_ANDROID_PHONE, Wide(p.model) + L" · API " + std::to_wstring(p.sdk) + W(L" · 已授权", L" · 已授权喵") + (p.root ? L" · ROOT" : L""));
        const auto build = p.stylusBuildEnabled ? (*p.stylusBuildEnabled ? W(L"允许", L"允许喵") : W(L"关闭", L"关闭喵")) : W(L"未知", L"未知喵");
        Set(ID_ANDROID_SETTING, W(L"强制属性：", L"强制属性喵：") + Wide(p.force.empty() ? "—" : p.force) + W(L"   用户开关：", L"   用户开关喵：") + Wide(p.setting.value_or("—")) + W(L"   ROM 支持：", L"   ROM 支持喵：") + build + W(L"   请在手机确认实际显示", L"   请在手机确认实际显示喵"));
        Set(ID_ANDROID_TOUCH_STATE, std::wstring(W(L"触摸反馈：", L"触摸反馈喵：")) + (!p.showTouchesKnown ? W(L"未知", L"未知喵")
            : p.showTouches == std::optional<std::string>("1") ? W(L"已开启", L"已开启喵")
            : p.showTouches ? W(L"已关闭", L"已关闭喵") : W(L"默认关闭", L"默认关闭喵")));
        Set(ID_ANDROID_POLICY, p.framework.empty() ? W(L"请分析手机资源；默认空图标不能通过图片覆盖显示。", L"请分析手机资源喵；空图标不能靠图片覆盖显示。") : W(L"资源已读取；请选择实际悬停路径，安装后检查覆盖权限与显示。", L"资源读好啦；选实际悬停路径，安装后检查权限和显示喵。"));
    } else {
        const bool waiting = selection >= 0 && selection < static_cast<int>(androidDevices.size()) &&
            (androidDevices[selection].state == "unauthorized" || androidDevices[selection].state == "authorizing");
        Set(ID_ANDROID_PHONE, waiting ? W(L"已发现手机，等待授权：请解锁并允许 USB 调试。", L"找到手机啦，等待授权喵：解锁并允许 USB 调试。")
            : W(L"手机需开启 USB 调试；数据线直连电脑、解锁并授权。", L"开启 USB 调试喵；数据线直连电脑、解锁并授权。"));
        Set(ID_ANDROID_SETTING, W(L"尚未检查当前手机。", L"当前手机还没检查喵。"));
        Set(ID_ANDROID_TOUCH_STATE, W(L"触摸反馈：尚未检查", L"触摸反馈：还没检查喵"));
    }
    std::wstring progress = toolsState.busy ? W(L"正在操作：", L"正在操作喵：") : toolsState.failed ? W(L"操作未完成：", L"操作没完成喵：") : W(L"操作结果：", L"操作结果喵：");
    if (!toolsState.detail.empty()) {
        const auto paragraph = toolsState.detail.find(L"\n\n");
        progress += toolsState.detail.substr(0, std::min<size_t>(180, paragraph));
    }
    else progress = W(L"选定手机后开始检查。", L"选好手机后开始检查喵。");
    Set(ID_ANDROID_PROGRESS, progress);
}
void Application::BeginAndroidTask(at::Action action, const std::wstring& exportPath) {
    RememberAndroidDraft(); at::Request request; request.action = action;
    request.serial = androidSerial; request.useRoot = androidUseRoot; request.profile = androidProfile;
    request.confirmedVisible = androidVisible; request.image = androidImagePath; request.hotspotX = androidHotspotX; request.hotspotY = androidHotspotY;
    request.exportPath = exportPath;
    if (action == at::Action::Build) {
        try { const auto text = Text(Item(ID_ANDROID_SIZE)); size_t used = 0; request.sizeDp = std::stoi(text, &used); if (used != text.size() || request.sizeDp < 16 || request.sizeDp > 96) throw std::runtime_error("size"); }
        catch (...) { Error(W(L"大小请输入 16～96 dp 的整数。", L"大小请输入 16～96 dp 的整数喵。")); return; }
        if (!androidVisible || !androidProfile) { Error(W(L"先确认手机显示悬停图标，再选择它的实际资源路径。", L"先确认手机显示悬停图标，再选实际资源路径喵。")); return; }
    }
    if (action != at::Action::Refresh && action != at::Action::Dependencies && request.serial.empty()) { Error(W(L"请先选择目标手机。", L"先选择目标手机喵。")); return; }
    if (androidTools.Start(main, std::move(request))) {
        UpdateAndroidTools();
        SaveAndroidPreferences();
    }
}
bool Application::SelectAndroidImage() {
    try {
        const auto directory = at::DataDirectory();
        auto source = androidImagePath; double x = androidHotspotX, y = androidHotspotY;
        if (!EditCursorDraft(W(L"Android 光标图片与热点", L"Android 光标图片与热点喵"), source, x, y)) return false;
        std::wstring error;
        const auto destination = directory / L"custom_cursor.png";
        if (!qp::SaveCustomCursorFiles(source, destination.wstring(), (directory / L"custom_cursor.ini").wstring(), {x, y}, error)) { Error(error); return false; }
        androidImagePath = destination.wstring(); androidHotspotX = x; androidHotspotY = y; androidShape = 3;
        SendMessageW(Item(ID_ANDROID_SHAPE), CB_SETCURSEL, 3, 0);
        androidPreview = qp::LoadCustomCursorImage(androidImagePath, error); InvalidateRect(Item(ID_ANDROID_PREVIEW), nullptr, FALSE);
        return true;
    } catch (const std::exception& error) { Error(Wide(error.what())); return false; }
}
bool Application::HandleAndroidCommand(int id, int event) {
    if (id < ID_ANDROID_DEVICE || id > ID_ANDROID_TOUCH_NOTE) return false;
    const int previousShape = androidShape;
    if (event == CBN_SELCHANGE || event == BN_CLICKED) RememberAndroidDraft();
    if (androidTools.Snapshot().busy && id != ID_ANDROID_CANCEL && id != ID_ANDROID_DETAILS) return true;
    switch (id) {
    case ID_ANDROID_DEVICE: if (event == CBN_SELCHANGE) { androidVisible = false; SendMessageW(Item(ID_ANDROID_VISIBLE), BM_SETCHECK, BST_UNCHECKED, 0); UpdateAndroidTools(); } break;
    case ID_ANDROID_REFRESH: BeginAndroidTask(at::Action::Refresh); break;
    case ID_ANDROID_WIRELESS: ShowWireless(*this); break;
    case ID_ANDROID_INSPECT: BeginAndroidTask(at::Action::Inspect); break;
    case ID_ANDROID_ROOT: BeginAndroidTask(at::Action::Root); break;
    case ID_ANDROID_ENABLE: BeginAndroidTask(at::Action::Enable); break;
    case ID_ANDROID_TOUCH_ENABLE: BeginAndroidTask(at::Action::EnableTouchFeedback); break;
    case ID_ANDROID_TOUCH_DISABLE: BeginAndroidTask(at::Action::DisableTouchFeedback); break;
    case ID_ANDROID_TOUCH_RESTORE: BeginAndroidTask(at::Action::RestoreTouchFeedback); break;
    case ID_ANDROID_RESTORE_SETTING: BeginAndroidTask(at::Action::RestoreSetting); break;
    case ID_ANDROID_REBOOT:
        if (MessageBoxW(main, W(L"现在重启选定手机？请先保存手机上正在进行的工作。", L"现在重启这台手机喵？先保存手机上的工作。"), W(L"重启手机", L"重启手机喵"), MB_YESNO | MB_ICONQUESTION) == IDYES) BeginAndroidTask(at::Action::Reboot); break;
    case ID_ANDROID_ANALYZE: BeginAndroidTask(at::Action::Analyze); break;
    case ID_ANDROID_VERIFY: BeginAndroidTask(at::Action::Verify); break;
    case ID_ANDROID_BUILD: BeginAndroidTask(at::Action::Build); break;
    case ID_ANDROID_EXPORT: {
        wchar_t path[32768] = L"QInEX-Stylus-Cursor.zip"; OPENFILENAMEW save{sizeof(save)}; save.hwndOwner = main; save.lpstrFile = path; save.nMaxFile = 32768;
        save.lpstrFilter = L"Magisk 模块 (*.zip)\0*.zip\0\0"; save.lpstrDefExt = L"zip"; save.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&save)) BeginAndroidTask(at::Action::Build, path); break;
    }
    case ID_ANDROID_INSTALL: {
        TASKDIALOG_BUTTON buttons[] = {{1, W(L"Magisk 系统覆盖（需 ROOT，安装后重启）", L"Magisk 系统覆盖喵（需 ROOT，安装后重启）")}, {2, W(L"普通覆盖（仅适用于允许公开覆盖的资源）", L"普通覆盖喵（资源须允许公开覆盖）")}};
        TASKDIALOGCONFIG dialog{sizeof(dialog)}; dialog.hwndParent = main; dialog.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION; dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        dialog.pszWindowTitle = W(L"应用手机光标", L"应用手机光标喵"); dialog.pszMainInstruction = W(L"选择安装方式", L"选择安装方式喵"); dialog.pszContent = W(L"安装使用已生成的手机专用包。资源权限和实际显示需要安装后检查。", L"使用已生成的手机专用包喵。安装后检查资源权限和实际显示。"); dialog.pButtons = buttons; dialog.cButtons = 2; int selected = 0;
        if (SUCCEEDED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr))) { if (selected == 1) BeginAndroidTask(at::Action::InstallMagisk); else if (selected == 2) BeginAndroidTask(at::Action::InstallOverlay); } break;
    }
    case ID_ANDROID_RESTORE_IMAGE: {
        TASKDIALOG_BUTTON buttons[] = {{1, W(L"恢复系统原光标", L"恢复系统原光标喵")}, {2, W(L"恢复上次安装前的 Magisk 模块", L"恢复上次安装前的 Magisk 模块喵")}};
        TASKDIALOGCONFIG dialog{sizeof(dialog)}; dialog.hwndParent = main; dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON; dialog.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        dialog.pszWindowTitle = W(L"恢复光标外观", L"恢复光标外观喵"); dialog.pszMainInstruction = W(L"选择恢复目标", L"选择恢复目标喵"); dialog.pButtons = buttons; dialog.cButtons = 2; int selected = 0;
        if (SUCCEEDED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr))) { if (selected == 1) BeginAndroidTask(at::Action::RestoreImage); else if (selected == 2) BeginAndroidTask(at::Action::RestorePrevious); } break;
    }
    case ID_ANDROID_CANCEL: androidTools.Cancel(); break;
    case ID_ANDROID_DETAILS: {
        const auto detail = androidDetails.empty() ? std::wstring(W(L"尚无操作记录。", L"还没有操作记录喵。")) : androidDetails.substr(0, 16384);
        TASKDIALOG_BUTTON buttons[] = {{1, W(L"导出诊断", L"导出诊断喵")}, {2, W(L"查看依赖与许可", L"查看依赖与许可喵")}, {3, W(L"打开恢复记录", L"打开恢复记录喵")}};
        TASKDIALOGCONFIG dialog{sizeof(dialog)}; dialog.hwndParent = main; dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON; dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        dialog.pszWindowTitle = W(L"Android 工具诊断", L"Android 工具诊断喵"); dialog.pszMainInstruction = W(L"操作详情", L"操作详情喵"); dialog.pszContent = detail.c_str(); dialog.pButtons = buttons; dialog.cButtons = 3; dialog.cxWidth = 520; int selected = 0;
        if (SUCCEEDED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr))) {
            try {
                if (selected == 1) {
                    wchar_t path[32768] = L"QInEX-Android-diagnostics.txt"; OPENFILENAMEW save{sizeof(save)}; save.hwndOwner = main; save.lpstrFile = path; save.nMaxFile = 32768; save.lpstrFilter = L"文本 (*.txt)\0*.txt\0\0"; save.lpstrDefExt = L"txt"; save.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetSaveFileNameW(&save)) at::AtomicWrite(path, Utf8(androidDetails));
                } else if (selected == 2) BeginAndroidTask(at::Action::Dependencies);
                else if (selected == 3) { auto path = at::DataDirectory() / L"devices"; std::filesystem::create_directories(path); ShellExecuteW(main, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
            } catch (const std::exception& error) { Error(Wide(error.what())); }
        } break;
    }
    case ID_ANDROID_EDIT: SelectAndroidImage(); break;
    case ID_ANDROID_PC_IMAGE: {
        if (!customCursorImage) { Error(W(L"请先在 PC 设置页保存自定义图片。", L"先在 PC 设置页保存自定义图片喵。")); break; }
        try {
            const auto directory = at::DataDirectory(); std::wstring error;
            androidImagePath = (directory / L"custom_cursor.png").wstring();
            if (!qp::SaveCustomCursorFiles(storageDirectory + L"\\custom_cursor.png", androidImagePath, (directory / L"custom_cursor.ini").wstring(), {customCursorHotspotX, customCursorHotspotY}, error)) { Error(error); break; }
            androidHotspotX = customCursorHotspotX; androidHotspotY = customCursorHotspotY; androidShape = 3;
            SendMessageW(Item(ID_ANDROID_SHAPE), CB_SETCURSEL, 3, 0); androidPreview = qp::LoadCustomCursorImage(androidImagePath, error); InvalidateRect(Item(ID_ANDROID_PREVIEW), nullptr, FALSE);
        } catch (const std::exception& error) { Error(Wide(error.what())); } break;
    }
    case ID_ANDROID_SHAPE:
        if (event == CBN_SELCHANGE) {
            if (androidShape == 3) { if (!SelectAndroidImage()) { androidShape = previousShape; SendMessageW(Item(ID_ANDROID_SHAPE), CB_SETCURSEL, previousShape, 0); } break; }
            try { androidImagePath = (at::DataDirectory() / L"preset.png").wstring(); PresetImage(androidShape, androidImagePath); androidHotspotX = androidShape == 2 ? 10.0 / 95 : .5; androidHotspotY = androidShape == 2 ? 8.0 / 95 : .5;
                std::wstring error; androidPreview = qp::LoadCustomCursorImage(androidImagePath, error); InvalidateRect(Item(ID_ANDROID_PREVIEW), nullptr, FALSE); }
            catch (const std::exception& error) { Error(Wide(error.what())); }
        } break;
    default: break;
    }
    return true;
}
void Application::SaveAndroidPreferences() {
    if (!androidInitialized) return;
    try {
        RememberAndroidDraft(); const auto prefs = (at::DataDirectory() / L"preferences.ini").wstring();
        WritePrivateProfileStringW(L"pointer", L"serial", Wide(androidSerial).c_str(), prefs.c_str());
        WritePrivateProfileStringW(L"pointer", L"endpoint", Wide(androidEndpoint).c_str(), prefs.c_str());
        WritePrivateProfileStringW(L"pointer", L"shape", std::to_wstring(androidShape).c_str(), prefs.c_str());
        WritePrivateProfileStringW(L"pointer", L"sizeDp", std::to_wstring(androidSizeDp).c_str(), prefs.c_str());
    } catch (...) {}
}
void Application::PaintAndroidPreview(const DRAWITEMSTRUCT& item) {
    Gdiplus::Graphics graphics(item.hDC); const auto width = item.rcItem.right - item.rcItem.left, height = item.rcItem.bottom - item.rcItem.top;
    for (int y = 0; y < height; y += 12) for (int x = 0; x < width; x += 12) {
        Gdiplus::SolidBrush tile((x / 12 + y / 12) % 2 ? Gdiplus::Color(255, 225, 229, 234) : Gdiplus::Color(255, 249, 250, 252)); graphics.FillRectangle(&tile, x, y, 12, 12);
    }
    if (!androidPreview) return;
    const auto& image = *androidPreview;
    Gdiplus::Bitmap bitmap(image.width, image.height, image.stride, PixelFormat32bppPARGB, const_cast<BYTE*>(image.premultipliedBgra.data()));
    const double ratio = std::min((width - 20.0) / image.width, (height - 20.0) / image.height);
    const float w = static_cast<float>(image.width * ratio), h = static_cast<float>(image.height * ratio), x = (width - w) / 2, y = (height - h) / 2;
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic); graphics.DrawImage(&bitmap, Gdiplus::RectF(x, y, w, h));
    const float hx = x + static_cast<float>(androidHotspotX) * w, hy = y + static_cast<float>(androidHotspotY) * h;
    Gdiplus::Pen pen(Gdiplus::Color(255, 220, 45, 85), 1.5f); graphics.DrawLine(&pen, hx - 7, hy, hx + 7, hy); graphics.DrawLine(&pen, hx, hy - 7, hx, hy + 7);
}
