// sly4_recomp oracle-gen / oracle-check: differential tests of the PPU interpreter
// against QEMU (see oracle.cpp and tools/ppc/oracle/).
#pragma once

namespace sly4::oracle {

int cmd_gen(int argc, char** argv);
int cmd_check(int argc, char** argv);

}  // namespace sly4::oracle
