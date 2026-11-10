// paste_files.cpp - see paste_files.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/paste_files.hpp"

#include <cstdint>
#include <cstring>

#include <imgui.h>

#include "core/clipboard_files.hpp"

namespace cascade::gui {

namespace {

// The reader the backend installed, kept so the wrapper can ask it first.
const char* (*g_backendRead)(ImGuiContext*) = nullptr;
// What the wrapper hands back: Dear ImGui expects the pointer to stay valid
// until its next read, so the text lives here.
std::string g_pasteText;
std::string g_scripted;

const char* readForPaste(ImGuiContext* ctx) {
    if (!g_scripted.empty()) {
        g_pasteText = g_scripted;
        return g_pasteText.c_str();
    }
    if (g_backendRead != nullptr) {
        if (const char* text = g_backendRead(ctx); text != nullptr && *text != '\0') { return text; }
    }
    g_pasteText = cascade::core::firstFileOnClipboard();
    if (!g_pasteText.empty()) { return g_pasteText.c_str(); }
    return nullptr;
}

}  // namespace

std::string firstDroppedFile(const unsigned char* block, std::size_t bytes) {
    // Moved to core/clipboard_files.* with the Slint window's paste (docs 6.63); the classic's name stays.
    return cascade::core::firstDroppedFile(block, bytes);
}

void installFilePaste() {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.Platform_GetClipboardTextFn == readForPaste) { return; }  // already wrapped
    g_backendRead = pio.Platform_GetClipboardTextFn;
    pio.Platform_GetClipboardTextFn = readForPaste;
}

void setScriptedClipboard(const std::string& text) { g_scripted = text; }

const char* clipboardTextForPaste() {
    return readForPaste(ImGui::GetCurrentContext());
}

}  // namespace cascade::gui
