#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include "ui_theme.h"

// 官网同系深色玻璃：细网格、轻微绿色透光、单像素边缘；无额外动画或图形依赖。
namespace qpointer::glass {
inline bool light = false;
inline UiPalette Palette() { return PointerPalette(light); }
inline COLORREF Gold() { return light ? RGB(137, 94, 23) : RGB(244, 191, 83); }
inline COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB(int(GetRValue(a)+(GetRValue(b)-GetRValue(a))*t),
        int(GetGValue(a)+(GetGValue(b)-GetGValue(a))*t), int(GetBValue(a)+(GetBValue(b)-GetBValue(a))*t));
}
inline Gdiplus::Color Color(COLORREF c, BYTE alpha = 255) {
    return {alpha, GetRValue(c), GetGValue(c), GetBValue(c)};
}
inline void Path(Gdiplus::GraphicsPath& path, const RECT& r, float radius) {
    const float x = r.left + .5f, y = r.top + .5f;
    const float w = float(r.right-r.left)-1, h = float(r.bottom-r.top)-1;
    const float d = std::max(1.f, std::min(radius*2, std::min(w,h)));
    path.AddArc(x,y,d,d,180,90); path.AddArc(x+w-d,y,d,d,270,90);
    path.AddArc(x+w-d,y+h-d,d,d,0,90); path.AddArc(x,y+h-d,d,d,90,90); path.CloseFigure();
}
inline void Background(HDC dc, RECT r) {
    if (r.right<=r.left || r.bottom<=r.top) return;
    const auto p = Palette();
    Gdiplus::Graphics graphics(dc);
    Gdiplus::Rect bounds(r.left,r.top,r.right-r.left,r.bottom-r.top);
    Gdiplus::LinearGradientBrush base(bounds,
        Color(light ? Mix(p.background,p.hover,.09) : RGB(6,37,26)),
        Color(light ? p.background : RGB(0,0,0)), 90.f);
    graphics.FillRectangle(&base,bounds);
    auto glow = [&](float x,float y,float w,float h,COLORREF tint,BYTE opacity) {
        Gdiplus::GraphicsPath shape; shape.AddEllipse(x,y,w,h);
        Gdiplus::PathGradientBrush brush(&shape);
        brush.SetCenterColor(Color(tint,opacity));
        Gdiplus::Color edge=Color(tint,0); int count=1; brush.SetSurroundColors(&edge,&count);
        graphics.FillPath(&brush,&shape);
    };
    const float w=float(bounds.Width), h=float(bounds.Height);
    glow(r.left+w*.20f,r.top-h*.35f,w*.9f,h*.90f,p.accent,light?14:9);
    // 静态坐标网格，不引入假坐标、性能读数或持续刷新的背景动画。
    const int step=std::max(24,int(w/20.f));
    Gdiplus::Pen grid(Color(p.accent,light?9:7),1.f);
    for(int x=r.left+step/2;x<r.right;x+=step) graphics.DrawLine(&grid,x,r.top,x,r.bottom);
    for(int y=r.top+step/2;y<r.bottom;y+=step) graphics.DrawLine(&grid,r.left,y,r.right,y);
}
inline void Surface(HDC dc, RECT r, float radius, COLORREF fill, COLORREF border, BYTE opacity = 0) {
    if(r.right-r.left<2 || r.bottom-r.top<2) return;
    Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // 官网按钮和卡片使用克制的圆角，避免膨胀胶囊和厚重的浮雕边。
    radius=std::min(radius,12.f);
    Gdiplus::GraphicsPath path; Path(path,r,radius);
    // 普通控件直接透出主窗口底图，只有操作强调色叠加极淡的状态高亮。
    const bool primary=GetGValue(fill)>GetRValue(fill)*1.7 && GetGValue(fill)>100;
    if (opacity || primary) {
        Gdiplus::SolidBrush material(Color(fill, opacity ? opacity : 18));
        graphics.FillPath(&material,&path);
    }
    Gdiplus::Pen rim(Color(border),1.f);
    graphics.DrawPath(&rim,&path);
}
}
