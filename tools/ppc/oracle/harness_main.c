/* Oracle harness entry point (runs under qemu-ppc64). Reads the case records written by
 * "sly4_recomp oracle-gen", lets the generated run_all (harness.S) execute every case
 * and writes the resulting register state records.
 *   harness cases.bin results.bin
 * An instruction the emulated CPU rejects (SIGILL) is skipped and listed in illegal.txt
 * as "word", so the generator's choices can be checked; those cases are not comparable.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

extern void run_all(void* in, void* out);

static FILE* illegal;

static void on_signal(int sig, siginfo_t* info, void* uc_) {
    (void)info;
    ucontext_t* uc = (ucontext_t*)uc_;
    unsigned long pc = uc->uc_mcontext.regs->nip;
    fprintf(illegal, "%s %08x\n", sig == SIGILL ? "ill" : "fpe", *(uint32_t*)pc);
    uc->uc_mcontext.regs->nip = pc + 4;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "uso: harness cases.bin results.bin\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 3;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* in = aligned_alloc(16, (size_t)size);
    void* out = aligned_alloc(16, (size_t)size);
    if (!in || !out || fread(in, 1, (size_t)size, f) != (size_t)size) return 4;
    fclose(f);
    memcpy(out, in, (size_t)size);

    /* Memory window for load/store cases: the same guest address on both sides. */
    if (mmap((void*)0x20000000, 0x10000, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) !=
        (void*)0x20000000)
        return 6;
    illegal = fopen("illegal.txt", "w");
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    run_all(in, out);
    fclose(illegal);
    FILE* o = fopen(argv[2], "wb");
    if (!o || fwrite(out, 1, (size_t)size, o) != (size_t)size) return 5;
    fclose(o);
    return 0;
}
