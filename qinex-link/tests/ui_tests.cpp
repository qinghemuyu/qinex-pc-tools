// 直接调用生产窗口过程和绘制，不启动输入捕获、串口、网络转发或第二个用户实例。
#define wWinMain LinkEntryPointUnderTest
#include "../src/main.cpp"
#undef wWinMain
#include "../../qinex-pointer/src/app.h"
#if QINEX_WITH_ANDROID_TOOLS
#include "../../qinex-pointer/src/embedded_runtime.h"
#endif
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Render(const std::filesystem::path& path) {
    HBITMAP bitmap = CaptureRect({0,0,double(S(BASE_W)),double(S(BASE_H))},g.hwnd);
    Check(bitmap != nullptr, "创建界面渲染失败");
    bool saved = SavePng(bitmap,path.wstring()); DeleteObject(bitmap);
    Check(saved,"保存界面渲染失败");
}
}

int wmain(int argc, wchar_t** argv) {
#if QINEX_WITH_ANDROID_TOOLS
    qp::android::ScopedDataDirectory androidData(std::filesystem::path(ExeDirectory()) / L"android-test");
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput input;
    if(Gdiplus::GdiplusStartup(&s_graphicsToken,&input,nullptr)!=Gdiplus::Ok) return 1;
    int result=0;
    try {
        HINSTANCE instance=GetModuleHandleW(nullptr);
        if(argc>=3) SC=std::stof(argv[2]);
        makeFonts();
        WNDCLASSW wc{}; wc.hInstance=instance; wc.lpfnWndProc=wndProc; wc.lpszClassName=L"QInEXLinkUiTest";
        Check(RegisterClassW(&wc)!=0,"注册测试窗口失败");
        g.hwnd=CreateWindowExW(0,wc.lpszClassName,L"QInEX 单窗口测试",WS_POPUP|WS_CLIPCHILDREN,
            0,0,S(BASE_W),S(BASE_H),nullptr,nullptr,instance,nullptr);
        Check(g.hwnd!=nullptr,"创建测试窗口失败");
        createNavigation(instance);
        // 顶部原生按钮保留输入语义，但擦背景和绘制必须走统一缓冲路径。
        Check(SendMessageW(s_pointerButton,WM_ERASEBKGND,0,0)==1,"顶部按钮不应擦出系统白底");
        SendMessageW(s_pointerButton,WM_MOUSEMOVE,0,MAKELPARAM(8,8));
        SendMessageW(s_pointerButton,BM_SETSTATE,TRUE,0);
        Check((SendMessageW(s_pointerButton,BM_GETSTATE,0,0)&BST_PUSHED)!=0,"动画不得破坏原生按下状态");
        SendMessageW(s_pointerButton,WM_TIMER,NAV_ANIMATION,0);
        SendMessageW(s_pointerButton,BM_SETSTATE,FALSE,0);
        SendMessageW(s_pointerButton,WM_MOUSELEAVE,0,0);
        SendMessageW(s_pointerButton,WM_TIMER,NAV_ANIMATION,0);
        Check(!s_navigation[1].hovered && !s_navigation[1].pressed &&
            s_navigation[1].hover==0 && s_navigation[1].press==0,"离开和松开后应清除动画状态");
        s_pointerReleaseMessage=RegisterWindowMessageW(L"QInEX.Pointer.ReleaseLocalInput.v1");
        std::filesystem::path output;
        if(argc>=2) { output=argv[1]; std::filesystem::create_directories(output); Render(output/L"device-dark.png"); }
        Check(ensurePointer(),"创建嵌入光标失败");
        const HWND child=app->main;
        Check(GetParent(child)==g.hwnd && (GetWindowLongPtrW(child,GWL_STYLE)&WS_CHILD),"光标必须属于同一主窗口");
        SendMessageW(g.hwnd,qp::desktop::kShowPointerMessage,0,0);
        ShowWindow(child,SW_SHOW);
        Check(s_pointerPage,"切换到光标页失败");
        Check(hitTest({rConnect().left+2,rConnect().top+2})==HIT_NONE,"光标页不应点击到背后的串口控件");
        for(int page=0;page<Application::kPageCount;++page) {
            app->SelectPage(page);
            RECT r{}; GetWindowRect(child,&r);
            const RECT bounds=rPointerContent();
            Check(r.right-r.left==bounds.right-bounds.left && r.bottom-r.top==bounds.bottom-bounds.top,"切换光标子页不应改变统一窗口大小");
            if(!output.empty()) Render(output/(L"pointer-"+std::to_wstring(page)+L".png"));
        }
        SendMessageW(g.hwnd,WM_COMMAND,ID_DEVICE_PAGE,0);
        Check(!s_pointerPage && !(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE),"切回设备页应隐藏光标内容");
        Check(hitTest({rConnect().left+2,rConnect().top+2})==HIT_CONNECT,"切回后串口命中区应恢复");
        Check(ensurePointer() && app->main==child,"反复切页必须复用同一光标实例");
        SetWindowTextW(app->Item(ID_SIZE),L"39");
        RECT suggested{0,0,BASE_W,BASE_H};
        SendMessageW(g.hwnd,WM_DPICHANGED,MAKELONG(120,120),reinterpret_cast<LPARAM>(&suggested));
        Check(Text(app->Item(ID_SIZE))==L"39","DPI 重建不应丢失未应用设置");
        RECT childPosition{}; GetWindowRect(child,&childPosition);
        MapWindowPoints(nullptr,g.hwnd,reinterpret_cast<POINT*>(&childPosition),2);
        const RECT expected=rPointerContent();
        Check(EqualRect(&childPosition,&expected),"DPI 变化后子页应保持嵌入位置");
        if(!output.empty()) {
            qp::glass::light=true; Render(output/L"device-light.png");
            app->settings.anime=true; Messages::anime=true; app->BuildControls();
            SendMessageW(g.hwnd,qp::desktop::kShowPointerMessage,0,0); ShowWindow(child,SW_SHOW);
            app->SelectPage(4); Render(output/L"pointer-light.png");
        }
        Check(!g.forwarding && !g.grabbed,"界面测试不得启动转发或独占");
        EnableWindow(g.hwnd,FALSE);
        requestExit();
        Check(IsWindow(g.hwnd) && app!=nullptr,"模态编辑期间不得销毁宿主与光标状态");
        EnableWindow(g.hwnd,TRUE);
        DestroyWindow(g.hwnd); g.hwnd=nullptr;
        Check(app==nullptr && !IsWindow(child),"宿主销毁应释放内置光标");
        std::cout << "单窗口父子关系、切页、点击隔离、各功能页尺寸与统一退出通过\n";
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n'; result=1;
        if(IsWindow(g.hwnd)) DestroyWindow(g.hwnd);
        qp::desktop::Shutdown();
    }
    freeFonts();
    Gdiplus::GdiplusShutdown(s_graphicsToken);
    return result;
}
