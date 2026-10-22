// "sly4_recomp spu-oracle": the game's EDGE zlib SPU code, run on the SPU interpreter,
// against the native inflate on real Sly4.psarc blocks (docs/SPU.md, 4b).
#pragma once

namespace sly4::spu_oracle {

int cmd(int argc, char** argv);
// "sly4_recomp psarc-hash": CRC32 of every block, inflated natively.
int cmd_hash(int argc, char** argv);

}  // namespace sly4::spu_oracle
