// port/ui/UIWidgets.cpp — implementation of the CVar-bound ImGui widget library.
//
// Ported from HarbourMasters/Lighthouse src/port/UI/UIWidgets.cpp (branch `develop`), CC0 1.0.
// See port/ui/UIWidgets.hpp for provenance and the full list of API adaptations made for
// G-Diffuser's libultraship fork and ImGui 1.91.9b.

// Must precede every include: this TU uses the ImVec2/ImVec4 math operators, which imgui.h only
// emits when this macro is set before it is first included, and imgui_internal.h hard-#errors if
// the macro appears after imgui.h was already processed
// (build/x64/_deps/imgui-src/imgui_internal.h:112-113).
#define IMGUI_DEFINE_MATH_OPERATORS

#include "UIWidgets.hpp"

#include <imgui.h>
#include <imgui_internal.h> // the custom Checkbox, RadioButton and StateButton re-implement ImGui's
                            // own widgets with extra label placement, so they need ItemAdd/ItemSize/
                            // ButtonBehavior/RenderFrame/RenderCheckMark/RenderNavCursor/GImGui.

#include <cmath>
#include <cstdlib> // strtol (CVarInputInt default parsing)
#include <random>
#include <string>

#include "ship/Context.h"            // Ship::Context::GetInstance()
#include "ship/window/Window.h"      // Ship::Window::GetGui()
#include "ship/window/gui/Gui.h"     // Ship::Gui::SaveConsoleVariablesNextFrame()
#include "ship/window/gui/GuiWindow.h" // Ship::GuiWindow::ToggleVisibility() (WindowButton)
#include "ship/window/gui/IconsFontAwesome4.h" // ICON_FA_WINDOW_CLOSE / ICON_FA_EXTERNAL_LINK_SQUARE

namespace UIWidgets {

// Lighthouse spelled this inline at every CVar write site through GetRawInstance(), which this fork
// does not have. The Window/Gui are also not guaranteed to be constructed when a widget is
// exercised from a tool or a test, so the whole chain is null-checked.
void SaveCVars() {
    auto context = Ship::Context::GetInstance();
    if (context == nullptr) {
        return;
    }
    auto window = context->GetWindow();
    if (window == nullptr) {
        return;
    }
    auto gui = window->GetGui();
    if (gui == nullptr) {
        return;
    }
    gui->SaveConsoleVariablesNextFrame();
}

// Automatically adds newlines to break up text longer than a specified number of characters
// Manually included newlines will still be respected and reset the line length
// If line is midword when it hits the limit, text should break at the last encountered space
std::string WrappedText(const char* text, unsigned int charactersPerLine) {
    std::string newText(text);
    const size_t tipLength = newText.length();
    int lastSpace = -1;
    int currentLineLength = 0;
    for (unsigned int currentCharacter = 0; currentCharacter < tipLength; currentCharacter++) {
        if (newText[currentCharacter] == '\n') {
            currentLineLength = 0;
            lastSpace = -1;
            continue;
        } else if (newText[currentCharacter] == ' ') {
            lastSpace = currentCharacter;
        }

        if ((currentLineLength >= (int)charactersPerLine) && (lastSpace >= 0)) {
            newText[lastSpace] = '\n';
            currentLineLength = currentCharacter - lastSpace - 1;
            lastSpace = -1;
        }
        currentLineLength++;
    }

    return newText;
}

std::string WrappedText(const std::string& text, unsigned int charactersPerLine) {
    return WrappedText(text.c_str(), charactersPerLine);
}

void PaddedSeparator(bool padTop, bool padBottom, float extraVerticalTopPadding, float extraVerticalBottomPadding) {
    if (padTop) {
        Spacer(extraVerticalTopPadding);
    }
    ImGui::Separator();
    if (padBottom) {
        Spacer(extraVerticalBottomPadding);
    }
}

void Tooltip(const char* text) {
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", WrappedText(text).c_str());
    }
}

void PushStyleMenu(const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(color.x, color.y, color.z, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ColorValues.at(Colors::DarkGray));
    ImGui::PushStyleColor(ImGuiCol_Border, ColorValues.at(Colors::DarkGray));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 15.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 3.0f);
}

void PushStyleMenu(Colors color) {
    PushStyleMenu(ColorValues.at(color));
}

void PopStyleMenu() {
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

bool BeginMenu(const char* label, Colors color) {
    bool dirty = false;
    PushStyleMenu(color);
    ImGui::SetNextWindowSizeConstraints(ImVec2(200.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginMenu(label)) {
        dirty = true;
    }
    PopStyleMenu();
    return dirty;
}

void PushStyleMenuItem(const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, color);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20.0f, 15.0f));
}

void PushStyleMenuItem(Colors color) {
    PushStyleMenuItem(ColorValues.at(color));
}

void PopStyleMenuItem() {
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(1);
}

bool MenuItem(const char* label, const char* shortcut, Colors color) {
    bool dirty = false;
    PushStyleMenuItem(color);
    if (ImGui::MenuItem(label, shortcut)) {
        dirty = true;
    }
    PopStyleMenuItem();
    return dirty;
}

void PushStyleButton(const ImVec4& color, const ImVec2 padding) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 5.0f);
}

void PushStyleButton(Colors color, ImVec2 padding) {
    PushStyleButton(ColorValues.at(color), padding);
}

void PopStyleButton() {
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
}

void PushStyleInput(const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(color.x, color.y, color.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 5.0f);
}

void PushStyleInput(Colors color) {
    PushStyleInput(ColorValues.at(color));
}

void PopStyleInput() {
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(7);
}

void PushStyleHeader(const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(color.x, color.y, color.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(color.x, color.y, color.z, 0.6f));
}

void PushStyleHeader(Colors color) {
    PushStyleHeader(ColorValues.at(color));
}

void PopStyleHeader() {
    ImGui::PopStyleColor(3);
}

bool Button(const char* label, const ButtonOptions& options) {
    ImGui::BeginDisabled(options.disabled);
    PushStyleButton(options.color, options.padding);
    bool dirty = ImGui::Button(label, options.size);
    PopStyleButton();
    ImGui::EndDisabled();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.disabledTooltip).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }
    return dirty;
}

bool WindowButton(const char* label, const char* cvarName, std::shared_ptr<Ship::GuiWindow> windowPtr,
                  const WindowButtonOptions& options) {
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0));
    std::string buttonText = label;
    bool dirty = false;
    // The icon reflects what the click will do: a close glyph when the window is already up,
    // an "open in new window" glyph otherwise.
    if (CVarGetInteger(cvarName, 0)) {
        buttonText = ICON_FA_WINDOW_CLOSE " " + buttonText;
    } else {
        buttonText = ICON_FA_EXTERNAL_LINK_SQUARE " " + buttonText;
    }
    if (Button(buttonText.c_str(), { { options.tooltip, options.disabled, options.disabledTooltip },
                                     options.size,
                                     options.padding,
                                     options.color })) {
        // Not a bare CVarSetInteger on the visibility CVar: an already-constructed GuiWindow reads
        // that CVar only at construction and consults its in-memory mIsVisible every frame after,
        // so writing the CVar alone is a no-op. ToggleVisibility() flips mIsVisible AND mirrors +
        // persists the CVar (libultraship GuiElement.cpp:40 / GuiWindow.cpp:61).
        if (windowPtr != nullptr) {
            windowPtr->ToggleVisibility();
            dirty = true;
        }
    }
    ImGui::PopStyleVar();
    return dirty;
}

void PushStyleCheckbox(const ImVec4& color, ImVec2 padding) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(color.x, color.y, color.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 1.0f, 1.0f, 0.7f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 5.0f);
}

void PushStyleCheckbox(Colors color, ImVec2 padding) {
    PushStyleCheckbox(ColorValues.at(color), padding);
}

void PopStyleCheckbox() {
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}

void Spacer(float height) {
    ImGui::Dummy(ImVec2(0.0f, height));
}

void Separator(bool padTop, bool padBottom, float extraVerticalTopPadding, float extraVerticalBottomPadding) {
    if (padTop) {
        Spacer(extraVerticalTopPadding);
    }
    ImGui::Separator();
    if (padBottom) {
        Spacer(extraVerticalBottomPadding);
    }
}

// Adds a "?" next to the previous ImGui item with a custom tooltip
void InsertHelpHoverText(const std::string& text) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "?");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", WrappedText(text, 60).c_str());
        ImGui::EndTooltip();
    }
}

void InsertHelpHoverText(const char* text) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "?");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", WrappedText(text, 60).c_str());
        ImGui::EndTooltip();
    }
}

// Draws label text at an arbitrary position rather than at the cursor. Needed because the custom
// Checkbox/RadioButton below place the label above / left / right of the box, which ImGui's own
// RenderText does not support.
void RenderText(ImVec2 pos, const char* text, const char* text_end, bool hide_text_after_hash) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;

    // Hide anything after a '##' string
    const char* text_display_end;
    if (hide_text_after_hash) {
        text_display_end = ImGui::FindRenderedTextEnd(text, text_end);
    } else {
        if (!text_end)
            text_end = text + strlen(text); // FIXME-OPT
        text_display_end = text_end;
    }

    if (text != text_display_end) {
        window->DrawList->AddText(g.Font, g.FontSize, pos, ImGui::GetColorU32(ImGuiCol_Text), text, text_display_end);
        if (g.LogEnabled)
            ImGui::LogRenderedText(&pos, text, text_display_end);
    }
}

// A re-implementation of ImGui::Checkbox whose hit box spans label + box, so the label is
// clickable, and which honours CheckboxOptions' label placement (Near/Far/Above/None) and
// left/right component alignment.
bool Checkbox(const char* _label, bool* value, const CheckboxOptions& options) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGui::BeginDisabled(options.disabled);

    bool above = options.labelPosition == LabelPositions::Above;
    bool lpFar = options.labelPosition == LabelPositions::Far;
    bool right = options.alignment == ComponentAlignments::Right;
    bool none = options.labelPosition == LabelPositions::None;

    // Prefixing "##" makes ImGui hash the text for the ID but render nothing, which is how
    // LabelPositions::None hides the label without losing a stable widget ID.
    std::string labelStr = (none ? "##" : "");
    labelStr.append(_label);

    const char* label = labelStr.c_str();

    PushStyleCheckbox(options.color, options.padding);
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);
    const float square_sz = ImGui::GetFrameHeight();
    ImVec2 pos = window->DC.CursorPos;

    if (right) {
        float labelOffsetX = (above ? 0 : (style.ItemInnerSpacing.x * 2.0f) + square_sz);
        if (!lpFar) {
            pos.x += ImGui::GetContentRegionAvail().x - (label_size.x + labelOffsetX);
        }
    }
    float bbAboveX = lpFar ? ImGui::GetContentRegionAvail().x
                           : (label_size.x + (above ? 0 : (style.ItemInnerSpacing.x * 2.0f) + square_sz));
    float bbAboveY = label_size.y + (above ? square_sz : 0) + (style.FramePadding.y * 2.0f);
    const ImRect total_bb(pos, pos + ImVec2(bbAboveX, bbAboveY));

    ImGui::ItemSize(total_bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(total_bb, id)) {
        PopStyleCheckbox();
        ImGui::EndDisabled();
        return false;
    }
    bool hovered, held, pressed;
    pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);
    if (pressed) {
        *value = !(*value);
        ImGui::MarkItemEdited(id);
    }
    ImVec2 checkPos = pos;
    ImVec2 labelPos = pos;
    if (options.labelPosition == LabelPositions::Above) {
        checkPos.y += label_size.y + (style.ItemInnerSpacing.y * 2.0f);
    } else {
        // Center with checkbox automatically
        labelPos.y += ImGui::GetStyle().FramePadding.y;
    }
    if (options.alignment == ComponentAlignments::Right) {
        checkPos.x = total_bb.Max.x - square_sz;
    } else {
        float labelFarOffset = ImGui::GetContentRegionAvail().x - label_size.x;
        float labelOffsetX = above ? 0 : (lpFar ? labelFarOffset : (style.ItemInnerSpacing.x * 2.0f) + square_sz);
        labelPos.x += labelOffsetX;
    }
    const ImRect check_bb(checkPos, checkPos + ImVec2(square_sz, square_sz));
    // Renamed from RenderNavHighlight in ImGui 1.91.4; we are on 1.91.9b.
    ImGui::RenderNavCursor(total_bb, id);
    ImGui::RenderFrame(check_bb.Min, check_bb.Max,
                       ImGui::GetColorU32((held && hovered) ? ImGuiCol_FrameBgActive
                                          : hovered         ? ImGuiCol_FrameBgHovered
                                                            : ImGuiCol_FrameBg),
                       true, style.FrameRounding);
    ImU32 check_col = ImGui::GetColorU32(ImGuiCol_CheckMark);
    bool mixed_value = (g.LastItemData.ItemFlags & ImGuiItemFlags_MixedValue) != 0;
    if (mixed_value) {
        // Undocumented tristate/mixed/indeterminate checkbox (#2644)
        // This may seem awkwardly designed because the aim is to make ImGuiItemFlags_MixedValue supported by all
        // widgets (not just checkbox)
        ImVec2 pad(ImMax(1.0f, IM_TRUNC(square_sz / 3.6f)), ImMax(1.0f, IM_TRUNC(square_sz / 3.6f)));
        window->DrawList->AddRectFilled(check_bb.Min + pad, check_bb.Max - pad, check_col, style.FrameRounding);
    } else if (*value) {
        const float pad = ImMax(1.0f, IM_TRUNC(square_sz / 6.0f));
        ImGui::RenderCheckMark(window->DrawList, check_bb.Min + ImVec2(pad, pad), check_col, square_sz - pad * 2.0f);
    }
    RenderText(labelPos, label, ImGui::FindRenderedTextEnd(label), true);
    PopStyleCheckbox();
    ImGui::EndDisabled();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.disabledTooltip).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }
    return pressed;
}

bool CVarCheckbox(const char* label, const char* cvarName, const CheckboxOptions& options) {
    bool dirty = false;
    bool value = (bool)CVarGetInteger(cvarName, options.defaultValue);
    if (Checkbox(label, &value, options)) {
        CVarSetInteger(cvarName, value);
        SaveCVars();
        dirty = true;
    }
    return dirty;
}

// Button with an explicit ID separate from its label, so the caller can change the label every
// frame (a play/pause toggle, a "Copied!" confirmation) without ImGui losing the widget's state.
bool StateButton(const char* str_id, const char* label, ImVec2 size, ButtonOptions options, ImGuiButtonFlags flags) {

    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) {
        return false;
    }

    const ImGuiStyle& style = g.Style;
    const ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);

    const ImGuiID id = window->GetID(str_id);
    const ImRect bb(window->DC.CursorPos, window->DC.CursorPos + size);
    const float default_size = ImGui::GetFrameHeight();
    ImGui::ItemSize(size, (size.y >= default_size) ? g.Style.FramePadding.y : -1.0f);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    if (g.LastItemData.ItemFlags & ImGuiItemFlags_ButtonRepeat) {
        ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    }

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, flags);

    if (g.LastItemData.ItemFlags & ImGuiItemFlags_ButtonRepeat) {
        ImGui::PopItemFlag(); // ImGuiItemFlags_ButtonRepeat;
    }
    PushStyleButton(options.color);
    const ImU32 bg_col = ImGui::GetColorU32((held && hovered) ? ImGuiCol_ButtonActive
                                            : hovered         ? ImGuiCol_ButtonHovered
                                                              : ImGuiCol_Button);
    ImGui::RenderNavCursor(bb, id);
    ImGui::RenderFrame(bb.Min, bb.Max, bg_col, true, g.Style.FrameRounding);
    ImGui::RenderTextClipped(bb.Min + (style.FramePadding * 0.35f), bb.Max - (style.FramePadding / 4), label, NULL,
                             &label_size, style.ButtonTextAlign, &bb);
    PopStyleButton();

    IMGUI_TEST_ENGINE_ITEM_INFO(id, str_id, g.LastItemData.StatusFlags);
    return pressed;
}

// Mirror of the width maths inside ImGui::BeginCombo, extracted so a combo can be pre-sized to its
// widest entry (see the Combobox templates in the header) instead of jittering with the selection.
float CalcComboWidth(const char* preview_value, ImGuiComboFlags flags) {
    ImGuiContext& g = *GImGui;

    const ImGuiStyle& style = g.Style;
    IM_ASSERT((flags & (ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_NoPreview)) !=
              (ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_NoPreview)); // Can't use both flags together
    if (flags & ImGuiComboFlags_WidthFitPreview)
        IM_ASSERT((flags & (ImGuiComboFlags_NoPreview | (ImGuiComboFlags)ImGuiComboFlags_CustomPreview)) == 0);

    const float arrow_size = (flags & ImGuiComboFlags_NoArrowButton) ? 0.0f : ImGui::GetFrameHeight();
    const float preview_width = ImGui::CalcTextSize(preview_value, NULL, true).x;
    float w = arrow_size + preview_width + (style.FramePadding.x * 2.0f);
    return w;
}

void PushStyleCombobox(const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(color.x, color.y, color.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(color.x, color.y, color.z, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
}

void PushStyleCombobox(Colors color) {
    PushStyleCombobox(ColorValues.at(color));
}

void PopStyleCombobox() {
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(9);
}

void PushStyleTabs(const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(color.x, color.y, color.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(color.x, color.y, color.z, 0.6f));
    // ImGuiCol_TabActive was renamed to ImGuiCol_TabSelected in ImGui 1.90.9; the old spelling now
    // only survives behind IMGUI_DISABLE_OBSOLETE_FUNCTIONS being off.
    ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(color.x, color.y, color.z, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(color.x, color.y, color.z, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
}

void PushStyleTabs(Colors color) {
    PushStyleTabs(ColorValues.at(color));
}

void PopStyleTabs() {
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(4);
}

void PushStyleSlider(Colors color_) {
    const ImVec4& color = ColorValues.at(color_);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(color.x, color.y, color.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(1.0, 1.0, 1.0, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.0, 1.0, 1.0, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
}

void PopStyleSlider() {
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(6);
}

bool SliderInt(const char* label, int32_t* value, const IntSliderOptions& options) {
    bool dirty = false;
    std::string invisibleLabelStr = "##" + std::string(label);
    const char* invisibleLabel = invisibleLabelStr.c_str();
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleSlider(options.color);
    float width = (options.size == ImVec2(0, 0)) ? ImGui::GetContentRegionAvail().x : options.size.x;
    if (options.labelPosition == LabelPositions::Near || options.labelPosition == LabelPositions::Far) {
        width = width - (ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x);
    }
    ImGui::AlignTextToFramePadding();
    if (options.alignment == ComponentAlignments::Right && options.labelPosition != LabelPositions::None) {
        ImGui::Text("%s", label);
        if (options.labelPosition == LabelPositions::Above) {
            ImGui::NewLine();
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
        } else if (options.labelPosition == LabelPositions::Near) {
            ImGui::SameLine();
        } else if (options.labelPosition == LabelPositions::Far) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
        }
    } else if (options.alignment == ComponentAlignments::Left) {
        if (options.labelPosition == LabelPositions::Above) {
            ImGui::Text("%s", label);
        }
    }
    if (options.showButtons) {
        if (Button("-", ButtonOptions{ .color = options.color }.Size(Sizes::Inline)) && *value > options.min) {
            *value -= options.step;
            if (options.clamp) {
                if (*value < options.min) {
                    *value = options.min;
                }
            }
            dirty = true;
        }
        ImGui::SameLine(0, 3.0f);
        ImGui::SetNextItemWidth(width - (ImGui::CalcTextSize("+").x + ImGui::GetStyle().FramePadding.x * 2 + 3) * 2);
    } else {
        ImGui::SetNextItemWidth(width);
    }
    if (ImGui::SliderScalar(invisibleLabel, ImGuiDataType_S32, value, &options.min, &options.max, options.format,
                            options.flags)) {
        if (options.clamp) {
            if (*value < options.min) {
                *value = options.min;
            }
            if (*value > options.max)
                *value = options.max;
        }
        dirty = true;
    }
    if (options.showButtons) {
        ImGui::SameLine(0, 3.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (Button("+", ButtonOptions{ .color = options.color }.Size(Sizes::Inline)) && *value < options.max) {
            *value += options.step;
            if (options.clamp) {
                if (*value > options.max)
                    *value = options.max;
            }
            dirty = true;
        }
    }

    if (options.alignment == ComponentAlignments::Left && options.labelPosition != LabelPositions::None) {
        if (options.labelPosition == LabelPositions::Near) {
            ImGui::SameLine();
            ImGui::Text("%s", label);
        } else if (options.labelPosition == LabelPositions::Far) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(label).x +
                            ImGui::GetStyle().ItemSpacing.x);
            ImGui::Text("%s", label);
        }
    }
    PopStyleSlider();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.disabledTooltip).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }
    ImGui::PopID();
    return dirty;
}

bool CVarSliderInt(const char* label, const char* cvarName, const IntSliderOptions& options) {
    bool dirty = false;
    int32_t value = CVarGetInteger(cvarName, options.defaultValue);
    if (SliderInt(label, &value, options)) {
        CVarSetInteger(cvarName, value);
        SaveCVars();
        dirty = true;
    }
    return dirty;
}

// Clamps into [min, max] and snaps to the decimal precision implied by `step`, so repeatedly
// nudging a 0.01-step slider does not accumulate binary float drift into 0.30000001.
void ClampFloat(float* value, float min, float max, float step) {
    int factor = 1;
    if (step < 1.0f) {
        factor *= 10;
    }
    if (step < 0.1f) {
        factor *= 10;
    }
    if (step < 0.01f) {
        factor *= 10;
    }
    if (step < 0.001f) {
        factor *= 10;
    }
    if (step < 0.0001f) {
        factor *= 10;
    }
    if (step < 0.00001f) {
        factor *= 10;
    }
    if (*value < min) {
        *value = min;
    } else if (*value > max) {
        *value = max;
    } else {
        int trunc = (int)std::round(*value * factor);
        *value = (float)trunc / factor;
    }
}

bool SliderFloat(const char* label, float* value, const FloatSliderOptions& options) {
    bool dirty = false;
    std::string invisibleLabelStr = "##" + std::string(label);
    const char* invisibleLabel = invisibleLabelStr.c_str();
    // isPercentage keeps the CVar in 0..1 but shows the user 0..100; only the display copy is
    // scaled, so the stored value never picks up rounding from the x100 round trip.
    float valueToDisplay = options.isPercentage ? *value * 100.0f : *value;
    float maxToDisplay = options.isPercentage ? options.max * 100.0f : options.max;
    float minToDisplay = options.isPercentage ? options.min * 100.0f : options.min;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleSlider(options.color);
    float labelSpacing = ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemSpacing.x;
    float width = (options.size == ImVec2(0, 0)) ? ImGui::GetContentRegionAvail().x : options.size.x;
    if (options.labelPosition == LabelPositions::Near || options.labelPosition == LabelPositions::Far) {
        width = width - (ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x);
    }
    ImGui::AlignTextToFramePadding();
    if (options.alignment == ComponentAlignments::Right) {
        ImGui::Text("%s", label);
        if (options.labelPosition == LabelPositions::Above) {
            ImGui::NewLine();
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
        } else if (options.labelPosition == LabelPositions::Near) {
            width -= labelSpacing;
            ImGui::SameLine();
        } else if (options.labelPosition == LabelPositions::Far || options.labelPosition == LabelPositions::None) {
            width -= labelSpacing;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
        }
    } else if (options.alignment == ComponentAlignments::Left) {
        if (options.labelPosition == LabelPositions::Above) {
            ImGui::Text("%s", label);
        }
    }
    if (options.showButtons) {
        if (Button("-", ButtonOptions{ .color = options.color }.Size(Sizes::Inline)) && *value > options.min) {
            *value -= options.step;
            if (options.clamp) {
                ClampFloat(value, options.min, options.max, options.step);
            }
            dirty = true;
        }
        ImGui::SameLine(0, 3.0f);
        ImGui::SetNextItemWidth(width - (ImGui::CalcTextSize("+").x + ImGui::GetStyle().FramePadding.x * 2 + 3) * 2);
    } else {
        ImGui::SetNextItemWidth(width);
    }
    if (ImGui::SliderScalar(invisibleLabel, ImGuiDataType_Float, &valueToDisplay, &minToDisplay, &maxToDisplay,
                            options.format, options.flags)) {
        *value = options.isPercentage ? valueToDisplay / 100.0f : valueToDisplay;
        if (options.clamp) {
            ClampFloat(value, options.min, options.max, options.step);
        }
        dirty = true;
    }
    if (options.showButtons) {
        ImGui::SameLine(0, 3.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (Button("+", ButtonOptions{ .color = options.color }.Size(Sizes::Inline)) && *value < options.max) {
            *value += options.step;
            if (options.clamp) {
                ClampFloat(value, options.min, options.max, options.step);
            }
            dirty = true;
        }
    }

    if (options.alignment == ComponentAlignments::Left) {
        if (options.labelPosition == LabelPositions::Near) {
            ImGui::SameLine();
            ImGui::Text("%s", label);
        } else if (options.labelPosition == LabelPositions::Far || options.labelPosition == LabelPositions::None) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - labelSpacing);
            ImGui::Text("%s", label);
        }
    }
    PopStyleSlider();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.disabledTooltip).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }
    ImGui::PopID();
    return dirty;
}

bool CVarSliderFloat(const char* label, const char* cvarName, const FloatSliderOptions& options) {
    bool dirty = false;
    float value = CVarGetFloat(cvarName, options.defaultValue);
    if (SliderFloat(label, &value, options)) {
        CVarSetFloat(cvarName, value);
        SaveCVars();
        dirty = true;
    }
    return dirty;
}

// Lets ImGui grow the std::string in place: it hands back the new length, we resize and re-point
// its buffer. Same idiom as ImGui's own misc/cpp/imgui_stdlib.cpp helper.
int InputTextResizeCallback(ImGuiInputTextCallbackData* data) {
    std::string* value = (std::string*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        value->resize(data->BufTextLen);
        data->Buf = (char*)value->c_str();
    }
    return 0;
}

bool InputString(const char* label, std::string* value, const InputOptions& options) {
    bool dirty = false;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleInput(options.color);
    if (options.hasError) {
        ImGui::PushStyleColor(ImGuiCol_Border, ColorValues.at(Colors::Red));
    }
    float width = (options.size == ImVec2(0, 0)) ? ImGui::GetContentRegionAvail().x : options.size.x;
    ImVec2 labelSize = ImGui::CalcTextSize(label, NULL, true);
    if (labelSize.x != 0) {
        if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::Text("%s", label);
            }
        } else if (options.alignment == ComponentAlignments::Right) {
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::NewLine();
                ImGui::SameLine(width - ImGui::CalcTextSize(label).x);
                ImGui::Text("%s", label);
            }
        }
    }
    ImGui::SetNextItemWidth(width);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
    if (options.secret) {
        flags |= ImGuiInputTextFlags_Password;
    }
    flags |= options.addedFlags;
    if (ImGui::InputText(label, (char*)value->c_str(), value->capacity() + 1, flags, InputTextResizeCallback, value)) {
        dirty = true;
    }
    if (value->empty() && !options.placeholder.empty()) {
        ImGui::SameLine(17.0f);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.4f), "%s", options.placeholder.c_str());
    }
    if (options.hasError) {
        ImGui::PopStyleColor();
    }
    PopStyleInput();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.hasError && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !IsCStringEmpty(options.errorText)) {
        ImGui::SetTooltip("%s", WrappedText(options.errorText).c_str());
    } else if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
               !IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.disabledTooltip).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }
    ImGui::PopID();
    return dirty;
}

bool CVarInputString(const char* label, const char* cvarName, const InputOptions& options) {
    bool dirty = false;
    std::string value = CVarGetString(cvarName, options.defaultValue.c_str());
    if (InputString(label, &value, options)) {
        CVarSetString(cvarName, value.c_str());
        SaveCVars();
        dirty = true;
    }
    return dirty;
}

bool InputInt(const char* label, int32_t* value, const InputOptions& options) {
    bool dirty = false;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleInput(options.color);
    float width = (options.size == ImVec2(0, 0)) ? ImGui::GetContentRegionAvail().x : options.size.x;
    if (options.alignment == ComponentAlignments::Left) {
        if (options.labelPosition == LabelPositions::Above) {
            ImGui::Text("%s", label);
        }
    } else if (options.alignment == ComponentAlignments::Right) {
        if (options.labelPosition == LabelPositions::Above) {
            ImGui::NewLine();
            ImGui::SameLine(width - ImGui::CalcTextSize(label).x);
            ImGui::Text("%s", label);
        }
    }
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputScalar(label, ImGuiDataType_S32, value, nullptr, nullptr, nullptr, options.addedFlags)) {
        dirty = true;
    }
    if ((ImGui::GetItemStatusFlags() & ImGuiItemStatusFlags_Edited) && !options.placeholder.empty()) {
        ImGui::SameLine(17.0f);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.4f), "%s", options.placeholder.c_str());
    }
    PopStyleInput();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.disabledTooltip).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }
    ImGui::PopID();
    return dirty;
}

bool CVarInputInt(const char* label, const char* cvarName, const InputOptions& options) {
    bool dirty = false;
    // FIX: upstream did `std::stoi(options.defaultValue)` unguarded. InputOptions::defaultValue is
    // a std::string defaulting to "", so std::stoi throws std::invalid_argument for every caller
    // that does not explicitly set a numeric default — i.e. the common case. strtol cannot throw
    // and yields 0 for unparseable input, which is the intended fallback.
    int32_t defaultValue = 0;
    if (!options.defaultValue.empty()) {
        defaultValue = static_cast<int32_t>(std::strtol(options.defaultValue.c_str(), nullptr, 10));
    }
    int32_t value = CVarGetInteger(cvarName, defaultValue);
    if (InputInt(label, &value, options)) {
        CVarSetInteger(cvarName, value);
        SaveCVars();
        dirty = true;
    }
    return dirty;
}

bool CVarColorPicker(const char* label, const char* cvarName, Color_RGBA8 defaultColor, bool hasAlpha,
                     uint8_t modifiers, Colors themeColor) {
    // One logical colour option is three CVars: the colour itself plus two independent booleans.
    // Keeping them as siblings under a shared prefix is what lets the Reset button wipe the whole
    // group with a single CVarClearBlock.
    std::string valueCVar = std::string(cvarName) + ".Value";
    std::string rainbowCVar = std::string(cvarName) + ".Rainbow";
    std::string lockedCVar = std::string(cvarName) + ".Locked";
    Color_RGBA8 color = CVarGetColor(valueCVar.c_str(), defaultColor);
    ImVec4 colorVec = ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
    bool changed = false;
    bool showReset = (modifiers & ColorPickerResetButton) != 0;
    bool showRandom = (modifiers & ColorPickerRandomButton) != 0;
    bool showRainbow = (modifiers & ColorPickerRainbowCheck) != 0;
    bool showLock = (modifiers & ColorPickerLockCheck) != 0;
    bool locked = CVarGetInteger(lockedCVar.c_str(), 0);
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs;
    // The lock checkbox sits OUTSIDE this disabled scope on purpose — it has to stay clickable to
    // be un-lockable.
    ImGui::BeginDisabled(locked);
    PushStyleCombobox(Colors::DarkGray);
    if (hasAlpha) {
        // ImGuiColorEditFlags_AlphaPreview was dropped here: ImGui 1.91.8 removed it and made the
        // checkerboard preview the default (imgui.h:1869 keeps it only as a `= 0` obsolete stub).
        changed = ImGui::ColorEdit4(label, (float*)&colorVec, flags | ImGuiColorEditFlags_AlphaBar);
    } else {
        changed = ImGui::ColorEdit3(label, (float*)&colorVec, flags | ImGuiColorEditFlags_NoAlpha);
    }
    PopStyleCombobox();
    ImGui::AlignTextToFramePadding();
    if (showReset) {
        ImGui::SameLine();
        std::string uniqueTag = "Reset##" + std::string(label);
        if (Button(uniqueTag.c_str(), ButtonOptions({ { .tooltip = "Resets this color to its default value" } })
                                          .Color(themeColor)
                                          .Size(Sizes::Inline))) {
            // Upstream also cleared "<cvar>.R/.G/.B/.A/.Type" here, a shim for SoH configs written
            // before the packed-colour format. G-Diffuser has no such legacy layout.
            CVarClearBlock(valueCVar.c_str());
            SaveCVars();
        }
    }
    if (showRandom) {
        ImGui::SameLine();
        std::string uniqueTag = "Random##" + std::string(label);
        if (Button(uniqueTag.c_str(), ButtonOptions({ { .tooltip = "Generates a random color value to use" } })
                                          .Color(themeColor)
                                          .Size(Sizes::Inline))) {
            colorVec = GetRandomValue();
            color.r = static_cast<uint8_t>(fmin(fmax(colorVec.x * 255, 0), 255));
            color.g = static_cast<uint8_t>(fmin(fmax(colorVec.y * 255, 0), 255));
            color.b = static_cast<uint8_t>(fmin(fmax(colorVec.z * 255, 0), 255));
            CVarSetColor(valueCVar.c_str(), color);
            CVarSetInteger(rainbowCVar.c_str(), 0); // On click disable rainbow mode.
            SaveCVars();
        }
    }
    if (showRainbow) {
        ImGui::SameLine();
        std::string uniqueTag = "Rainbow##" + std::string(cvarName) + "Rainbow";

        CVarCheckbox(uniqueTag.c_str(), rainbowCVar.c_str(),
                     CheckboxOptions({ { .tooltip = "Cycles through colors on a timer\n"
                                                    "Overwrites previously chosen color" } })
                         .Color(themeColor));
    }
    ImGui::EndDisabled();
    if (showLock) {
        ImGui::SameLine();
        std::string uniqueTag = "Lock##" + std::string(cvarName) + "Locked";

        CVarCheckbox(uniqueTag.c_str(), lockedCVar.c_str(),
                     CheckboxOptions({ { .tooltip = "Prevents this color from being changed" } }).Color(themeColor));
    }
    if (changed) {
        color.r = (uint8_t)(colorVec.x * 255.0f);
        color.g = (uint8_t)(colorVec.y * 255.0f);
        color.b = (uint8_t)(colorVec.z * 255.0f);
        color.a = (uint8_t)(colorVec.w * 255.0f);
        CVarSetColor(valueCVar.c_str(), color);
        SaveCVars();
        changed = true;
    }

    return changed;
}

bool CVarColorPicker(const char* label, const char* cvarName, const ColorPickerOptions& options) {
    uint8_t modifiers = 0;
    if (options.showReset) {
        modifiers |= ColorPickerResetButton;
    }
    if (options.showRandom) {
        modifiers |= ColorPickerRandomButton;
    }
    if (options.showRainbow) {
        modifiers |= ColorPickerRainbowCheck;
    }
    if (options.showLock) {
        modifiers |= ColorPickerLockCheck;
    }
    return CVarColorPicker(label, cvarName, options.defaultValue, options.useAlpha, modifiers, options.color);
}

// `options` is accepted for symmetry with CVarRadioButton but deliberately unread: honouring
// options.color would need a Push/PopStyleCheckbox pair that the early-return paths below would
// leave unbalanced. Upstream drew this one with the ambient style too.
bool RadioButton(const char* label, bool active, const RadioButtonsOptions& options) {
    (void)options;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);

    const float square_sz = ImGui::GetFrameHeight();
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect check_bb(pos, pos + ImVec2(square_sz, square_sz));
    const ImRect total_bb(
        pos, pos + ImVec2(square_sz + (label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f),
                          label_size.y + style.FramePadding.y * 2.0f));
    ImGui::ItemSize(total_bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(total_bb, id))
        return false;

    ImVec2 center = check_bb.GetCenter();
    center.x = IM_ROUND(center.x);
    center.y = IM_ROUND(center.y);
    const float radius = (square_sz - 1.0f) * 0.5f;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);
    if (pressed)
        ImGui::MarkItemEdited(id);

    ImGui::RenderNavCursor(total_bb, id);
    const int num_segment = window->DrawList->_CalcCircleAutoSegmentCount(radius);
    window->DrawList->AddCircleFilled(center, radius,
                                      ImGui::GetColorU32((held && hovered) ? ImGuiCol_FrameBgActive
                                                         : hovered         ? ImGuiCol_FrameBgHovered
                                                                           : ImGuiCol_FrameBg),
                                      num_segment);
    if (active) {
        const float pad = ImMax(1.0f, IM_TRUNC(square_sz / 6.0f));
        window->DrawList->AddCircleFilled(center, radius - pad, ImGui::GetColorU32(ImGuiCol_CheckMark));
    }

    if (style.FrameBorderSize > 0.0f) {
        window->DrawList->AddCircle(center + ImVec2(1, 1), radius, ImGui::GetColorU32(ImGuiCol_BorderShadow),
                                    num_segment, style.FrameBorderSize);
        window->DrawList->AddCircle(center, radius, ImGui::GetColorU32(ImGuiCol_Border), num_segment,
                                    style.FrameBorderSize);
    }

    ImVec2 label_pos = ImVec2(check_bb.Max.x + style.ItemInnerSpacing.x, check_bb.Min.y + style.FramePadding.y);
    if (g.LogEnabled)
        ImGui::LogRenderedText(&label_pos, active ? "(x)" : "( )");
    if (label_size.x > 0.0f)
        RenderText(label_pos, label, ImGui::FindRenderedTextEnd(label), true);

    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);
    return pressed;
}

bool CVarRadioButton(const char* text, const char* cvarName, int32_t id, const RadioButtonsOptions& options) {
    // The ID is the label plus the CVar name so several radio groups can reuse the same visible
    // text without colliding in ImGui's ID stack; the visible text is then drawn separately.
    std::string make_invisible = "##" + std::string(text) + std::string(cvarName);

    bool ret = false;
    int val = CVarGetInteger(cvarName, options.defaultIndex);
    PushStyleCheckbox(options.color);
    if (ImGui::RadioButton(make_invisible.c_str(), id == val)) {
        CVarSetInteger(cvarName, id);
        SaveCVars();
        ret = true;
    }
    ImGui::SameLine();
    ImGui::Text("%s", text);
    PopStyleCheckbox();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(options.tooltip).c_str());
    }

    return ret;
}

// Exposes a bitfield as a row of checkboxes, eight per line. fmt::format is unavailable on the port
// target (ADAPTATION #4), hence the plain string concatenation for the per-bit ImGui IDs.
void DrawFlagArray32(const std::string& name, uint32_t& flags, Colors color) {
    ImGui::PushID(name.c_str());
    for (int32_t flagIndex = 0; flagIndex < 32; flagIndex++) {
        if ((flagIndex % 8) != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(flagIndex);
        uint32_t bitMask = 1 << flagIndex;
        bool flag = (flags & bitMask) != 0;
        PushStyleCheckbox(color);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 3.0f));
        std::string id = "##" + name + std::to_string(flagIndex);
        if (ImGui::Checkbox(id.c_str(), &flag)) {
            if (flag) {
                flags |= bitMask;
            } else {
                flags &= ~bitMask;
            }
        }
        ImGui::PopStyleVar();
        PopStyleCheckbox();
        ImGui::PopID();
    }
    ImGui::PopID();
}

void DrawFlagArray16(const std::string& name, uint16_t& flags, Colors color) {
    ImGui::PushID(name.c_str());
    for (int16_t flagIndex = 0; flagIndex < 16; flagIndex++) {
        if ((flagIndex % 8) != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(flagIndex);
        uint16_t bitMask = (uint16_t)(1 << flagIndex);
        bool flag = (flags & bitMask) != 0;
        PushStyleCheckbox(color);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 3.0f));
        std::string id = "##" + name + std::to_string(flagIndex);
        if (ImGui::Checkbox(id.c_str(), &flag)) {
            if (flag) {
                flags |= bitMask;
            } else {
                flags &= (uint16_t)~bitMask;
            }
        }
        ImGui::PopStyleVar();
        PopStyleCheckbox();
        ImGui::PopID();
    }
    ImGui::PopID();
}

void DrawFlagArray8(const std::string& name, uint8_t& flags, Colors color) {
    ImGui::PushID(name.c_str());
    for (int8_t flagIndex = 0; flagIndex < 8; flagIndex++) {
        if ((flagIndex % 8) != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(flagIndex);
        uint8_t bitMask = (uint8_t)(1 << flagIndex);
        bool flag = (flags & bitMask) != 0;
        PushStyleCheckbox(color);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 3.0f));
        std::string id = "##" + name + std::to_string(flagIndex);
        if (ImGui::Checkbox(id.c_str(), &flag)) {
            if (flag) {
                flags |= bitMask;
            } else {
                flags &= (uint8_t)~bitMask;
            }
        }
        ImGui::PopStyleVar();
        PopStyleCheckbox();
        ImGui::PopID();
    }
    ImGui::PopID();
}

ImVec4 GetRandomValue() {
#if !defined(__SWITCH__) && !defined(__WIIU__)
    std::random_device rd;
    std::mt19937 rng(rd());
#else
    // std::random_device is unusable on those consoles' libc, so fall back to a hashed rand().
    size_t seed = std::hash<std::string>{}(std::to_string(rand()));
    std::mt19937_64 rng(seed);
#endif
    std::uniform_int_distribution<int> dist(0, 255 - 1);

    ImVec4 NewColor;
    NewColor.x = (float)(dist(rng)) / 255.0f;
    NewColor.y = (float)(dist(rng)) / 255.0f;
    NewColor.z = (float)(dist(rng)) / 255.0f;
    NewColor.w = 1.0f;
    return NewColor;
}

Color_RGBA8 RGBA8FromVec(ImVec4 vec) {
    Color_RGBA8 color = { static_cast<uint8_t>(vec.x * 255), static_cast<uint8_t>(vec.y * 255),
                          static_cast<uint8_t>(vec.z * 255), static_cast<uint8_t>(vec.w * 255) };
    return color;
}

ImVec4 VecFromRGBA8(Color_RGBA8 color) {
    ImVec4 vec = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f };
    return vec;
}

} // namespace UIWidgets
