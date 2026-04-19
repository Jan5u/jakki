#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include <SDL3/SDL.h>
#include <imgui_internal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vulkan/vulkan_core.h>

int main(int argc, char *argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
    }

    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window *window = SDL_CreateWindow("Dear ImGui SDL3+SDL_GPU example", (int)(1280 * main_scale), (int)(720 * main_scale), window_flags);
    if (window == nullptr) {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    SDL_PropertiesID props = SDL_CreateProperties();

    SDL_GPUVulkanOptions vk_opts = {0};
    vk_opts.vulkan_api_version = VK_API_VERSION_1_3;

    const char *device_exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    vk_opts.device_extension_count = 2;
    vk_opts.device_extension_names = device_exts;

    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &vk_opts);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_METALLIB_BOOLEAN, true);

    SDL_GPUDevice *gpu_device = SDL_CreateGPUDeviceWithProperties(props);
    if (gpu_device == nullptr) {
        printf("Error: SDL_CreateGPUDevice(): %s\n", SDL_GetError());
    }

    if (!SDL_ClaimWindowForGPUDevice(gpu_device, window)) {
        printf("Error: SDL_ClaimWindowForGPUDevice(): %s\n", SDL_GetError());
    }
    SDL_SetGPUSwapchainParameters(gpu_device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = gpu_device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu_device, window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    ImGui_ImplSDLGPU3_Init(&init_info);

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    bool request_redraw = true;
    int selected_tab = 0;
    std::string message_buffer;

    bool done = false;
    while (!done) {
        SDL_Event event;
        const int wait_timeout_ms = request_redraw ? 0 : 250;
        const bool got_event = SDL_WaitEventTimeout(&event, wait_timeout_ms);

        if (got_event) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
                done = true;

            request_redraw = true;

            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT)
                    done = true;
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
                    done = true;
                request_redraw = true;
            }
        }

        if (!request_redraw)
            continue;

        if (done)
            break;

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            request_redraw = false;
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGuiID dockspace_id = ImGui::GetID("My Dockspace");
        ImGuiViewport *viewport = ImGui::GetMainViewport();

        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);
            ImGuiID dock_id_left = 0;
            ImGuiID dock_id_main = dockspace_id;
            ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Left, 0.20f, &dock_id_left, &dock_id_main);
            ImGuiID dock_id_left_top = 0;
            ImGuiID dock_id_left_bottom = 0;
            ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Up, 0.80f, &dock_id_left_top, &dock_id_left_bottom);
            ImGuiID dock_id_right = 0;
            ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Right, 0.20f, &dock_id_right, &dock_id_main);
            ImGui::DockBuilderDockWindow("Tabs", dock_id_main);
            ImGui::DockBuilderDockWindow("Channels", dock_id_left_top);
            ImGui::DockBuilderDockWindow("User", dock_id_left_bottom);
            ImGui::DockBuilderDockWindow("Users", dock_id_right);
            ImGui::DockBuilderFinish(dockspace_id);
        }

        ImGui::DockSpaceOverViewport(dockspace_id, viewport, ImGuiDockNodeFlags_PassthruCentralNode);

        ImGui::Begin("Tabs");
        if (ImGui::BeginTabBar("MainTabs")) {
            if (ImGui::BeginTabItem("General")) {
                selected_tab = 0;
                ImGui::TextUnformatted("General controls");
                if (ImGui::Button("Hover me")) {
                    message_buffer = "General: button clicked";
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Chat")) {
                selected_tab = 1;
                static bool notify_mentions = true;
                static float chat_opacity = 0.80f;
                ImGui::Checkbox("Notify mentions", &notify_mentions);
                ImGui::SliderFloat("Overlay opacity", &chat_opacity, 0.0f, 1.0f, "%.2f");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Debug")) {
                selected_tab = 2;
                ImGui::Text("FPS: %.1f", io.Framerate);
                ImGui::Text("Frame time: %.3f ms", 1000.0f / io.Framerate);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        ImGui::Text("Selected tab index: %d", selected_tab);
        if (!message_buffer.empty()) {
            ImGui::TextUnformatted(message_buffer.c_str());
        }
        ImGui::End();

        ImGui::Begin("Channels");
        ImGui::End();

        ImGui::Begin("User");
        ImGui::End();

        ImGui::Begin("Users");
        ImGui::End();

        ImGui::Render();
        ImDrawData *draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);

        SDL_GPUCommandBuffer *command_buffer = SDL_AcquireGPUCommandBuffer(gpu_device);

        SDL_GPUTexture *swapchain_texture;
        SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, window, &swapchain_texture, nullptr, nullptr);

        if (swapchain_texture != nullptr && !is_minimized) {
            ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);

            SDL_GPUColorTargetInfo target_info = {};
            target_info.texture = swapchain_texture;
            target_info.clear_color = SDL_FColor{clear_color.x, clear_color.y, clear_color.z, clear_color.w};
            target_info.load_op = SDL_GPU_LOADOP_CLEAR;
            target_info.store_op = SDL_GPU_STOREOP_STORE;
            target_info.mip_level = 0;
            target_info.layer_or_depth_plane = 0;
            target_info.cycle = false;
            SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);

            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);

            SDL_EndGPURenderPass(render_pass);
        }

        SDL_SubmitGPUCommandBuffer(command_buffer);

        request_redraw = ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown() || io.WantTextInput;
    }

    SDL_WaitForGPUIdle(gpu_device);
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyProperties(props);
    SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
    SDL_DestroyGPUDevice(gpu_device);
    SDL_DestroyWindow(window);
    SDL_Quit();
}