

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <memory>
#include <string>
#include <print>

#include <SDL3/SDL.h>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

#include "network.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "gui.hpp"
#include "audio/audio.hpp"
#include "video/video.hpp"

static int selectedChannel = -1;
static int selectedUser = -1;
struct App {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    std::string connect_address = "127.0.0.1";
    std::string connect_port = "7777";
    Network network;
    Auth auth;
    GUI gui;
    Config config;
    Audio audio{network, config};
    Video video;
    std::string username = "jansu";
};

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    auto app = std::make_unique<App>();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
    app->window = SDL_CreateWindow("Jakki", (int)(1280 * main_scale), (int)(800 * main_scale), window_flags);
    if (app->window == nullptr) {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }


    app->video.vulkan_context = app->video.CreateVulkanVideoContext(app->window);

    SDL_PropertiesID props;
    props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, "vulkan");
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, app->window);
    app->video.SetupVulkanRenderProperties(app->video.vulkan_context, props);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB);
    app->renderer = SDL_CreateRendererWithProperties(props);
    SDL_DestroyProperties(props);


    // app->renderer = SDL_CreateRenderer(app->window, nullptr);
    SDL_SetRenderVSync(app->renderer, 1);
    if (app->renderer == nullptr) {
        SDL_Log("Error: SDL_CreateRenderer(): %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    app->video.renderer = app->renderer;
    SDL_SetWindowPosition(app->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(app->window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui_ImplSDL3_InitForSDLRenderer(app->window, app->renderer);
    ImGui_ImplSDLRenderer3_Init(app->renderer);

    app->network.setAuthManager(&app->auth);
    app->network.setGUI(&app->gui);
    app->network.setAudio(&app->audio);
    app->auth.setUsername(app->username);

    *appstate = app.release();

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    auto app = static_cast<App *>(appstate);

    if (SDL_GetWindowFlags(app->window) & SDL_WINDOW_MINIMIZED) {
        SDL_WaitEvent(nullptr);
        return SDL_APP_CONTINUE;
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuiIO &io = ImGui::GetIO();

    app->video.decodeLoop();

    bool show_demo_window = true;
    bool show_another_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    // ImGui::ShowDemoWindow(&show_demo_window);

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
        auto SetNoTabBar = [](ImGuiID id) {
            if (ImGuiDockNode *node = ImGui::DockBuilderGetNode(id))
                node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
        };
        SetNoTabBar(dock_id_left_top);
        SetNoTabBar(dock_id_left_bottom);
        SetNoTabBar(dock_id_main);
        SetNoTabBar(dock_id_right);
    }

    ImGui::DockSpaceOverViewport(dockspace_id, viewport, ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::Begin("Tabs");
    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem("General")) {
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Screenshare")) {
            SDL_Texture *video_texture = app->video.GetTexture();
            if (video_texture) {
                ImGui::Image((ImTextureID)video_texture, ImVec2((float)app->video.GetTextureWidth(), (float)app->video.GetTextureHeight()));
            } else {
                ImGui::Text("No video texture");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) {
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    ImGui::Begin("Channels");

    ImGui::BeginChild("user_card", ImVec2(0, 32), true);
    ImGui::Text("%s", app->username.c_str());
    ImGui::EndChild();

    auto channels = app->gui.getChannelList();
    int idx_c = 0;
    int idx_u = 0;
    for (const auto &ch : channels) {
        if (ImGui::Selectable(ch.name.c_str(), selectedChannel == idx_c)) {
            selectedChannel = idx_c;
            if (!ch.name.starts_with('#')) {
                app->network.joinVoiceChannel(ch.name);
            }
        }
        for (const auto &user : ch.users) {
            if (ImGui::Selectable(user.username.c_str(), selectedUser == idx_u)) {
                selectedUser = idx_u;
            }
            idx_u++;
        }
        idx_c++;
    }

    ImGui::End();

    ImGui::Begin("User");
    if (ImGui::Button("Connect")) {
        ImGui::OpenPopup("Connect to Server");
    }
    if (ImGui::BeginPopupModal("Connect to Server", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Address");
        ImGui::InputText("##Address", &app->connect_address);
        ImGui::Text("Port");
        ImGui::InputText("##Port", &app->connect_port);
        ImGui::Text("Username");
        ImGui::InputText("##Username", &app->username);


    
    if (ImGui::Button("Connect")) {
        ImGui::CloseCurrentPopup();
        std::println("Connect to: {}", app->connect_address);
        app->auth.setUsername(app->username);
        app->network.connectToServer(app->connect_address, app->connect_port);
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
    }
    
    ImGui::EndPopup();
    }
    ImGui::End();

    ImGui::Begin("Users");
    ImGui::End();

    ImGui::Render();
    SDL_SetRenderScale(app->renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(app->renderer, clear_color.x, clear_color.y, clear_color.z, clear_color.w);
    SDL_RenderClear(app->renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), app->renderer);
    SDL_RenderPresent(app->renderer);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    auto app = static_cast<App *>(appstate);
    ImGui_ImplSDL3_ProcessEvent(event);
    if (event->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event->window.windowID == SDL_GetWindowID(app->window))
        return SDL_APP_SUCCESS;

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    auto app = static_cast<App *>(appstate);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    SDL_Quit();
}