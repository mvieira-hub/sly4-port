// sly4.exe: native PC port of Sly Cooper: Thieves in Time (BCES01284).
// Phase 1: loads the decrypted EBOOT into the PS3 address space, then opens the
// Vulkan window of phase 0.
//
//   sly4.exe --game <PS3_GAME>       loads <PS3_GAME>\USRDIR\EBOOT.elf
//   sly4.exe --eboot <EBOOT.elf>     loads that file
//   sly4.exe ... --no-window         stops after loading (diagnostics)
//   sly4.exe ... --dump-imports f.csv  writes module,nid,name,slot of every import
//   sly4.exe --nid-names <file>      NID name candidates (default tools\data\ps3_nid_candidates.txt)
//   sly4.exe --frames 300            presents 300 frames and exits (exit code 0 = ok)
//   sly4.exe --validation            enables VK_LAYER_KHRONOS_validation when installed
//   sly4.exe --no-vsync              MAILBOX/IMMEDIATE present mode when available
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "gpu/vulkan_context.h"
#include "loader/elf_loader.h"
#include "loader/image_report.h"
#include "loader/nid.h"
#include "memory/guest_memory.h"

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

constexpr const char* kDefaultNidNames = "tools/data/ps3_nid_candidates.txt";

struct Args {
    long frames = 0;  // 0 = until the window is closed
    std::string game_dir, eboot, nid_names, dump_imports;
    bool window = true;
    sly4::VulkanOptions vulkan;
};

bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        auto value = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        bool ok = true;
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            out.frames = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--game") == 0) {
            ok = value(out.game_dir);
        } else if (std::strcmp(argv[i], "--eboot") == 0) {
            ok = value(out.eboot);
        } else if (std::strcmp(argv[i], "--nid-names") == 0) {
            ok = value(out.nid_names);
        } else if (std::strcmp(argv[i], "--dump-imports") == 0) {
            ok = value(out.dump_imports);
        } else if (std::strcmp(argv[i], "--no-window") == 0) {
            out.window = false;
        } else if (std::strcmp(argv[i], "--validation") == 0) {
            out.vulkan.validation = true;
        } else if (std::strcmp(argv[i], "--no-vsync") == 0) {
            out.vulkan.vsync = false;
        } else {
            ok = false;
        }
        if (!ok) {
            SLY4_ERROR("opção desconhecida ou sem valor: %s", argv[i]);
            return false;
        }
    }
    if (out.eboot.empty() && !out.game_dir.empty())
        out.eboot = (std::filesystem::path(out.game_dir) / "USRDIR" / "EBOOT.elf").string();
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

// Phase 1: PS3 address space + executable. Returns false if loading failed.
bool load_executable(const Args& args, sly4::GuestMemory& mem, sly4::LoadedImage& img) {
    if (!mem.reserve()) return false;
    sly4::install_guest_fault_handler(mem);

    sly4::NidNames names;
    const std::string nid_file = args.nid_names.empty() ? kDefaultNidNames : args.nid_names;
    if (names.load_file(nid_file))
        SLY4_INFO("%zu nomes de NID carregados de %s", names.size(), nid_file.c_str());
    else if (!args.nid_names.empty())
        SLY4_WARN("lista de NIDs não encontrada: %s", nid_file.c_str());

    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    if (!sly4::load_elf(args.eboot, mem, img, error)) {
        SLY4_ERROR("falha ao carregar %s: %s", args.eboot.c_str(), error.c_str());
        return false;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    SLY4_INFO("%s carregado em %.1f ms; %.1f MiB confirmados na memória do PS3", args.eboot.c_str(), ms,
              mem.committed_bytes() / 1048576.0);
    sly4::log_image_summary(img, names);
    if (!args.dump_imports.empty()) {
        if (sly4::write_imports_csv(img, names, args.dump_imports))
            SLY4_INFO("importações gravadas em %s", args.dump_imports.c_str());
        else
            SLY4_ERROR("não foi possível gravar %s", args.dump_imports.c_str());
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // log messages are UTF-8 (Portuguese accents)
#endif
    Args args;
    if (!parse_args(argc, argv, args)) return 2;
    SLY4_INFO("sly4 %s - port nativo, fase 1 (memória do PS3 + loader)", "0.0.2");

    sly4::GuestMemory mem;
    sly4::LoadedImage image;
    if (!args.eboot.empty() && !load_executable(args, mem, image)) return 3;
    if (!args.window) return 0;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SLY4_ERROR("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Sly Cooper: Thieves in Time - port nativo", 1280, 720,
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
