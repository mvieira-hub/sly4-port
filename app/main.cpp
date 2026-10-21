// sly4.exe: native PC port of Sly Cooper: Thieves in Time (BCES01284).
// Phase 0: opens the window and presents a cleared Vulkan 1.3 swapchain.
//
//   sly4.exe                 window until it is closed (Esc closes)
//   sly4.exe --frames 300    presents 300 frames and exits (smoke test, exit code 0 = ok)
//   sly4.exe --validation    enables VK_LAYER_KHRONOS_validation when installed
//   sly4.exe --no-vsync      MAILBOX/IMMEDIATE present mode when available
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gpu/vulkan_context.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "core/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

struct Args {
    long frames = 0;  // 0 = until the window is closed
    sly4::VulkanOptions vulkan;
};

bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            out.frames = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--validation") == 0) {
            out.vulkan.validation = true;
        } else if (std::strcmp(argv[i], "--no-vsync") == 0) {
            out.vulkan.vsync = false;
        } else {
            SLY4_ERROR("opção desconhecida: %s", argv[i]);
            return false;
        }
    }
    return true;
}

const char* present_mode_name(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        default: return "outro";
    }
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // log messages are UTF-8 (Portuguese accents)
#endif
    Args args;
    if (!parse_args(argc, argv, args)) return 2;
    SLY4_INFO("sly4 %s - port nativo, fase 0 (janela + Vulkan)", "0.0.1");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SLY4_ERROR("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Sly Cooper: Thieves in Time - port nativo (fase 0)", 1280, 720,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        SLY4_ERROR("SDL_CreateWindow: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    sly4::VulkanContext gpu;
    if (!gpu.init(window, args.vulkan)) {
        gpu.shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    VkExtent2D ext = gpu.swapchain_extent();
    SLY4_INFO("swapchain %ux%u, formato %d, %s", ext.width, ext.height, static_cast<int>(gpu.swapchain_format()),
              present_mode_name(gpu.present_mode()));

    // Sly's blue (linear values; the sRGB swapchain converts them on write).
    const std::array<float, 4> clear = {0.02f, 0.09f, 0.35f, 1.0f};
    const auto start = std::chrono::steady_clock::now();
    long presented = 0;
    bool running = true, ok = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running = false; break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.key == SDLK_ESCAPE) running = false;
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: gpu.notify_resized(); break;
                default: break;
            }
        }
        if (!running) break;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(16);
            continue;
        }
        if (!gpu.draw_frame(clear)) {
            ok = false;
            break;
        }
        ++presented;
        if (args.frames > 0 && presented >= args.frames) break;
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    SLY4_INFO("%ld quadros em %.2f s (%.1f fps) em %s", presented, secs, secs > 0 ? presented / secs : 0.0,
              gpu.device_name().c_str());

    gpu.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
}
