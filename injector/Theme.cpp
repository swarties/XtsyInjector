// Theme.cpp — Applies the Gravity Client dark theme to ImGui.
#include "Theme.h"
#include "imgui.h"

// Configures ImGui style with a dark gray palette matching the injector UI.
void Theme::apply() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    const ImVec4 accent       = ImVec4(0.255f, 0.180f, 0.400f, 1.0f); // #412e66
    const ImVec4 accentHover  = ImVec4(0.305f, 0.220f, 0.470f, 1.0f);
    const ImVec4 accentActive = ImVec4(0.220f, 0.155f, 0.345f, 1.0f);

    // Core window colors — dark gray palette.
    colors[ImGuiCol_WindowBg]           = ImVec4(0.118f, 0.118f, 0.118f, 1.0f);  // #1E1E1E
    colors[ImGuiCol_ChildBg]            = ImVec4(0.118f, 0.118f, 0.118f, 0.0f);  // Transparent
    colors[ImGuiCol_PopupBg]            = ImVec4(0.157f, 0.157f, 0.157f, 1.0f);

    // Frame / input field colors.
    colors[ImGuiCol_FrameBg]            = ImVec4(0.165f, 0.165f, 0.165f, 1.0f);  // #2A2A2A
    colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.200f, 0.200f, 0.200f, 1.0f);  // #333333
    colors[ImGuiCol_FrameBgActive]      = ImVec4(0.230f, 0.230f, 0.230f, 1.0f);  // #3A3A3A

    // Title bar (hidden in our case, but set for completeness).
    colors[ImGuiCol_TitleBg]            = ImVec4(0.118f, 0.118f, 0.118f, 1.0f);
    colors[ImGuiCol_TitleBgActive]      = ImVec4(0.157f, 0.157f, 0.157f, 1.0f);

    // Button colors.
    colors[ImGuiCol_Button]             = ImVec4(0.200f, 0.200f, 0.200f, 1.0f);  // #333333
    colors[ImGuiCol_ButtonHovered]      = ImVec4(0.260f, 0.260f, 0.260f, 1.0f);
    colors[ImGuiCol_ButtonActive]       = accent;
    colors[ImGuiCol_Header]             = accent;
    colors[ImGuiCol_HeaderHovered]      = accentHover;
    colors[ImGuiCol_HeaderActive]       = accentActive;
    colors[ImGuiCol_Tab]                = accentActive;
    colors[ImGuiCol_TabHovered]         = accentHover;
    colors[ImGuiCol_TabActive]          = accent;
    colors[ImGuiCol_TabSelected]        = accent;
    colors[ImGuiCol_TabDimmed]          = ImVec4(0.180f, 0.140f, 0.240f, 1.0f);
    colors[ImGuiCol_TabDimmedSelected]  = accentActive;
    colors[ImGuiCol_TabDimmedSelectedOverline] = accent;
    colors[ImGuiCol_NavCursor]          = accent;
    colors[ImGuiCol_NavWindowingHighlight] = accent;
    colors[ImGuiCol_CheckMark]          = accent;
    colors[ImGuiCol_SliderGrab]         = accentHover;
    colors[ImGuiCol_SliderGrabActive]   = accent;

    // Scrollbar colors.
    colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.118f, 0.118f, 0.118f, 1.0f);
    colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.300f, 0.300f, 0.300f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.400f, 0.400f, 0.400f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.500f, 0.500f, 0.500f, 1.0f);

    // Progress bar color.
    colors[ImGuiCol_PlotHistogram]      = accent;

    // Border colors.
    colors[ImGuiCol_Border]             = ImVec4(0.240f, 0.240f, 0.240f, 1.0f);  // #3D3D3D
    colors[ImGuiCol_BorderShadow]       = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // Text colors.
    colors[ImGuiCol_Text]               = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);       // #FFFFFF
    colors[ImGuiCol_TextDisabled]       = ImVec4(0.533f, 0.533f, 0.533f, 1.0f);  // #888888

    // Style tweaks for a cleaner modern look.
    style.WindowRounding    = 14.0f;
    style.ChildRounding     = 10.0f;
    style.FrameRounding     = 8.0f;
    style.PopupRounding     = 8.0f;
    style.GrabRounding      = 8.0f;
    style.ScrollbarRounding = 10.0f;
    style.WindowBorderSize  = 0.0f;
    style.FrameBorderSize   = 0.0f;
    style.WindowPadding     = ImVec2(0.0f, 0.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.ItemSpacing       = ImVec2(8.0f, 6.0f);
    style.AntiAliasedFill   = true;
    style.AntiAliasedLines  = true;
    style.AntiAliasedLinesUseTex = true;
}
