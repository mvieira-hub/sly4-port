// Firmware PRX modules the port runs as they are (LLE), recompiled like the game. Each
// one is loaded at a fixed address so that its generated code (generated/fw_<name>,
// "sly4_recomp emit --prx ... --base ...") and the runtime see the same relocated image.
// The modules come from the user's own firmware (the PS3UPDAT.PUP on the game disc),
// decrypted with "rpcs3.exe --decrypt"; nothing of them is distributed.
#pragma once

#include "memory/guest_memory.h"

namespace sly4 {

struct FirmwareModule {
    const char* name;  // generated/fw_<name>, register_functions_<name>()
    const char* file;  // decrypted PRX, in the firmware folder (--firmware)
    GuestAddr base;
};

// 0x0F000000-0x0FFFFFFF is kept for firmware modules (below the 64 KiB user area).
constexpr GuestAddr kFirmwareAreaBase = 0x0F000000, kFirmwareAreaEnd = 0x10000000;

inline constexpr FirmwareModule kFirmwareModules[] = {
    {"libsre", "libsre.prx", 0x0F000000},  // SPURS (cellSpurs, cellSpursJq...)
};

}  // namespace sly4
