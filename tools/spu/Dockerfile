# GNU binutils for the Cell SPU (spu-elf): objdump/readelf for the SPU programs
# embedded in EBOOT. 2.35 still ships the SPU port.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates wget xz-utils texinfo bison flex python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
RUN wget -q https://ftp.gnu.org/gnu/binutils/binutils-2.35.tar.xz \
    && tar xf binutils-2.35.tar.xz \
    && cd binutils-2.35 \
    && ./configure --target=spu-elf --prefix=/opt/spu --disable-nls --disable-werror \
       --disable-gdb --disable-sim --disable-gprof --disable-ld --disable-gas \
    && make -j"$(nproc)" all-binutils \
    && make install-binutils \
    && cd .. && rm -rf binutils-2.35 binutils-2.35.tar.xz
ENV PATH=/opt/spu/bin:$PATH
WORKDIR /work
