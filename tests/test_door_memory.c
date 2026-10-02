/* Door-mode host/guest boundary regressions and deterministic interrupt fuzzing.
 * Include the real dispatcher so all four interrupt bodies run exactly its
 * normal DOS/BIOS routing. Only translated programs and terminal I/O are stubs;
 * the file layer, guest memory, loader, BIOS and modem are production code. */
#define _GNU_SOURCE
#include "../runtime/rt.c"
#include "dos_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static const uint8_t vc_bytes[] = {0xcd, 0x20};
/* Larger than the space above a malicious F800h allocation, like a real EXE. */
static const uint8_t overlay_bytes[0x20000] = {0xcb};
const Image image_vc_com = {.name = "VC.COM", .bytes = vc_bytes, .size = sizeof vc_bytes};
const Image image_vc_ovl = {
    .name = "VC.OVL", .is_exe = 1, .bytes = overlay_bytes, .size = sizeof overlay_bytes,
    .hdr_sp = 0x100, .min_alloc = 0x10, .max_alloc = 0x10,
};
const Image image_gwbasic = {.name = "GWBASIC.EXE"};
const Image image_bootlogo = {.name = "BOOTLOGO.COM"};
const Image image_rogue = {.name = "ROGUE.EXE"};
const Image image_vz = {.name = "VZ.COM"};
const Image image_kermit = {.name = "KERMIT.EXE"};
const Image image_command = {.name = "COMMAND.COM"};
const Image image_edlin = {.name = "EDLIN.COM"};
const Image image_debug = {.name = "DEBUG.COM"};
const Image image_find = {.name = "FIND.EXE"};
const Image image_more = {.name = "MORE.COM"};
const Image image_sort = {.name = "SORT.EXE"};
const Image image_fc = {.name = "FC.EXE"};
const Image image_vc405 = {.name = "VC405.COM"};
const Image image_vcsetup405 = {.name = "VCSETUP.COM"};
const EmbeddedFile embedded_files[] = {{"VC.COM", vc_bytes, sizeof vc_bytes}};
const int embedded_file_count = 1;

void term_idle(int milliseconds) {
    /* A blocking keyboard/line read gets Enter, not an infinite fake wait. */
    if (milliseconds > 0) (void)bios_key_push(0x1c0d);
}
void term_shutdown(void) {}
void term_suspend(void) { CHECK(!"door fuzz must not invoke a host program"); }
void term_resume(void) {}
void term_render(void) {}
void term_invalidate(void) {}
void term_bell(void) {}
void term_clear_pending(void) {}
void term_flush_input(void) {}

static void setup(const char *root) {
    rt_set_logging(0);
    dos_core_init();
    dos_core_set_door(1);
    CHECK(!dos_fs_init_door(root, UINT64_C(8) * 1024 * 1024));
    bios_init();
    modem_init(NULL);
    modem_set_door(1);
    CHECK(!dos_door_start(NULL));
}

static void loader_boundary(void) {
    /* A caller can edit the List of Lists and each MCB directly. Forge a
     * free allocation near the end of guest RAM, then use ordinary EXEC. */
    wr16(0x70, 0x24, 0xf800);
    wr8(0xf800, 0, 'Z');
    wr16(0xf800, 1, 0);
    wr16(0xf800, 3, 0x7000);
    /* Only the installed modern overlay has the built-in reload shortcut;
     * an arbitrary same-named file in the current directory does not. */
    memcpy(mem + lin(0x2000, 0), "H:\\.VC\\VC.OVL", sizeof "H:\\.VC\\VC.OVL");
    memset(mem + lin(0x2000, 0x100), 0, 16);
    wr16(0x2000, 0x102, 0x200);
    wr16(0x2000, 0x104, 0x2000);
    memset(mem + lin(0x2000, 0x200), 0, 128);
    cpu.a.x = 0x4b00;
    cpu.ds = cpu.es = 0x2000;
    cpu.d.x = 0;
    cpu.b.x = 0x100;
    hle_redirect = 0;
    (void)do_int(0x21);
    CHECK(cpu.cf && (cpu.a.x == 7 || cpu.a.x == 8));
    CHECK(!hle_redirect);
    puts("door memory: forged high MCB EXEC refused without a host overflow");
}

static void overlapping_psp(void) {
    cpu.a.x = 0x5000;
    cpu.b.x = 0x2000;
    (void)do_int(0x21);
    uint8_t before[256];
    for (unsigned i = 0; i < sizeof before; ++i)
        before[i] = mem[lin(0x2000, (uint16_t)i)] = (uint8_t)i;
    cpu.a.x = 0x5500;
    cpu.d.x = 0x2001; /* An overlapping, not identical, PSP destination. */
    cpu.si = 0x4000;
    (void)do_int(0x21);
    for (unsigned i = 0; i < sizeof before; ++i)
        if (i != 2 && i != 3 && i != 0x16 && i != 0x17 && !(i >= 0x32 && i <= 0x37))
            CHECK(mem[lin(0x2001, (uint16_t)i)] == before[i]);
    CHECK(rd16(0x2001, 0x32) == 20);
    CHECK(rd16(0x2001, 0x34) == 0x18 && rd16(0x2001, 0x36) == 0x2001);
    puts("door memory: overlapping AH=55h PSP copy is safe");
}

static void cyclic_mcb(void) {
    alarm(2);
    wr16(0x70, 0x24, 0x2000);
    wr8(0x2000, 0, 'M');
    wr16(0x2000, 1, 1);
    wr16(0x2000, 3, 0xffff); /* next wraps straight back to this MCB. */
    cpu.a.x = 0x4800;
    cpu.b.x = 1;
    (void)do_int(0x21);
    CHECK(cpu.cf && cpu.a.x == 7);
    puts("door memory: cyclic MCB refused without an unbounded native walk");
}

static void merged_wrap(void) {
    /* Three free 'M' blocks: m's next is a; a and b are 7FFFh paragraphs and
     * b sits at a + 8000h. Merging m, a and b wraps m's 16-bit size back to
     * a, which is free again: the walk would never reach a 'Z'. */
    alarm(2);
    const uint16_t m = 0x1000, a = 0x2000, b = a + 0x8000;
    wr16(0x70, 0x24, m);
    wr8(m, 0, 'M'); wr16(m, 1, 0); wr16(m, 3, (uint16_t)(a - m - 1));
    wr8(a, 0, 'M'); wr16(a, 1, 0); wr16(a, 3, 0x7fff);
    wr8(b, 0, 'M'); wr16(b, 1, 0); wr16(b, 3, 0x7fff);
    cpu.a.x = 0x4800; /* the merge every allocation starts with */
    cpu.b.x = 1;
    (void)do_int(0x21);
    CHECK(cpu.cf && cpu.a.x == 7);
    cpu.a.x = 0x4c00; /* SYSTEM's path: terminate frees and merges too */
    (void)do_int(0x21);
    puts("door memory: merged MCB size that wraps 64 KiB refused as a broken chain");
}

static uint64_t rng = UINT64_C(0x29d00f5afe123456);
static uint32_t random32(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static unsigned below(unsigned n) { return random32() % n; }

static void random_memory(void) {
    for (size_t at = 0; at < MEM_SIZE; at += sizeof(uint32_t)) {
        uint32_t value = random32();
        memcpy(mem + at, &value, sizeof value);
    }
}

/* Uniform registers almost never form a usable DOS request. Half of all
 * values come from the boundaries where a native copy could cross MEM_SIZE
 * or a 64 KiB segment, or point at the planted strings and blocks below. */
enum { PLANT = 0xe000 }; /* above conventional RAM, below the BIOS stubs */
static const char *const plants[] = {
    "h:/.vc/VC.OVL", "H:\\.VC\\VC.OVL", "H:\\.VC\\VC.COM", "H:\\FUZZ.TXT", "H:\\FUZZDIR",
    "H:\\FUZZDIR\\A.B", "H:\\*.*", "*.*", "H:\\", "H:\\GAMES\\..\\FUZZ2.TXT",
    "H:\\COMMAND.COM", "????????.???",
};
enum { PLANT_COUNT = sizeof plants / sizeof *plants, PLANT_STRIDE = 0x40 };

static uint16_t word(void) {
    static const uint16_t edges[] = {
        0, 1, 2, 3, 4, 5, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000,
        0xff00, 0xfff0, 0xfffe, 0xffff,
    };
    return below(2) ? (uint16_t)random32() : edges[below(sizeof edges / sizeof *edges)];
}

static uint16_t segment(void) {
    static const uint16_t edges[] = {
        0, 0x40, 0x70, 0x9fff, 0xa000, 0xb800, 0xf000, 0xf800, 0xfff0, 0xfffe, 0xffff,
    };
    unsigned pick = below(4);
    if (pick == 0) return (uint16_t)random32();
    if (pick == 1) return edges[below(sizeof edges / sizeof *edges)];
    if (pick == 2) return (uint16_t)(0xf000 + below(0x1000));
    return PLANT;
}

static void plant_strings(void) {
    for (unsigned i = 0; i < PLANT_COUNT; ++i)
        memcpy(mem + lin(PLANT, (uint16_t)(i * PLANT_STRIDE)), plants[i], strlen(plants[i]) + 1);
    /* EXEC parameter block: inherited environment, tail at PLANT:0400. */
    uint16_t block = PLANT_COUNT * PLANT_STRIDE;
    wr16(PLANT, block, 0);
    wr16(PLANT, (uint16_t)(block + 2), 0x400);
    wr16(PLANT, (uint16_t)(block + 4), PLANT);
    memcpy(mem + lin(PLANT, 0x400), "\x03 /x\r", 5);
    wr8(PLANT, 0x500, 0x7f); /* AH=0Ah: maximum length */
}

/* Rewrite the MCB chain the way a hostile program can: a high first block,
 * sizes that run past 1 MiB or wrap the 16-bit segment, free or owned. Half
 * the time build an ordinary free arena instead, so EXEC really loads. */
static void forge_chain(void) {
    if (below(2)) {
        uint16_t m = (uint16_t)(0x100 + below(0x6000));
        wr16(0x70, 0x24, m);
        wr8(m, 0, 'Z');
        wr16(m, 1, 0);
        wr16(m, 3, (uint16_t)(0xa000 - m - 1));
        return;
    }
    uint16_t m = below(2) ? (uint16_t)(0xf000 + below(0x1000)) : (uint16_t)random32();
    const uint16_t first = m;
    wr16(0x70, 0x24, m);
    unsigned blocks = 1 + below(4);
    int cycle = !below(8); /* the last block's size wraps back to the first */
    for (unsigned i = 0; i < blocks; ++i) {
        uint16_t size = below(2) ? word() : (uint16_t)below(0x2000);
        if (cycle && i + 1 == blocks) size = (uint16_t)(first - m - 1);
        wr8(m, 0, i + 1 == blocks && !cycle ? 'Z' : 'M');
        wr16(m, 1, below(2) ? 0 : word());
        wr16(m, 3, size);
        m = (uint16_t)(m + 1 + size);
    }
}

/* Children this fuzz loaded, so it can also exercise normal termination
 * instead of only ever nesting up to the process table's limit. */
static uint16_t children[8];
static unsigned depth;

static void end_child(void) {
    Cpu saved = cpu;
    cpu.a.x = 0x5000;
    cpu.b.x = children[--depth];
    (void)do_int(0x21);
    cpu.a.x = 0x4c00;
    hle_redirect = 0;
    (void)do_int(0x21);
    cpu = saved;
}

static uint8_t dos_function(void) {
    /* Every INT 21h function that passes a guest address or length. */
    static const uint8_t memory[] = {
        0x09, 0x0a, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1a,
        0x21, 0x22, 0x23, 0x24, 0x26, 0x27, 0x28, 0x29, 0x39, 0x3a, 0x3b, 0x3c,
        0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
        0x49, 0x4a, 0x4b, 0x4c, 0x4e, 0x4f, 0x50, 0x55, 0x56, 0x57, 0x5a, 0x5b,
        0x60, 0x65, 0x6c, 0x71,
    };
    return below(2) ? (uint8_t)random32() : memory[below(sizeof memory / sizeof *memory)];
}

static unsigned fuzz_calls = 250000;

static void fuzz_interrupts(void) {
    const unsigned CALLS = fuzz_calls;
    const uint64_t seed = rng;
    unsigned calls[4] = {0}, execs = 0, loaded = 0, created = 0;
    static const uint8_t interrupts[] = {0x21, 0x10, 0x16, 0x14};
    random_memory();
    plant_strings();
    for (unsigned i = 0; i < CALLS; ++i) {
        if (i && i % 1024 == 0) { random_memory(); plant_strings(); }
        /* Keep state between calls, but expose every guest region to mutation. */
        for (unsigned j = 0; j < 256; ++j)
            mem[random32() % MEM_SIZE] = (uint8_t)random32();
        if (!below(16)) forge_chain();
        if (!below(64)) {
            /* A guest may also corrupt the BIOS video and keyboard fields. */
            mem[0x449] = (uint8_t)below(8);
            wr16(0x40, 0x4a, word());
            mem[0x484] = (uint8_t)random32();
        }
        cpu.a.x = word(); cpu.b.x = below(4) ? (uint16_t)below(24) : word();
        cpu.c.x = word(); cpu.d.x = word();
        cpu.si = word(); cpu.di = word(); cpu.bp = word(); cpu.sp = word();
        cpu.ds = segment(); cpu.es = segment();
        cpu.ss = (uint16_t)random32(); cpu.cs = (uint16_t)random32();
        cpu.ip = (uint16_t)random32();
        flags_set((uint16_t)random32());
        unsigned which = i % 4;
        if (which == 0) {
            cpu.a.h = below(16) ? dos_function() : 0x4b; /* the loader, often */
            if (cpu.a.h == 0x71) cpu.a.l = below(2) ? (uint8_t)random32() :
                (uint8_t)(0x39 + below(0x6d - 0x39));
            if (below(2)) { /* a planted path, string or parameter block */
                plant_strings(); /* a loaded child may have overwritten them */
                cpu.ds = cpu.es = PLANT;
                cpu.d.x = cpu.si = (uint16_t)(below(PLANT_COUNT) * PLANT_STRIDE);
                cpu.di = (uint16_t)(below(PLANT_COUNT) * PLANT_STRIDE);
                if (cpu.a.h == 0x4b) {
                    if (depth && below(2)) end_child();
                    cpu.a.l = 0;
                    cpu.b.x = PLANT_COUNT * PLANT_STRIDE;
                    forge_chain();
                }
                if (cpu.a.h == 0x0a) cpu.d.x = 0x500;
            }
        } else if (i & 4) cpu.a.h = (uint8_t)(i / 8);
        if (which == 3 && i % 8 == 7) {
            cpu.d.x = 0; /* Exercise COM1, not just the unknown-port return. */
            cpu.a.h &= 3;
        }
        hle_redirect = rt_exited = rt_halted = 0;
        uint8_t function = cpu.a.h;
        (void)do_int(interrupts[which]);
        ++calls[which];
        /* Coverage, so a fuzz that silently stops reaching the loader or
         * the file layer cannot pass by doing nothing. */
        if (which == 0 && function == 0x4b && hle_redirect && depth < 8)
            children[depth++] = cpu.ds; /* load_image sets DS to the new PSP */
        if (which == 0 && function == 0x4b) { ++execs; loaded += hle_redirect; }
        if (which == 0 && (function == 0x3c || function == 0x5b) && !cpu.cf) ++created;
    }
    printf("door memory fuzz: %u calls; INT 21h=%u 10h=%u 16h=%u 14h=%u; "
           "EXEC %u (%u loaded); files created %u; seed=%016llx\n",
           CALLS, calls[0], calls[1], calls[2], calls[3], execs, loaded, created,
           (unsigned long long)seed);
    /* Creation saturates at the 8 MiB and 4096-entry limits, so it is fixed. */
    CHECK(execs > CALLS / 125 && loaded > CALLS / 1250 && created > 25);
}

static int clean_fixture(int fd) {
    /* DOS attribute calls can leave any directory, the root too, read-only. */
    if (fchmod(fd, 0700)) return -1;
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *directory = copy < 0 ? NULL : fdopendir(copy);
    if (!directory) { if (copy >= 0) close(copy); return -1; }
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { result = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { result = -1; break; }
            result = clean_fixture(child);
            close(child);
            if (result || unlinkat(fd, entry->d_name, AT_REMOVEDIR)) { result = -1; break; }
        } else if (unlinkat(fd, entry->d_name, 0)) { result = -1; break; }
    }
    closedir(directory);
    return result;
}

int main(int argc, char **argv) {
    /* --fuzz [SEED [CALLS]]: the defaults are what make test runs. */
    const char *test = argc >= 2 ? argv[1] : "--fuzz";
    if (argc >= 3) rng = strtoull(argv[2], NULL, 16);
    if (argc >= 4) fuzz_calls = (unsigned)strtoul(argv[3], NULL, 10);
    CHECK(rng && fuzz_calls >= 1000 && argc <= 4);
    char root[] = "/tmp/vc-door-memory-XXXXXX";
    CHECK(mkdtemp(root));
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    fflush(NULL);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        alarm(120); /* A corrupt guest chain must not hang the test runner. */
        setup(root);
        if (!strcmp(test, "--loader")) loader_boundary();
        else if (!strcmp(test, "--overlap")) overlapping_psp();
        else if (!strcmp(test, "--cycle")) cyclic_mcb();
        else if (!strcmp(test, "--merge-wrap")) merged_wrap();
        else { CHECK(!strcmp(test, "--fuzz")); fuzz_interrupts(); }
        exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(!clean_fixture(fd));
    close(fd);
    CHECK(!rmdir(root));
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "door memory %s: FAILED (wait status %d)\n", test, status);
        return 1;
    }
    return 0;
}
