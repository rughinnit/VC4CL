/*
 * qrun - minimal VideoCore IV QPU launch harness for hand-written kernels.
 *
 * Allocates a CMA dma-buf, finds its physical base (signature scan through
 * /dev/mem, like VC4CL's DMAHeap), maps it uncached through /dev/mem and uses
 * only that mapping, so no ARM cache maintenance is needed.
 *
 * Loads a raw QPU program plus per-QPU uniform blocks, launches user programs
 * through the V3D request queue, waits with a timeout, reads the performance
 * counters and the result area.
 *
 * Usage: qrun <code.bin> <num_qpus> <n_iter> <out_words_per_qpu> [out_base_off]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define V3D_PHYS_BASE 0x3FC00000UL
#define V3D_LENGTH    0x4000UL

#define V3D_IDENT0         (0x0000 / 4)
#define V3D_L2CACTL        (0x0020 / 4)
#define V3D_SLCACTL        (0x0024 / 4)
#define V3D_SRQPC          (0x0430 / 4)
#define V3D_SRQUA          (0x0434 / 4)
#define V3D_SRQCS          (0x043C / 4)
#define V3D_VPMBASE        (0x0504 / 4)
#define V3D_COUNTER_CLEAR  (0x0670 / 4)
#define V3D_COUNTER_ENABLE (0x0674 / 4)
#define V3D_COUNTER_VALUE  (0x0680 / 4)
#define V3D_COUNTER_MAP    (0x0684 / 4)
#define V3D_ERRORS         (0x0F20 / 4)

#define CTR_EXEC_CYCLES 16
#define CTR_INSTR_HITS  20

struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

#define ARENA_SIZE (16 * 1024 * 1024)
#define AB_A_OFF 0x60000u
#define AB_B_OFF 0xA0000u
/* k-major packed A panel: PANEL[(rb*N + k)*16 + r] = A[rb*16 + r][k] */
#define PANEL_OFF 0x100000u
/* per-QPU scratch panel (in-kernel packing variant), one 16-row block each */
#define PANELQ_OFF 0x200000u

static volatile uint32_t *v3d;
static uint8_t *gpu;
static uint32_t gpu_bus = 0, gpu_phys = 0;
static int dmabuf_fd = -1;

static uint32_t xs_state = 123456789u;
static uint32_t xs_next(void)
{
    uint32_t x = xs_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xs_state = x;
    return x;
}

/* value in [-1,1), exactly representable in fp32 (23-bit integer / 2^22) */
static float xs_float(void)
{
    int32_t v = (int32_t)((xs_next() >> 9) & 0x7FFFFFu) - (1 << 22);
    return (float)v / 4194304.0f;
}

static void save_words(const char *path, uint32_t off, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "qrun: cannot write %s\n", path);
        return;
    }
    fwrite(gpu + off, 4, n, f);
    fclose(f);
}

static void die(const char *msg)
{
    fprintf(stderr, "qrun: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void *map_phys(uint32_t phys, size_t len, int prot)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        die("open /dev/mem");
    void *p = mmap(NULL, len, prot, MAP_SHARED, fd, (off_t)phys);
    close(fd);
    if (p == MAP_FAILED)
        die("mmap /dev/mem");
    return p;
}

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void barrier(void)
{
    __sync_synchronize();
    asm volatile("dsb sy" ::: "memory");
}

static void cache_clean(void *addr, size_t bytes)
{
    uintptr_t line = (uintptr_t)addr & ~63ul;
    for (; line < (uintptr_t)addr + bytes; line += 64)
        asm volatile("dc civac, %0" : : "r"(line) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

/* allocate a CMA dma-buf and locate its physical base */
static void gpu_alloc(void)
{
    static const char magic[16] = "QRUN-V3D-ARENA!";
    int heap = open("/dev/dma_heap/linux,cma", O_RDWR | O_CLOEXEC);
    if (heap < 0)
        die("open dma_heap");
    struct dma_heap_allocation_data a;
    memset(&a, 0, sizeof(a));
    a.len = ARENA_SIZE;
    a.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) != 0)
        die("dma_heap alloc");
    close(heap);
    dmabuf_fd = (int)a.fd;

    void *cached = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (cached == MAP_FAILED)
        die("mmap dmabuf");
    memcpy(cached, magic, sizeof(magic));
    cache_clean(cached, 64);
    __sync_synchronize();

    /* scan physical RAM page by page for the signature */
    int mem = open("/dev/mem", O_RDONLY | O_SYNC);
    if (mem < 0)
        die("open /dev/mem ro");
    uint32_t *window = malloc(1 << 20);
    if (!window)
        die("malloc");
    uint32_t hit = 0;
    const uint32_t limit = 0x3B400000u; /* below the firmware/GPU heap */
    for (uint32_t base = 0; base < limit && !hit; base += (1u << 20)) {
        ssize_t got = pread(mem, window, 1 << 20, (off_t)base);
        if (got <= 0)
            continue;
        for (ssize_t off = 0; off + 16 <= got; off += 4096) {
            if (memcmp((char *)window + off, magic, sizeof(magic)) == 0) {
                hit = base + (uint32_t)off;
                break;
            }
        }
    }
    free(window);
    close(mem);
    if (!hit) {
        fprintf(stderr, "qrun: could not locate the dma-buf in physical RAM\n");
        exit(1);
    }
    gpu_phys = hit;
    gpu_bus = gpu_phys | 0xC0000000u;
    munmap(cached, ARENA_SIZE); /* never touched again */
    gpu = map_phys(gpu_phys, ARENA_SIZE, PROT_READ | PROT_WRITE);
    fprintf(stderr, "arena: dmabuf fd=%d phys=0x%08x bus=0x%08x host=%p\n",
            dmabuf_fd, gpu_phys, gpu_bus, gpu);
}

#define IOCTL_MBOX_PROPERTY _IOWR(100, 0, char *)

static void mbox_enable_qpu(void)
{
    int fd = open("/dev/vcio", O_RDWR);
    if (fd < 0)
        die("open /dev/vcio");
    unsigned msg[7] = {0};
    msg[0] = sizeof(msg);
    msg[2] = 0x00030012; /* ENABLE_QPU */
    msg[3] = 1 * 4;
    msg[5] = 1;
    if (ioctl(fd, IOCTL_MBOX_PROPERTY, msg) < 0)
        die("mailbox enable qpu");
    close(fd);
}

static void ctr_enable(int idx, unsigned type)
{
    v3d[V3D_COUNTER_ENABLE] |= (1u << 31) | (1u << idx);
    v3d[V3D_COUNTER_MAP + idx * 2] = type & 0x1F;
    v3d[V3D_COUNTER_VALUE + idx * 2] = 0;
}

static uint32_t ctr_read(int idx)
{
    return v3d[V3D_COUNTER_VALUE + idx * 2];
}

static int srq_reset(void)
{
    const uint32_t RESET = (1u << 0) | (1u << 7) | (1u << 8) | (1u << 16);
    for (int attempt = 0; attempt < 20; ++attempt) {
        v3d[V3D_SRQCS] = RESET;
        barrier();
        uint32_t s = v3d[V3D_SRQCS];
        if ((s & 0x3F) == 0 && ((s >> 16) & 0xFF) == 0 && (s & (1u << 7)) == 0)
            return 1;
        usleep(100);
    }
    return 0;
}

static const char *state_str(void)
{
    static char buf[64];
    uint32_t s = v3d[V3D_SRQCS];
    snprintf(buf, sizeof(buf), "queue=%u err=%u done=%u",
             s & 0x3F, (s >> 7) & 1, (s >> 16) & 0xFF);
    return buf;
}

/* ---------------------------------------------------------------------------
 * One program launch.  code + uniform blocks + QPU list all live in the same
 * dma-buf, so several programs can be run against the same arena in one
 * process (that is what makes "pack, then GEMM" measurable as one unit).
 * ------------------------------------------------------------------------- */
#define UNI_OFF 0x8000u      /* uniform blocks (16 words each) */
#define QLIST_OFF 0x9000u    /* SRQPC/SRQUA pairs, filled from the uniform file */

struct lres {
    uint64_t wall_ns;
    uint32_t cycles, instrs, errs, code_words;
    int done;
};

static int launch_prog(const char *path, int nqpu, const uint32_t (*uni)[8],
                       int do_flush, uint32_t code_off, uint32_t uni_off,
                       struct lres *r)
{
    static uint8_t code[8192];
    FILE *f = fopen(path, "rb");
    if (!f)
        die("open code.bin");
    size_t code_len = fread(code, 1, sizeof(code), f);
    fclose(f);
    if (code_len == 0 || (code_len % 8) != 0) {
        fprintf(stderr, "qrun: bad code size %zu for %s\n", code_len, path);
        return -1;
    }
    if (nqpu < 1 || nqpu > 12) {
        fprintf(stderr, "qrun: num_qpus must be 1..12\n");
        return -1;
    }
    r->code_words = (uint32_t)(code_len / 8);
    /* each program gets its own code area: a fresh code block at the same
       address can be shadowed by the stale (clean, but not dirty) L2 line of
       the previous program, and the QPUs would run the old code */
    memcpy(gpu + code_off, code, code_len);
    uint32_t code_bus = gpu_bus + code_off;

    for (int i = 0; i < nqpu; ++i) {
        uint32_t *u = (uint32_t *)(gpu + uni_off + 64 * i);
        for (int j = 0; j < 8; ++j)
            u[j] = uni[i][j];
    }
    barrier();

    ctr_enable(0, CTR_EXEC_CYCLES);
    ctr_enable(1, CTR_INSTR_HITS);
    v3d[V3D_COUNTER_CLEAR] = 3;
    barrier();
    v3d[V3D_VPMBASE] = 16;
    barrier();
    if (do_flush) {
        v3d[V3D_L2CACTL] = 1u << 2;
    }
    /* always invalidate the slice caches: the instruction cache must not serve
       the previous program's code after the code area has been overwritten */
    v3d[V3D_SLCACTL] = 0xFFFFFFFFu;
    barrier();
    v3d[V3D_ERRORS] = 0xFFFFFFFFu;     /* ERRSTAT is write-1-to-clear */
    barrier();
    if (!srq_reset()) {
        printf("LAUNCH_FAIL reset %s\n", state_str());
        return -1;
    }
    for (int i = 0; i < nqpu; ++i) {
        v3d[V3D_SRQUA] = gpu_bus + uni_off + 64 * i;
        v3d[V3D_SRQPC] = code_bus;
    }
    barrier();
    uint32_t s = v3d[V3D_SRQCS];
    if (s & (1u << 7)) {
        printf("LAUNCH_FAIL queueerr %s\n", state_str());
        return -1;
    }
    uint64_t t0 = now_ns();
    r->done = 0;
    uint64_t limit = 20000000000ull;
    if (getenv("QRUN_TMO_MS")) limit = strtoull(getenv("QRUN_TMO_MS"), NULL, 0) * 1000000ull;
    while (1) {
        s = v3d[V3D_SRQCS];
        if (((s >> 16) & 0xFF) == (uint32_t)nqpu && (s & 0x3F) == 0 && !(s & (1u << 7))) {
            r->done = 1;
            break;
        }
        if (now_ns() - t0 > limit)
            break;
    }
    r->wall_ns = now_ns() - t0;
    r->cycles = ctr_read(0);
    r->instrs = ctr_read(1);
    r->errs = v3d[V3D_ERRORS];
    if (!r->done) {
        printf("TMO_STATS exec_cycles=%u instr_hits=%u\n", r->cycles, r->instrs);
        printf("TIMEOUT %s (queue left dirty: a stuck QPU cannot be reset from the ARM, reboot to recover)\n",
               state_str());
        v3d[V3D_SRQCS] = (1u << 0) | (1u << 7) | (1u << 8) | (1u << 16);
    }
    return r->done ? 0 : -1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 5) {
        fprintf(stderr, "usage: %s <code.bin> <num_qpus> <n_iter> <out_words_per_qpu> [out_base_off]\n", argv[0]);
        return 2;
    }
    const char *code_path = argv[1];
    int nqpu = atoi(argv[2]);
    uint32_t niter = (uint32_t)strtoul(argv[3], NULL, 0);
    int out_words = atoi(argv[4]);
    uint32_t out_off = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 0x10000;

    if (nqpu < 1 || nqpu > 12) {
        fprintf(stderr, "qrun: num_qpus must be 1..12\n");
        return 2;
    }

    gpu_alloc();
    mbox_enable_qpu();
    v3d = map_phys(V3D_PHYS_BASE, V3D_LENGTH, PROT_READ | PROT_WRITE);
    uint32_t ident0 = v3d[V3D_IDENT0];
    uint32_t gemm_N = 0;
    uint32_t uni[12][8];
    memset(uni, 0, sizeof(uni));

    memset(gpu + out_off, 0xCD, (size_t)out_words * 4 * nqpu);

    /* ---- GEMM mode: deterministic A/B in the arena, C base shared by all QPUs */
    const char *gem = getenv("QRUN_GEMM");
    if (gem) {
        gemm_N = (uint32_t)strtoul(gem, NULL, 0);
        uint32_t nn = gemm_N * gemm_N;
        /* flush the L2 *before* the CPU fill, so no stale dirty line can
           write back over it later */
        v3d[V3D_L2CACTL] = 1u << 2;
        barrier();
        for (uint32_t i = 0; i < nn; ++i) {
            ((float *)(gpu + AB_A_OFF))[i] = xs_float();
            ((float *)(gpu + AB_B_OFF))[i] = xs_float();
            ((uint32_t *)(gpu + out_off))[i] = 0;
        }
        barrier();
        if (getenv("QRUN_CPUPANEL")) {
            /* reference pack done on the ARM, to validate the QPU packer */
            float *A = (float *)(gpu + AB_A_OFF);
            float *P = (float *)(gpu + PANEL_OFF);
            uint32_t ncol = gemm_N / 16;
            for (uint32_t rb = 0; rb < ncol; ++rb)
                for (uint32_t k = 0; k < gemm_N; ++k)
                    for (uint32_t r = 0; r < 16; ++r)
                        P[(rb * gemm_N + k) * 16 + r] = A[(rb * 16 + r) * gemm_N + k];
            barrier();
        }
        save_words(getenv("QRUN_SAVE_A") ? getenv("QRUN_SAVE_A") : "/tmp/a.bin", AB_A_OFF, nn);
        save_words(getenv("QRUN_SAVE_B") ? getenv("QRUN_SAVE_B") : "/tmp/b.bin", AB_B_OFF, nn);
        fprintf(stderr, "gemm: N=%u  A=0x%08x B=0x%08x C=0x%08x P=0x%08x\n",
                gemm_N, gpu_bus + AB_A_OFF, gpu_bus + AB_B_OFF, gpu_bus + out_off,
                gpu_bus + PANEL_OFF);
    }
    /* main-program uniforms (GEMM kernel, or whatever argv[1] is) */
    for (int i = 0; i < nqpu; ++i) {
        uni[i][0] = niter;
        uni[i][1] = (getenv("QRUN_OUT_PHYS") ? gpu_phys : gpu_bus) + out_off
                    + (uint32_t)i * (uint32_t)out_words * 4;
        if (gemm_N) {
            uni[i][0] = gemm_N;
            uni[i][1] = gpu_bus + out_off;
            uni[i][2] = gpu_bus + AB_A_OFF;
            uni[i][3] = gpu_bus + AB_B_OFF;
            uni[i][4] = gpu_bus + PANEL_OFF;
            uni[i][5] = (uint32_t)i;
            uni[i][6] = (uint32_t)nqpu;
        }
    }

    /* optional self-check fill: word i = 0x3F800000 + i (all normal floats) */
    if (getenv("QRUN_FILL_WORDS")) {
        uint32_t n = (uint32_t)strtoul(getenv("QRUN_FILL_WORDS"), NULL, 0);
        uint32_t *o = (uint32_t *)(gpu + out_off);
        for (uint32_t i = 0; i < n; ++i)
            o[i] = 0x3F800000u + i;
        barrier();
        fprintf(stderr, "fill: %u words at out_off, o[0]=0x%08x o[1]=0x%08x o[16]=0x%08x\n",
                n, o[0], o[1], o[16]);
    }

    /* optional pack pre-pass (same arena, second launch): QRUN_PACK_BIN */
    struct lres rp, rg;
    int do_flush_first = getenv("QRUN_NOFLUSH") ? 0 : 1;
    const char *pack_bin = getenv("QRUN_PACK_BIN");
    if (pack_bin && gemm_N) {
        int pnq = getenv("QRUN_PACK_NQPU") ? atoi(getenv("QRUN_PACK_NQPU")) : 2;
        uint32_t ncol = gemm_N / 16;
        if (pnq < 1 || ncol % (uint32_t)pnq) {
            fprintf(stderr, "qrun: pack nqpu must divide N/16\n");
            return 2;
        }
        uint32_t per = ncol / (uint32_t)pnq;
        uint32_t up[12][8];
        memset(up, 0, sizeof(up));
        for (int i = 0; i < pnq; ++i) {
            up[i][0] = gemm_N;
            up[i][1] = (uint32_t)i * per * 64u * gemm_N;   /* rb0 * 64*N */
            up[i][2] = gpu_bus + AB_A_OFF;
            up[i][3] = 64u * gemm_N;                       /* row-block byte stride */
            up[i][4] = gpu_bus + PANEL_OFF;
            up[i][5] = per;                                /* row blocks this QPU */
            up[i][6] = (uint32_t)i;                        /* VPM row base 16*i */
        }
        fprintf(stderr, "pack: %s N=%u nqpu=%d per_qpu=%u\n", pack_bin, gemm_N, pnq, per);
        if (launch_prog(pack_bin, pnq, (const uint32_t (*)[8])up, do_flush_first,
                        0x0000, 0x8000, &rp) != 0)
            return 5;
        printf("PACK wall_ns=%llu exec_cycles=%u instr_hits=%u v3d_errors=0x%08x %s\n",
               (unsigned long long)rp.wall_ns, rp.cycles, rp.instrs, rp.errs, state_str());
        if (!getenv("QRUN_KEEP_L2"))
            do_flush_first = 1;  /* write the packed panel back and start clean */
    }

    if (launch_prog(code_path, nqpu, (const uint32_t (*)[8])uni, do_flush_first,
                    0x4000, 0x8800, &rg) != 0)
        return 5;

    printf("BUS=0x%08x PHYS=0x%08x IDENT0=0x%08x V3D=%s\n", gpu_bus, gpu_phys, ident0,
           ident0 == 0xdeadbeefu ? "SUSPENDED" : "LIVE");
    printf("QPUS=%d ITER=%u CODE_WORDS=%u\n", nqpu, niter, rg.code_words);
    printf("DONE wall_ns=%llu exec_cycles=%u instr_hits=%u v3d_errors=0x%08x %s\n",
           (unsigned long long)rg.wall_ns, rg.cycles, rg.instrs, rg.errs, state_str());
    if (rg.cycles && rg.wall_ns)
        printf("CLOCK_MHZ=%.2f\n",
               (double)rg.cycles / nqpu / ((double)rg.wall_ns * 1e-9) / 1e6);


    for (int i = 0; i < nqpu; ++i) {
        uint32_t *o = (uint32_t *)(gpu + out_off + (size_t)i * out_words * 4);
        printf("OUT%d:", i);
        for (int j = 0; j < out_words; ++j)
            printf(" %08x", o[j]);
        printf("\n");
    }
    if (getenv("QRUN_SETTLE")) {
        usleep(200000);
        v3d[V3D_L2CACTL] = 1u << 2;
        barrier();
        for (int i = 0; i < nqpu; ++i) {
            uint32_t *o = (uint32_t *)(gpu + out_off + (size_t)i * out_words * 4);
            printf("AFTER%d:", i);
            for (int j = 0; j < out_words; ++j)
                printf(" %08x", o[j]);
            printf("\n");
        }
    }

    if (gemm_N && getenv("QRUN_SAVE_PANEL")) {
        usleep(2000);
        v3d[V3D_L2CACTL] = 1u << 2;
        barrier();
        save_words(getenv("QRUN_SAVE_PANEL"), PANEL_OFF, gemm_N * gemm_N);
    }
    if (gemm_N && getenv("QRUN_SAVE_C")) {
        usleep(2000);
        v3d[V3D_L2CACTL] = 1u << 2;
        barrier();
        save_words(getenv("QRUN_SAVE_C"), out_off, gemm_N * gemm_N);
    }
    uint32_t srq = v3d[V3D_SRQCS];
    printf("STATE: %s SRQCS=0x%08x\n", ident0 == 0xdeadbeefu ? "SUSPENDED" : "LIVE", srq);
    return 0;
}
