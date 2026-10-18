// ui_theme.h - 局内 overlay 与启动器共用的 ImGui 主题 / 字体
#pragma once
#include "imgui.h"

namespace ra3 {

// 字形范围：ImGui 自带的 ChineseSimplifiedCommon 不含箭头等符号，会显示成 '?'。
inline const ImWchar* UiGlyphRanges() {
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,   // 基本拉丁 + 拉丁补充（含 × · °）
        0x2000, 0x206F,   // 常用标点（— … ‘ ’）
        0x2190, 0x21FF,   // 箭头 ← ↑ → ↓
        0x2460, 0x24FF,   // 带圈数字 ①②
        0x25A0, 0x25FF,   // 几何图形 ● ■ ▲
        0x2600, 0x26FF,   // 杂项符号 ★ ☆
        0x3000, 0x30FF,   // CJK 标点（「」【】）+ 假名
        0x31F0, 0x31FF,
        0xFF00, 0xFFEF,   // 全角形式
        0x4E00, 0x9FAF,   // CJK 基本区
        0,
    };
    return ranges;
}

inline ImFont* AddUiFont(ImGuiIO& io, float px) {
    ImFontConfig fc;
    fc.OversampleH = 1;
    fc.OversampleV = 1;
    fc.PixelSnapH = true;
    ImFont* f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", px, &fc, UiGlyphRanges());
    if (!f) f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", px, &fc, UiGlyphRanges());
    if (!f) f = io.Fonts->AddFontDefault();
    return f;
}

// 深色底 + 青色高亮；局内 overlay 与启动器保持同一套观感。
inline void ApplyUiTheme(float windowAlpha = 0.95f) {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 4.f;
    st.FrameRounding = 3.f;
    st.GrabRounding = 3.f;
    st.WindowBorderSize = 1.f;
    st.FrameBorderSize = 0.f;
    st.ItemSpacing = ImVec2(8, 6);
    st.FramePadding = ImVec2(8, 4);

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.07f, 0.08f, 0.10f, windowAlpha);
    c[ImGuiCol_ChildBg]       = ImVec4(0.085f, 0.10f, 0.125f, 0.85f);
    c[ImGuiCol_PopupBg]       = ImVec4(0.07f, 0.08f, 0.10f, 0.98f);
    c[ImGuiCol_TitleBg]       = ImVec4(0.10f, 0.13f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.18f, 0.26f, 1.00f);
    c[ImGuiCol_Tab]           = ImVec4(0.11f, 0.14f, 0.19f, 1.00f);
    c[ImGuiCol_TabHovered]    = ImVec4(0.20f, 0.42f, 0.55f, 1.00f);
    c[ImGuiCol_TabSelected]   = ImVec4(0.16f, 0.34f, 0.46f, 1.00f);
    c[ImGuiCol_Header]        = ImVec4(0.16f, 0.30f, 0.40f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.44f, 0.56f, 1.00f);
    c[ImGuiCol_HeaderActive]  = ImVec4(0.26f, 0.50f, 0.62f, 1.00f);
    c[ImGuiCol_Button]        = ImVec4(0.16f, 0.32f, 0.44f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.46f, 0.60f, 1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.12f, 0.26f, 0.36f, 1.00f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.35f, 0.85f, 0.95f, 1.00f);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.35f, 0.75f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.90f, 1.00f, 1.00f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]= ImVec4(0.16f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.28f, 0.36f, 1.00f);
    c[ImGuiCol_Separator]     = ImVec4(0.22f, 0.28f, 0.34f, 1.00f);
    c[ImGuiCol_Text]          = ImVec4(0.88f, 0.91f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]  = ImVec4(0.52f, 0.58f, 0.64f, 1.00f);
    c[ImGuiCol_Border]        = ImVec4(0.20f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_ScrollbarBg]   = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.30f, 0.36f, 1.00f);
}

} // namespace ra3
