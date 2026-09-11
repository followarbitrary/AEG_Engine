#include <imgui.h>

#include <cassert>

#ifndef IMGUI_HAS_DOCK
#error "AEG requires the docking-enabled Dear ImGui source"
#endif

int main() {
    ImGuiContext* context = ImGui::CreateContext();
    assert(context != nullptr);

    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* font_pixels = nullptr;
    int font_width = 0;
    int font_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
    assert(font_pixels != nullptr);
    assert(font_width > 0);
    assert(font_height > 0);

    ImGui::NewFrame();
    const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    assert(dockspace_id != 0);
    ImGui::Begin("Dockable Smoke");
    ImGui::TextUnformatted("AEG");
    ImGui::End();
    ImGui::Render();

    const ImDrawData* draw_data = ImGui::GetDrawData();
    assert(draw_data != nullptr);
    assert(draw_data->Valid);

    ImGui::DestroyContext(context);
    return 0;
}
