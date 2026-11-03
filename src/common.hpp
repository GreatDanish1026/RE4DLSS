// What main.cpp (logging, shader ids, resource tracking) shares with the upscaler, DLSS, FSR and
// frame generation code.
#pragma once

#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include <reshade.hpp>

#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace rd {

void Log(const char* fmt, ...);
std::wstring ExeDir();  // with a trailing backslash

// Shader ids (FNV-1a of the bytecode) of a pipeline: pixel shader for graphics, compute shader.
uint32_t PipelinePs(reshade::api::pipeline pipe);
uint32_t PipelineCs(reshade::api::pipeline pipe);  // compute shader id, 0 for graphics

// The resource a view was created for, from the add-on's own map (never asks ReShade about a
// view whose resource may have been freed).
reshade::api::resource ViewResource(reshade::api::resource_view view);

// Frame generation (framegen.cpp, shared with the Unreal add-ons): inputs go to their frame when
// the game submits them, not when they're recorded.
constexpr bool kTagAtRecord = false;
constexpr bool kTsrMapped = true;  // the upscaling pass is known (framegen.cpp's name from the TSR add-ons)
// Unreal's HDR output copies the picture before the HUD out of the back buffer; RE4 has no such copy
// known yet, so frame generation runs without a pre-HUD picture (the HUD may wobble a little).
constexpr bool kHudFromBackBufferCopy = false;

// Overlay helpers (upscaler.cpp).
float ControlWidth();
void Help(const char* text);  // a "(?)" after the last item, with <text> as its tooltip
extern const ImVec4 kWarn;
// Puts the game's pipeline, root signatures, descriptor heaps and bindings back on <cl> after
// the add-on recorded work of its own on it.
void RestoreListState(reshade::api::command_list* cl);

// framegen.cpp: DLSS Frame Generation through Streamline, or AMD FSR frame generation.
void RegisterFrameGen();
void UnregisterFrameGen();
void DrawFrameGenSettings();
void DrawFrameGenStatus();
std::string FrameGenHeadline(bool* warn);
std::string FrameGenDiag();  // inputs tagged / frames without them since the last call, and why
// True while frame generation wants each frame's camera (the upscaler then looks for SceneInfo).
bool FrameGenWantsCamera();
// RE Engine's SceneInfo constant buffer (kSceneInfoBytes) of the frame's main view.
constexpr uint32_t kSceneInfoBytes = 528;
void FrameGenSetSceneInfo(const float* scene);
// This frame's depth (r32f, reversed Z) and motion vectors (UV offsets, previous minus current),
// both in D3D12 resource state <state>, tagged on <list>.
void FrameGenTag(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, unsigned state,
                 unsigned renderWidth, unsigned renderHeight, float jitterX, float jitterY, bool reset);

// upscaler.cpp
void UpscalerInit();
void UpscalerShutdown();
bool UpscalerOnDraw(reshade::api::command_list* cl, uint32_t vertices, uint32_t instances);  // true: draw replaced
bool UpscalerOnDispatch(reshade::api::command_list* cl);  // true: dispatch replaced
void UpscalerPresent(reshade::api::swapchain* sc);
void UpscalerOverlay();

}  // namespace rd
