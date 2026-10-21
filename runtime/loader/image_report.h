// Human-readable summary and CSV dump of a loaded executable (diagnostics).
#pragma once

#include <string>

#include "loader/elf_loader.h"
#include "loader/nid.h"

namespace sly4 {

void log_image_summary(const LoadedImage& img, const NidNames& names);

// module,nid,name,slot - the same columns eboot_scan.py writes (minus the stub address),
// so the two can be compared line by line.
bool write_imports_csv(const LoadedImage& img, const NidNames& names, const std::string& path);

}  // namespace sly4
