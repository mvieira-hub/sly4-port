# Oracle for the PPU interpreter: real PowerPC64 big-endian execution under QEMU.
# A generated C harness (tools/ppc/oracle/gen_cases.py) runs each test instruction
# with a given register state and dumps the resulting state; the recompiler's
# interpreter must reproduce it.
#   docker build -t sly4-ppc-oracle:1 tools/ppc/oracle
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        qemu-user gcc-powerpc64-linux-gnu libc6-dev-ppc64-cross python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
