// RE4DLSS 0.0.x is a frame capture probe (main.cpp): the upscaler comes once the game's FSR 2
// pass and its inputs are known (docs/FINDINGS.md). These do nothing yet.

#include "common.hpp"

namespace rd {

void UpscalerInit() {}
void UpscalerShutdown() {}
bool UpscalerOnDraw(reshade::api::command_list*, uint32_t, uint32_t) { return false; }
void UpscalerPresent(reshade::api::swapchain*) {}
void UpscalerOverlay() { ImGui::TextDisabled("Frame capture probe: changes nothing in the game."); }

}  // namespace rd
