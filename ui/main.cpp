// CircuitLab Android entry point.
//
// SDLActivity loads libSDL3.so and libmain.so, then calls the exported
// SDL_main symbol. Including SDL_main.h renames main() to SDL_main on Android.

#include "App.h"
#include "Theme.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#ifdef __ANDROID__
#include <android/native_window.h>
#include <dlfcn.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <map>
#include <string>

namespace {

void requestHighRefreshRate(SDL_Window* window)
{
#ifdef __ANDROID__
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    if (properties == 0) {
        return;
    }
    auto* nativeWindow = static_cast<ANativeWindow*>(
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
    if (!nativeWindow) {
        return;
    }

    // ANativeWindow_setFrameRate() only exists on Android 11+; resolve it at
    // runtime so the APK still runs on API 26-29.
    using SetFrameRateFn = int32_t (*)(ANativeWindow*, float, int8_t);
    static SetFrameRateFn setFrameRate = []() -> SetFrameRateFn {
        void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            return nullptr;
        }
        return reinterpret_cast<SetFrameRateFn>(
            dlsym(library, "ANativeWindow_setFrameRate"));
    }();
    if (setFrameRate) {
        const int32_t result = setFrameRate(
            nativeWindow, 120.0f,
            static_cast<int8_t>(ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT));
        SDL_Log("Requested 120 Hz frame rate: %d", result);
    }
#else
    (void)window;
#endif
}

void updateSafeInsets(SDL_Window* window, circuitlab::App& app)
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_Rect safe{0, 0, width, height};
    if (!SDL_GetWindowSafeArea(window, &safe) || safe.w <= 0 || safe.h <= 0) {
        safe = SDL_Rect{0, 0, width, height};
    }
    app.setSafeInsets(static_cast<float>(safe.x), static_cast<float>(safe.y),
                      static_cast<float>(std::max(0, width - (safe.x + safe.w))),
                      static_cast<float>(std::max(0, height - (safe.y + safe.h))));
}

}  // namespace

int main(int /*argc*/, char* /*argv*/[])
{
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("CircuitLab", 0, 0, SDL_WINDOW_FULLSCREEN);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Prefer GLES2 for broad Android compatibility; fall back to SDL's default
    // driver, then to the software renderer.
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "opengles2");
    if (!renderer) {
        SDL_Log("GLES2 renderer unavailable (%s), trying default", SDL_GetError());
        renderer = SDL_CreateRenderer(window, nullptr);
    }
    if (!renderer) {
        SDL_Log("Default renderer unavailable (%s), trying software", SDL_GetError());
        renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    }
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_Log("CircuitLab renderer: %s", SDL_GetRendererName(renderer));
    // VSync follows the display refresh rate. The Android Activity asks the
    // platform for 120 Hz, and the native window request below covers the SDL
    // SurfaceView (whose default request is "no preference").
    SDL_SetRenderVSync(renderer, 1);
    requestHighRefreshRate(window);

    const SDL_DisplayMode* displayMode =
        SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (displayMode) {
        SDL_Log("CircuitLab display: %dx%d @ %.1f Hz", displayMode->w, displayMode->h,
                displayMode->refresh_rate);
    }
    SDL_Log("CircuitLab display scale: %.2f", SDL_GetWindowDisplayScale(window));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
        SDL_Log("ImGui SDL3 backend init failed");
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
        SDL_Log("ImGui SDL_Renderer3 backend init failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    float uiScale = SDL_GetWindowDisplayScale(window);
    if (!(uiScale > 0.0f)) {
        // Fallback for platforms that only report the pixel size.
        uiScale = static_cast<float>(windowHeight) / 720.0f * 1.5f;
    }
    uiScale = std::clamp(uiScale, 1.0f, 4.0f);
    Theme::apply(uiScale);
    Theme::loadFont(uiScale);

    char* prefPath = SDL_GetPrefPath("CircuitLab", "circuit-sim");
    std::string baseDir;
    if (prefPath) {
        baseDir = prefPath;
        SDL_free(prefPath);
    }

    circuitlab::App app;
    app.init(baseDir, uiScale);
    updateSafeInsets(window, app);

    std::map<SDL_FingerID, ImVec2> fingers;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                updateSafeInsets(window, app);
                requestHighRefreshRate(window);
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
                fingers[event.tfinger.fingerID] =
                    ImVec2(event.tfinger.x, event.tfinger.y);
                break;
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED:
                fingers.erase(event.tfinger.fingerID);
                break;
            case SDL_EVENT_PINCH_BEGIN:
                app.pinchBegin();
                break;
            case SDL_EVENT_PINCH_UPDATE: {
                ImVec2 center(0.5f, 0.5f);
                if (fingers.size() >= 2) {
                    auto it = fingers.begin();
                    const ImVec2 first = it->second;
                    ++it;
                    const ImVec2 second = it->second;
                    center = ImVec2((first.x + second.x) * 0.5f,
                                    (first.y + second.y) * 0.5f);
                }
                app.pinchZoom(event.pinch.scale, center);
                break;
            }
            case SDL_EVENT_PINCH_END:
                app.pinchEnd();
                fingers.clear();
                break;
            case SDL_EVENT_DID_ENTER_BACKGROUND:
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                app.onAppBackground();
                break;
            case SDL_EVENT_RENDER_DEVICE_RESET:
                ImGui_ImplSDLRenderer3_DestroyDeviceObjects();
                ImGui_ImplSDLRenderer3_CreateDeviceObjects();
                break;
            default:
                break;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 16, 20, 27, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    app.onAppBackground();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
