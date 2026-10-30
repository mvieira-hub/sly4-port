# Legal notice

**sly4-port** is an independent, non-commercial research project about the
software of *Sly Cooper: Thieves in Time* (PlayStation 3). Its purpose is
**software preservation** and **interoperability**: understanding how a game
written for the Cell Broadband Engine (PowerPC 64 PPU + SPUs, LV2 kernel, RSX
GPU) works, and making a copy that someone legitimately owns run natively on a
different platform (Windows on x86-64).

## No proprietary material is included

This repository contains only original work: source code, scripts, build
files and documentation written for this project. It does **not** contain,
and has never contained in its history:

- the game's executable (`EBOOT.BIN` / `EBOOT.elf`), SPU programs or any other
  code from the disc, in original, decrypted, disassembled or recompiled form
  (the engineering notes quote at most a handful of instructions at a time,
  with addresses, to explain a specific finding);
- game data or assets of any kind: archives, models, textures, audio, video,
  scripts, shaders, text or images;
- PlayStation 3 firmware, system modules or SDK libraries;
- encryption keys, license files, console identifiers or any means to
  decrypt protected content.

The C++ that the recompiler produces is derived from the game's code. It is
generated locally, from each user's own copy, into a folder that Git ignores,
and it is never committed or distributed. The continuous integration builds
only the runtime, with no game data.

Decrypting the executable is done by a separate, publicly available tool
(RPCS3) on the user's own machine. This project ships no decryption code.

## You need your own copy of the game

The tools and the runtime here do nothing on their own. To use them you must
own a **legitimately purchased copy** of the game on its original disc and
make your own dump of it for personal use, in compliance with the laws of
your country. Do not ask for, share or link to game files, firmware or keys
in issues or pull requests: such content will be removed.

This project does not condone or support piracy in any form.

## Trademarks and affiliation

*Sly Cooper* and *Sly Cooper: Thieves in Time* are trademarks of Sony
Interactive Entertainment LLC. The game was developed by Sanzaru Games.
*PlayStation*, *PS3* and *Cell Broadband Engine* are trademarks of their
respective owners. All trademarks belong to their owners and are used here
only to identify the software being studied.

This project is **not affiliated with, endorsed by or sponsored by** Sony
Interactive Entertainment, Sanzaru Games or any of their partners.

## Third-party references

Some techniques follow the public source code and documentation of the RPCS3
emulator and of the GNU binutils, which are credited where they are used.
Library and format names (for example PlayStation EDGE, Havok, Bink, SPURS)
are mentioned only to describe how the game's data and code are structured.

## No warranty, and a contact for concerns

The software is provided "as is", without warranty of any kind (see
[LICENSE](LICENSE)). This notice is a statement of the project's intent and
practices, not legal advice.

If you are a rights holder and believe something in this repository should
not be here, please open an issue or contact the author through GitHub, and
it will be reviewed and removed promptly.
