# GNU binutils for the Cell PPU (powerpc64): objdump -M cell knows the Cell-only VMX
# instructions (lvlx/lvrx...) that Ghidra and capstone cannot decode. Same 2.35 source as tools/spu.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates wget xz-utils texinfo bison flex python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
RUN wget -q https://ftp.gnu.org/gnu/binutils/binutils-2.35.tar.xz \
    && tar xf binutils-2.35.tar.xz \
    && cd binutils-2.35 \
    && ./configure --target=powerpc64-linux --prefix=/opt/ppc --disable-nls --disable-werror \
       --disable-gdb --disable-sim --disable-gprof --disable-ld --disable-gas \
    && make -j"$(nproc)" all-binutils \
    && make install-binutils \
    && cd .. && rm -rf binutils-2.35 binutils-2.35.tar.xz
ENV PATH=/opt/ppc/bin:$PATH
WORKDIR /work
