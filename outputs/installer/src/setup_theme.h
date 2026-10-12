#pragma once
// INSTALL19: COPY of the launcher palette and ImGui style (nWiiRuntime/src/ui/ui_app.cpp ApplyStyle, kGold/kParchment).
// Keep in sync by hand; LAUNCH19 may unify both later. Not shared code on purpose: the Setup must not depend on
// src/ui files that CTRL18/LAUNCH19 are changing.
#include "imgui.h"

namespace sb::theme {

inline const ImU32 kGold = IM_COL32(236, 200, 120, 255);
inline const ImU32 kGoldBright = IM_COL32(255, 226, 150, 255);
inline const ImU32 kParchment = IM_COL32(244, 232, 205, 255);
inline const ImU32 kDim = IM_COL32(158, 143, 115, 255);
inline const ImU32 kGreen = IM_COL32(150, 205, 120, 255);
inline const ImU32 kAmber = IM_COL32(240, 175, 70, 255);
inline const ImU32 kRed = IM_COL32(255, 115, 90, 255);

inline float& ScaleRef() {
    static float s = 1.0f;
    return s;
}
inline float Scale() { return ScaleRef(); }

inline void ApplyStyle(float s) {
    ScaleRef() = s;
    ImGuiStyle st;
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 8; st.ChildRounding = 6; st.FrameRounding = 5; st.PopupRounding = 6;
    st.GrabRounding = 4; st.TabRounding = 5; st.ScrollbarRounding = 6;
    st.WindowBorderSize = 1; st.FrameBorderSize = 0; st.PopupBorderSize = 1;
    st.WindowPadding = ImVec2(20, 16); st.FramePadding = ImVec2(10, 6); st.ItemSpacing = ImVec2(10, 10);
    st.ItemInnerSpacing = ImVec2(8, 6); st.ScrollbarSize = 14; st.GrabMinSize = 14;
    ImVec4* c = st.Colors;
    const ImVec4 gold(0.925f, 0.784f, 0.470f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.957f, 0.910f, 0.804f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.56f, 0.45f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.070f, 0.056f, 0.038f, 0.93f);
    c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.088f, 0.058f, 0.60f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.072f, 0.048f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(gold.x, gold.y, gold.z, 0.45f);
    c[ImGuiCol_FrameBg] = ImVec4(0.21f, 0.165f, 0.10f, 0.90f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.31f, 0.245f, 0.14f, 0.95f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.39f, 0.30f, 0.16f, 1.0f);
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = ImVec4(0.85f, 0.70f, 0.40f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.86f, 0.52f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.27f, 0.205f, 0.12f, 0.95f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.47f, 0.36f, 0.18f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.62f, 0.48f, 0.22f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(gold.x, gold.y, gold.z, 0.22f);
    c[ImGuiCol_HeaderHovered] = ImVec4(gold.x, gold.y, gold.z, 0.32f);
    c[ImGuiCol_HeaderActive] = ImVec4(gold.x, gold.y, gold.z, 0.45f);
    c[ImGuiCol_Separator] = ImVec4(gold.x, gold.y, gold.z, 0.35f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.20f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.55f, 0.45f, 0.27f, 0.8f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.70f, 0.57f, 0.32f, 0.9f);
    c[ImGuiCol_ScrollbarGrabActive] = gold;
    c[ImGuiCol_NavCursor] = gold; // the gold focus outline
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.20f, 0.16f, 0.10f, 1.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_PlotHistogram] = gold; // progress bars
    st.ScaleAllSizes(s);
    ImGui::GetStyle() = st;
}

} // namespace sb::theme
