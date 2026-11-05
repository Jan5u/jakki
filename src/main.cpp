#include <algorithm>
#include <cmath>
#include <memory>
#include <print>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

#include "audio/audio.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "gui.hpp"
#include "network.hpp"
#include "video/capture/capture_linux.hpp"
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
    std::unique_ptr<PipewireCapture> screenshareCapture;
    std::string username = "jansu";
    bool needsRedraw = false;
    bool uiDirty = true;
    Uint32 redrawEventType = 0;
    int popupStackSize = 0;
    UIContext ui;
    Button settingsButton;
    Dialog settingsDialog;
    Button connectButton;
    ConnectDialog connectDialog;
    Button screenshareButton;
    ScreenshareDialog screenshareDialog;
    ChatPanel chat;
    SDL_FRect contentRect{};
    bool viewingScreenshare = false;
    std::string screenshareUser;
    SDL_Texture *uiTexture = nullptr;
    SDL_Surface *dialogSurface = nullptr;
    SDL_Texture *dialogTexture = nullptr;
    SDL_Surface *connectSurface = nullptr;
    SDL_Texture *connectTexture = nullptr;
    SDL_Surface *shareSurface = nullptr;
    SDL_Texture *shareTexture = nullptr;
    SDL_Texture *videoTarget = nullptr;
    int videoTargetWidth = 0;
    int videoTargetHeight = 0;
    SDL_Surface *chatSurface = nullptr;
    SDL_Texture *chatTexture = nullptr;
    float scale = 1.0f;
};

static SDL_Surface *loadTintedIcon(const std::string &path, int size, SDL_Color color) {
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) {
        SDL_Log("Failed to open icon %s: %s", path.c_str(), SDL_GetError());
        return nullptr;
    }
    SDL_Surface *icon = IMG_LoadSizedSVG_IO(io, size, size);
    SDL_CloseIO(io);
    if (!icon) {
        SDL_Log("Failed to load icon %s: %s", path.c_str(), SDL_GetError());
        return nullptr;
    }

    SDL_Surface *tinted = SDL_ConvertSurface(icon, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(icon);
    if (!tinted) {
        return nullptr;
    }

    SDL_LockSurface(tinted);
    Uint32 *pixels = (Uint32 *)tinted->pixels;
    const int pitch = tinted->pitch / (int)sizeof(Uint32);
    for (int y = 0; y < tinted->h; ++y) {
        for (int x = 0; x < tinted->w; ++x) {
            Uint32 *pixel = &pixels[y * pitch + x];
            Uint8 alpha = (Uint8)(*pixel >> 24);
            *pixel = SDL_MapSurfaceRGBA(tinted, color.r, color.g, color.b, (Uint8)(alpha * color.a / 255));
        }
    }
    SDL_UnlockSurface(tinted);

    SDL_SetSurfaceBlendMode(tinted, SDL_BLENDMODE_BLEND);
    return tinted;
}

static bool ensureUiSurface(App *app) {
    int w = 0, h = 0;
    SDL_GetWindowSize(app->window, &w, &h);
    if (app->ui.surface && app->ui.surface->w == w && app->ui.surface->h == h) {
        return false;
    }
    SDL_DestroySurface(app->ui.surface);
    app->ui.surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);

    SDL_DestroyTexture(app->uiTexture);
    app->uiTexture = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (app->uiTexture) {
        SDL_SetTextureBlendMode(app->uiTexture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(app->uiTexture, SDL_SCALEMODE_NEAREST);
    }
    return true;
}

static void ensureSurfaceTexture(SDL_Renderer *renderer, SDL_Surface *&surface, SDL_Texture *&texture, int w, int h) {
    if (surface && surface->w == w && surface->h == h) {
        return;
    }
    SDL_DestroySurface(surface);
    surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
}

static bool ensureVideoTarget(App *app, int w, int h) {
    if (app->videoTarget && app->videoTargetWidth == w && app->videoTargetHeight == h) {
        return true;
    }
    SDL_DestroyTexture(app->videoTarget);
    app->videoTarget = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!app->videoTarget) {
        app->videoTargetWidth = 0;
        app->videoTargetHeight = 0;
        return false;
    }
    SDL_SetTextureScaleMode(app->videoTarget, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(app->videoTarget, SDL_BLENDMODE_NONE);
    app->videoTargetWidth = w;
    app->videoTargetHeight = h;
    return true;
}

static void layoutUi(App *app) {
    int w = 0, h = 0;
    SDL_GetWindowSize(app->window, &w, &h);

    const float margin = 20.0f * app->scale;
    const float sidebarWidth = std::round(std::min(240.0f * app->scale, (float)w * 0.35f));
    const float bottomBarHeight = std::round(56.0f * app->scale);
    const float controlSize = 40.0f * app->scale;
    const float controlPadding = 12.0f * app->scale;
    const float controlsY = std::round((float)h - bottomBarHeight + (bottomBarHeight - controlSize) * 0.5f);

    app->settingsButton.rect = {std::round(sidebarWidth - controlPadding - controlSize), controlsY, controlSize, controlSize};
    const float controlGap = 8.0f * app->scale;
    app->screenshareButton.rect = {std::round(app->settingsButton.rect.x - controlGap - controlSize), controlsY, controlSize, controlSize};
    const float connectButtonWidth = std::round(app->screenshareButton.rect.x - controlGap - controlPadding);
    app->connectButton.rect = {std::round(controlPadding), controlsY, connectButtonWidth, controlSize};

    const float minWidth = 480.0f * app->scale;
    const float maxWidth = 1000.0f * app->scale;
    const float minHeight = 360.0f * app->scale;
    const float maxHeight = 680.0f * app->scale;
    float dialogWidth = std::round(std::clamp((float)w * 0.72f, minWidth, maxWidth));
    float dialogHeight = std::round(std::clamp((float)h * 0.72f, minHeight, maxHeight));
    dialogWidth = std::round(std::min(dialogWidth, (float)w - margin * 2.0f));
    dialogHeight = std::round(std::min(dialogHeight, (float)h - margin * 2.0f));

    app->settingsDialog.rect = {
        std::round((w - dialogWidth) * 0.5f),
        std::round((h - dialogHeight) * 0.5f),
        dialogWidth,
        dialogHeight,
    };
    app->settingsDialog.layout();

    ensureSurfaceTexture(app->renderer, app->dialogSurface, app->dialogTexture, (int)dialogWidth, (int)dialogHeight);

    const float connectWidth = std::round(std::min(460.0f * app->scale, (float)w - margin * 2.0f));
    const float connectHeight = std::round(std::min(300.0f * app->scale, (float)h - margin * 2.0f));
    app->connectDialog.rect = {
        std::round((w - connectWidth) * 0.5f),
        std::round((h - connectHeight) * 0.5f),
        connectWidth,
        connectHeight,
    };
    app->connectDialog.layout();

    ensureSurfaceTexture(app->renderer, app->connectSurface, app->connectTexture, (int)connectWidth, (int)connectHeight);

    const float shareWidth = std::round(std::min(420.0f * app->scale, (float)w - margin * 2.0f));
    const float shareHeight = std::round(std::min(220.0f * app->scale, (float)h - margin * 2.0f));
    app->screenshareDialog.rect = {
        std::round((w - shareWidth) * 0.5f),
        std::round((h - shareHeight) * 0.5f),
        shareWidth,
        shareHeight,
    };
    app->screenshareDialog.layout();

    ensureSurfaceTexture(app->renderer, app->shareSurface, app->shareTexture, (int)shareWidth, (int)shareHeight);

    app->gui.channelList.rect = {0.0f, 0.0f, sidebarWidth, std::round((float)h - bottomBarHeight)};

    app->contentRect = {
        std::round(sidebarWidth),
        0.0f,
        std::round((float)w - sidebarWidth),
        (float)h,
    };
    app->chat.rect = app->contentRect;
    app->chat.layout();

    ensureSurfaceTexture(app->renderer, app->chatSurface, app->chatTexture, (int)app->chat.rect.w, (int)app->chat.rect.h);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    auto app = std::make_unique<App>();

    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "waitevent");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
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

    SDL_SetRenderVSync(app->renderer, SDL_RENDERER_VSYNC_DISABLED);
    if (app->renderer == nullptr) {
        SDL_Log("Error: SDL_CreateRenderer(): %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    app->video.renderer = app->renderer;
    SDL_SetWindowPosition(app->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(app->window);

    TTF_Init();
    app->ui.font = TTF_OpenFont("/usr/share/fonts/TTF/DejaVuSans.ttf", 16.0f * main_scale);
    if (!app->ui.font) {
        SDL_Log("Failed to open font: %s", SDL_GetError());
    }
    ensureUiSurface(app.get());

    app->scale = main_scale;
    app->gui.channelList.channelHeight = 30.0f * main_scale;
    app->gui.channelList.userHeight = 26.0f * main_scale;
    app->gui.channelList.userIndent = 22.0f * main_scale;
    app->gui.channelList.avatarSize = 18.0f * main_scale;
    app->gui.channelList.padding = 8.0f * main_scale;
    app->gui.channelList.topPadding = 12.0f * main_scale;
    app->gui.channelList.cornerRadius = 0.0f;
    app->gui.channelList.rowRadius = 4.0f * main_scale;
    app->settingsButton.icon =
        loadTintedIcon(std::string(JAKKI_ASSET_DIR) + "/ui/icons/settings.svg", (int)(20.0f * main_scale), app->settingsButton.textColor);

    app->settingsDialog.title = "Settings";
    app->settingsDialog.cornerRadius = 10.0f * main_scale;
    app->settingsDialog.itemRadius = 8.0f * main_scale;
    app->settingsDialog.padding = 24.0f * main_scale;
    app->settingsDialog.titleArea = 56.0f * main_scale;
    app->settingsDialog.iconSize = 20.0f * main_scale;
    app->settingsDialog.sliderHeight = 20.0f * main_scale;
    app->settingsDialog.sliderWidth = 340.0f * main_scale;
    app->settingsDialog.contentGap = 16.0f * main_scale;
    app->settingsDialog.navWidth = 180.0f * main_scale;
    app->settingsDialog.navItemHeight = 40.0f * main_scale;
    app->settingsDialog.navPadding = 12.0f * main_scale;
    app->settingsDialog.navItemGap = 4.0f * main_scale;

    const int sectionIconSize = (int)app->settingsDialog.iconSize;
    const std::string iconDir = std::string(JAKKI_ASSET_DIR) + "/ui/icons/";

    app->gui.channelList.avatarIcon = loadTintedIcon(iconDir + "user.svg", (int)app->gui.channelList.avatarSize, app->gui.channelList.userNameColor);

    app->chat.cornerRadius = 0.0f;
    app->chat.padding = 16.0f * main_scale;
    app->chat.titleArea = 44.0f * main_scale;
    app->chat.inputHeight = 36.0f * main_scale;
    app->chat.inputGap = 12.0f * main_scale;
    app->chat.buttonWidth = 90.0f * main_scale;
    app->chat.lineHeight = 22.0f * main_scale;
    app->chat.input.font = app->ui.font;
    app->chat.input.placeholder = "Message...";
    app->chat.input.padding = 10.0f * main_scale;
    app->chat.input.cornerRadius = 6.0f * main_scale;
    app->chat.sendButton = {{}, "Send"};
    app->chat.sendButton.cornerRadius = 6.0f * main_scale;

    DialogSection appearance;
    appearance.title = "Appearance";
    appearance.icon = loadTintedIcon(iconDir + "wallpaper.svg", sectionIconSize, app->settingsDialog.sectionColor);
    appearance.sliders.push_back({{}, 0.7f});
    appearance.sliders.push_back({{}, 0.4f});

    DialogSection audio;
    audio.title = "Audio";
    audio.icon = loadTintedIcon(iconDir + "headphones.svg", sectionIconSize, app->settingsDialog.sectionColor);

    DialogSection screenshare;
    screenshare.title = "Screenshare";
    screenshare.icon = loadTintedIcon(iconDir + "monitor.svg", sectionIconSize, app->settingsDialog.sectionColor);

    app->settingsDialog.sections.push_back(std::move(appearance));
    app->settingsDialog.sections.push_back(std::move(audio));
    app->settingsDialog.sections.push_back(std::move(screenshare));

    app->connectButton = {{}, "Connect"};

    app->connectDialog.title = "Connect to Server";
    app->connectDialog.cornerRadius = 10.0f * main_scale;
    app->connectDialog.padding = 24.0f * main_scale;
    app->connectDialog.titleArea = 56.0f * main_scale;
    app->connectDialog.labelHeight = 18.0f * main_scale;
    app->connectDialog.fieldHeight = 36.0f * main_scale;
    app->connectDialog.fieldGap = 18.0f * main_scale;
    app->connectDialog.buttonWidth = 120.0f * main_scale;
    app->connectDialog.address.text = app->connect_address;
    app->connectDialog.address.placeholder = "127.0.0.1";
    app->connectDialog.address.font = app->ui.font;
    app->connectDialog.address.padding = 10.0f * main_scale;
    app->connectDialog.address.cornerRadius = 6.0f * main_scale;
    app->connectDialog.username.text = app->username;
    app->connectDialog.username.placeholder = "username";
    app->connectDialog.username.font = app->ui.font;
    app->connectDialog.username.padding = 10.0f * main_scale;
    app->connectDialog.username.cornerRadius = 6.0f * main_scale;
    app->connectDialog.connectButton = {{}, "Connect"};
    app->connectDialog.connectButton.cornerRadius = 6.0f * main_scale;

    app->screenshareButton.icon = loadTintedIcon(iconDir + "monitor.svg", (int)(20.0f * main_scale), app->settingsButton.textColor);
    app->screenshareDialog.cornerRadius = 10.0f * main_scale;
    app->screenshareDialog.padding = 24.0f * main_scale;
    app->screenshareDialog.titleArea = 56.0f * main_scale;
    app->screenshareDialog.messageHeight = 44.0f * main_scale;
    app->screenshareDialog.buttonWidth = 120.0f * main_scale;
    app->screenshareDialog.buttonHeight = 36.0f * main_scale;
    app->screenshareDialog.buttonGap = 12.0f * main_scale;
    app->screenshareDialog.startButton = {{}, "Start"};
    app->screenshareDialog.startButton.cornerRadius = 6.0f * main_scale;
    app->screenshareDialog.cancelButton = {{}, "Cancel"};
    app->screenshareDialog.cancelButton.cornerRadius = 6.0f * main_scale;

    layoutUi(app.get());

    app->network.setAuthManager(&app->auth);
    app->network.setGUI(&app->gui);
    app->network.setAudio(&app->audio);
    app->network.setVideo(&app->video);
    app->auth.setUsername(app->username);

    app->needsRedraw = true;
    app->redrawEventType = SDL_RegisterEvents(1);
    app->gui.setOnStateChanged([app_raw = app.get()]() {
        SDL_Event event;
        SDL_zero(event);
        event.type = app_raw->redrawEventType;
        SDL_PushEvent(&event);
    });

    *appstate = app.release();

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    auto app = static_cast<App *>(appstate);

    if (app->video.decodeLoop()) {
        app->needsRedraw = true;
    }

    if (SDL_GetWindowFlags(app->window) & SDL_WINDOW_MINIMIZED) {
        return SDL_APP_CONTINUE;
    }

    if (!app->needsRedraw) {
        return SDL_APP_CONTINUE;
    }
    app->needsRedraw = false;

    if (ensureUiSurface(app)) {
        app->uiDirty = true;
    }
    layoutUi(app);

    if (app->uiDirty) {
        SDL_FillSurfaceRect(app->ui.surface, nullptr, 0);
        app->gui.channelList.draw(app->ui);
        app->connectButton.draw(app->ui);
        app->screenshareButton.draw(app->ui);
        app->settingsButton.draw(app->ui);
        SDL_UpdateTexture(app->uiTexture, nullptr, app->ui.surface->pixels, app->ui.surface->pitch);
        app->uiDirty = false;
    }

    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);
    SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);
    SDL_RenderTexture(app->renderer, app->uiTexture, nullptr, nullptr);

    // Main content: screenshare stream takes over the area where chat is rendered.
    if (app->viewingScreenshare) {
        const SDL_FRect content = app->contentRect;
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(app->renderer, &content);

        SDL_Texture *videoTexture = app->video.GetTexture();
        if (videoTexture) {
            float fullWidth = 0.0f, fullHeight = 0.0f;
            SDL_GetTextureSize(videoTexture, &fullWidth, &fullHeight);
            const float uvLeft = app->video.GetVisibleUvLeft();
            const float uvTop = app->video.GetVisibleUvTop();
            const float uvRight = app->video.GetVisibleUvRight();
            const float uvBottom = app->video.GetVisibleUvBottom();
            SDL_FRect src{uvLeft * fullWidth, uvTop * fullHeight, (uvRight - uvLeft) * fullWidth, (uvBottom - uvTop) * fullHeight};
            if (src.w > 0.0f && src.h > 0.0f && ensureVideoTarget(app, (int)src.w, (int)src.h)) {
                // Blit the YUV frame 1:1 into the RGB target (no scaling => no aliasing),
                // then scale the target down with linear filtering.
                SDL_SetRenderTarget(app->renderer, app->videoTarget);
                SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
                SDL_RenderClear(app->renderer);
                SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);
                SDL_RenderTexture(app->renderer, videoTexture, &src, nullptr);
                SDL_SetRenderTarget(app->renderer, nullptr);

                const float aspect = src.w / src.h;
                float dstWidth = content.w;
                float dstHeight = dstWidth / aspect;
                if (dstHeight > content.h) {
                    dstHeight = content.h;
                    dstWidth = dstHeight * aspect;
                }
                SDL_FRect dst{content.x + (content.w - dstWidth) * 0.5f, content.y + (content.h - dstHeight) * 0.5f, dstWidth, dstHeight};
                SDL_RenderTexture(app->renderer, app->videoTarget, nullptr, &dst);
            }
        }
    } else if (app->chat.open && app->chatTexture) {
        app->chat.messages = app->gui.getMessages(app->chat.channel);
        SDL_FillSurfaceRect(app->chatSurface, nullptr, 0);
        UIContext chatUi{app->chatSurface, app->ui.font};
        app->chat.draw(chatUi);
        SDL_UpdateTexture(app->chatTexture, nullptr, app->chatSurface->pixels, app->chatSurface->pitch);
        SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);
        SDL_RenderTexture(app->renderer, app->chatTexture, nullptr, &app->chat.rect);
    }

    const bool settingsOpen = app->settingsDialog.open && app->dialogTexture;
    const bool connectOpen = app->connectDialog.open && app->connectTexture;
    const bool shareOpen = app->screenshareDialog.open && app->shareTexture;
    if (settingsOpen || connectOpen || shareOpen) {
        SDL_Color dim = app->settingsDialog.dimColor;
        SDL_Surface *surface = app->dialogSurface;
        SDL_Texture *texture = app->dialogTexture;
        SDL_FRect dst = app->settingsDialog.rect;
        if (connectOpen) {
            dim = app->connectDialog.dimColor;
            surface = app->connectSurface;
            texture = app->connectTexture;
            dst = app->connectDialog.rect;
        } else if (shareOpen) {
            dim = app->screenshareDialog.dimColor;
            surface = app->shareSurface;
            texture = app->shareTexture;
            dst = app->screenshareDialog.rect;
        }

        SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(app->renderer, dim.r, dim.g, dim.b, dim.a);
        SDL_RenderFillRect(app->renderer, nullptr);
        SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);

        SDL_FillSurfaceRect(surface, nullptr, 0);
        UIContext dialogUi{surface, app->ui.font};
        if (connectOpen) {
            app->connectDialog.draw(dialogUi);
        } else if (shareOpen) {
            app->screenshareDialog.draw(dialogUi);
        } else {
            app->settingsDialog.draw(dialogUi);
        }
        SDL_UpdateTexture(texture, nullptr, surface->pixels, surface->pitch);
        SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);
        SDL_RenderTexture(app->renderer, texture, nullptr, &dst);
    }

    SDL_RenderPresent(app->renderer);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    auto app = static_cast<App *>(appstate);

    const bool dialogOpenBefore = app->settingsDialog.open || app->connectDialog.open || app->screenshareDialog.open;
    if (app->connectDialog.open) {
        app->connectDialog.handleEvent(*event);
        if (app->connectDialog.connectRequested) {
            app->connectDialog.connectRequested = false;
            app->connect_address = app->connectDialog.address.text;
            app->username = app->connectDialog.username.text;
            app->auth.setUsername(app->username);
            std::println("Connecting to {}:{} as {}", app->connect_address, app->connect_port, app->username);
            app->network.connectToServer(app->connect_address, app->connect_port);
            app->connectDialog.open = false;
        }
    } else if (app->screenshareDialog.open) {
        app->screenshareDialog.handleEvent(*event);
        if (app->screenshareDialog.startRequested) {
            app->screenshareDialog.startRequested = false;
            app->screenshareCapture = std::make_unique<PipewireCapture>(&app->network);
            app->screenshareCapture->selectScreen();
            app->screenshareCapture->startCapture();
            app->screenshareDialog.open = false;
        }
    } else if (app->settingsDialog.open) {
        app->settingsDialog.handleEvent(*event);
    } else {
        // Non-modal chat panel
        if (app->chat.open) {
            app->chat.handleEvent(*event);
            if (app->chat.sendRequested) {
                app->chat.sendRequested = false;
                if (!app->chat.input.text.empty()) {
                    app->network.sendTextMessage(app->chat.channel, app->chat.input.text);
                    app->chat.input.text.clear();
                    app->chat.input.cursor = 0;
                    app->chat.input.selectionAnchor = 0;
                    app->chat.input.focused = true;
                }
            }
            if (!app->chat.open) {
                SDL_StopTextInput(app->window);
                TextInput::resetCursor();
                app->chat.input.hovered = false;
                app->chat.input.selecting = false;
                app->chat.input.focused = false;
            }
        }

        bool insideChat = false;
        if (app->chat.open &&
            (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP || event->type == SDL_EVENT_MOUSE_MOTION)) {
            const float mx = event->type == SDL_EVENT_MOUSE_MOTION ? event->motion.x : event->button.x;
            const float my = event->type == SDL_EVENT_MOUSE_MOTION ? event->motion.y : event->button.y;
            const SDL_FPoint point{mx, my};
            insideChat = SDL_PointInRectFloat(&point, &app->chat.rect);
        }

        if (!insideChat) {
            if (app->connectButton.handleEvent(*event)) {
                app->connectDialog.open = true;
                app->connectDialog.address.focused = true;
                app->connectDialog.address.cursor = (int)app->connectDialog.address.text.size();
                app->connectDialog.address.selectionAnchor = app->connectDialog.address.cursor;
                app->connectDialog.username.focused = false;
                SDL_StartTextInput(app->window);
            } else if (app->settingsButton.handleEvent(*event)) {
                app->settingsDialog.open = true;
            } else if (app->screenshareButton.handleEvent(*event)) {
                app->screenshareDialog.open = true;
            }

            std::string clickedChannel;
            std::string clickedUser;
            if (app->gui.channelList.handleEvent(*event, clickedChannel, clickedUser)) {
                if (!clickedUser.empty()) {
                    if (!app->viewingScreenshare || app->screenshareUser != clickedUser) {
                        std::println("Joining screenshare from: {}", clickedUser);
                        app->viewingScreenshare = true;
                        app->screenshareUser = clickedUser;
                        app->chat.open = false;
                        SDL_StopTextInput(app->window);
                        TextInput::resetCursor();
                        app->network.joinScreenShare(clickedUser);
                    }
                } else if (!clickedChannel.empty()) {
                    app->viewingScreenshare = false;
                    app->screenshareUser.clear();
                    if (ChannelList::isTextChannel(clickedChannel)) {
                        app->chat.open = true;
                        app->chat.channel = clickedChannel;
                        app->chat.messages = app->gui.getMessages(clickedChannel);
                        app->chat.input.text.clear();
                        app->chat.input.cursor = 0;
                        app->chat.input.selectionAnchor = 0;
                        app->chat.input.focused = true;
                        SDL_StartTextInput(app->window);
                        app->network.requestChannelHistory(clickedChannel, 50);
                    } else {
                        std::println("Joining voice channel: {}", clickedChannel);
                        app->network.joinVoiceChannel(clickedChannel);
                    }
                }
            }
        }
    }
    const bool dialogClosed = dialogOpenBefore && !(app->settingsDialog.open || app->connectDialog.open || app->screenshareDialog.open);
    if (dialogClosed) {
        SDL_StopTextInput(app->window);
        TextInput::resetCursor();
        app->connectDialog.address.hovered = false;
        app->connectDialog.username.hovered = false;
        app->connectDialog.address.selecting = false;
        app->connectDialog.username.selecting = false;
    }

    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event->window.windowID == SDL_GetWindowID(app->window))
            return SDL_APP_SUCCESS;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        app->needsRedraw = true;
        if (!dialogOpenBefore) {
            app->uiDirty = true;
        }
        break;
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_POSITION:
    case SDL_EVENT_DROP_COMPLETE:
    case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_TEXT:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
    case SDL_EVENT_DISPLAY_ADDED:
    case SDL_EVENT_DISPLAY_REMOVED:
    case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        app->needsRedraw = true;
        app->uiDirty = true;
        break;
    default:
        if (event->type == app->redrawEventType) {
            app->needsRedraw = true;
            app->uiDirty = true;
        }
        break;
    }

    if (dialogClosed) {
        app->needsRedraw = true;
        app->uiDirty = true;
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    auto app = static_cast<App *>(appstate);
    for (auto &section : app->settingsDialog.sections) {
        SDL_DestroySurface(section.icon);
    }
    SDL_DestroyTexture(app->dialogTexture);
    SDL_DestroySurface(app->dialogSurface);
    SDL_DestroyTexture(app->connectTexture);
    SDL_DestroySurface(app->connectSurface);
    SDL_DestroyTexture(app->shareTexture);
    SDL_DestroySurface(app->shareSurface);
    SDL_DestroyTexture(app->videoTarget);
    SDL_DestroyTexture(app->chatTexture);
    SDL_DestroySurface(app->chatSurface);
    SDL_DestroyTexture(app->uiTexture);
    SDL_DestroySurface(app->settingsButton.icon);
    SDL_DestroySurface(app->screenshareButton.icon);
    SDL_DestroySurface(app->gui.channelList.avatarIcon);
    TextInput::cleanupCursors();
    TTF_CloseFont(app->ui.font);
    TTF_Quit();
    SDL_DestroySurface(app->ui.surface);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    SDL_Quit();
}
