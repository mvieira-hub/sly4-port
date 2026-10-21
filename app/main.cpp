// sly4.exe: native PC port of Sly Cooper: Thieves in Time (BCES01284).
// Loads the decrypted EBOOT into the PS3 address space and, when the build includes the
// recompiled game code (generated/ppu), starts the game's main thread on the phase 3
// kernel; the Vulkan window of phase 0 runs alongside.
//
//   sly4.exe --game <PS3_GAME>       loads <PS3_GAME>\USRDIR\EBOOT.elf
//   sly4.exe --eboot <EBOOT.elf>     loads that file
//   sly4.exe ... --no-window         no window: runs the game until it exits (or --seconds)
//   sly4.exe ... --no-run            loads only, does not start the game
//   sly4.exe ... --seconds N         stops the game after N seconds and prints the summary
//   sly4.exe ... --trace             logs every library call and syscall, not just the first
//   sly4.exe ... --data <dir>        where /dev_hdd0 and /dev_hdd1 live (default %LOCALAPPDATA%\sly4-port)
//   sly4.exe ... --dump-imports f.csv  writes module,nid,name,slot of every import
//   sly4.exe --nid-names <file>      NID name candidates (default tools\data\ps3_nid_candidates.txt)
//   sly4.exe --frames 300            presents 300 frames and exits (exit code 0 = ok)
//   sly4.exe --validation            enables VK_LAYER_KHRONOS_validation when installed
//   sly4.exe --no-vsync              MAILBOX/IMMEDIATE present mode when available
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "cpu/ppu_rt.h"
#include "gpu/vulkan_context.h"
#include "kernel/kernel.h"
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

#ifdef SLY4_HAS_GEN
namespace sly4::gen {
void register_functions();  // generated/ppu/table.cpp
}
#endif

namespace {

constexpr const char* kDefaultNidNames = "tools/data/ps3_nid_candidates.txt";

struct Args {
    long frames = 0;  // 0 = until the window is closed
    long seconds = 0;  // 0 = no limit for the game
    std::string game_dir, eboot, nid_names, dump_imports, data_dir;
    bool window = true, run = true, trace = false;
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
        } else if (std::strcmp(argv[i], "--no-run") == 0) {
            out.run = false;
        } else if (std::strcmp(argv[i], "--data") == 0) {
            ok = value(out.data_dir);
        } else if (std::strcmp(argv[i], "--trace") == 0) {
            out.trace = true;
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            out.seconds = std::strtol(argv[++i], nullptr, 10);
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
bool load_executable(const Args& args, sly4::GuestMemory& mem, sly4::LoadedImage& img, sly4::NidNames& names) {
    if (!mem.reserve()) return false;
    sly4::install_guest_fault_handler(mem);

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

// ---------------------------------------------------------------- phase 3: the game
sly4::Kernel* g_kernel = nullptr;
// The game is over once any guest thread calls sys_process_exit (or main returns).
bool game_over() { return g_kernel && g_kernel->exited(); }
int game_status() { return g_kernel ? g_kernel->exit_status() : 0; }

[[noreturn]] void stop_with_summary(int code) {
    if (g_kernel) g_kernel->print_summary();
    std::fflush(stderr);
    std::_Exit(code);
}

#ifdef SLY4_HAS_GEN
void on_guest_fault(sly4::PPUContext& c, sly4::GuestMemory&, const char* what) {
    SLY4_ERROR("PPU: %s em 0x%08X (LR 0x%08llX)", what, c.cia, static_cast<unsigned long long>(c.lr));
    stop_with_summary(4);
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* e) {
    const sly4::PpuThread* t = sly4::current_thread();
    SLY4_ERROR("exceção 0x%08lX no host%s; LR do convidado 0x%08llX", e->ExceptionRecord->ExceptionCode,
               t ? (" na thread " + t->name).c_str() : "", t ? static_cast<unsigned long long>(t->ctx.lr) : 0ull);
    stop_with_summary(5);
}

DWORD WINAPI game_thread(void*) {
    g_kernel->run_main();
    return 0;
}
#endif

// Starts the game's main thread on a host thread with a large stack (recompiled guest
// calls nest as host calls). Returns false if the game cannot run in this build.
bool start_game(const Args& args, sly4::GuestMemory& mem, const sly4::LoadedImage& img, const sly4::NidNames& names) {
#ifdef SLY4_HAS_GEN
    sly4::gen::register_functions();
    static sly4::Kernel kernel(mem, img);
    g_kernel = &kernel;
    sly4::KernelOptions opt;
    opt.trace = args.trace;
    // PS3_GAME is the folder above USRDIR\EBOOT.elf; saves and caches go to %LOCALAPPDATA%.
    const std::filesystem::path eboot = std::filesystem::absolute(args.eboot);
    opt.game_dir = args.game_dir.empty() ? eboot.parent_path().parent_path().string()
                                         : std::filesystem::absolute(args.game_dir).string();
    const char* local = std::getenv("LOCALAPPDATA");
    opt.data_dir = args.data_dir.empty() ? (std::filesystem::path(local ? local : ".") / "sly4-port").string()
                                         : std::filesystem::absolute(args.data_dir).string();
    if (!kernel.init(names, opt)) return false;
    sly4::rt::hooks().fault = on_guest_fault;
    SetUnhandledExceptionFilter(on_unhandled);
    HANDLE h = CreateThread(nullptr, 256u << 20, game_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) return false;
    CloseHandle(h);
    return true;
#else
    SLY4_WARN("este build não tem o código do jogo: gere com \"sly4_recomp emit\" e compile de novo");
    return false;
#endif
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // log messages are UTF-8 (Portuguese accents)
#endif
    Args args;
    if (!parse_args(argc, argv, args)) return 2;
    SLY4_INFO("sly4 %s - port nativo, fase 3 (kernel e HLE)", "0.0.3");

    sly4::GuestMemory mem;
    sly4::LoadedImage image;
    sly4::NidNames names;
    if (!args.eboot.empty() && !load_executable(args, mem, image, names)) return 3;
    const bool game = !args.eboot.empty() && args.run && start_game(args, mem, image, names);
    const auto game_start = std::chrono::steady_clock::now();
    auto game_timed_out = [&] {
        return args.seconds > 0 && std::chrono::steady_clock::now() - game_start >= std::chrono::seconds(args.seconds);
    };
    if (!args.window) {
        if (!game) return 0;
        while (!game_over() && !game_timed_out()) Sleep(10);
        if (!game_over()) SLY4_INFO("tempo esgotado (%ld s): o jogo continua rodando; parando", args.seconds);
        stop_with_summary(game_over() ? game_status() : 0);
    }

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
        if (game && (game_over() || game_timed_out())) break;
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    SLY4_INFO("%ld quadros em %.2f s (%.1f fps) em %s", presented, secs, secs > 0 ? presented / secs : 0.0,
              gpu.device_name().c_str());

    gpu.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    // Guest threads cannot be interrupted: leave without waiting for them.
    if (game) stop_with_summary(ok ? game_status() : 1);
    return ok ? 0 : 1;
}
