#include "editor/core/Clipboard.h"
#include "imgui.h"
namespace raven::editor {
void CopyTextToClipboard(const std::string& text) { ImGui::SetClipboardText(text.c_str()); }
} // namespace raven::editor
