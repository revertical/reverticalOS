/* Module loader for RVLB libraries and RVEX executables (x86-64, v2). */

#include "rvmod.h"
#include "rvfs.h"
#include "string.h"
#include "serial.h"
#include "timer.h"

extern void rvmod_trampoline(uintptr_t entry, uintptr_t stack_top);

typedef struct {
    char name[28];
    uintptr_t address;
} Sym;

static u8 arena[RVMOD_ARENA] __attribute__((aligned(16)));
static u32 arena_used;

static Sym syms[RVMOD_MAX_SYMS];
static int sym_count;

static char module_names[8][20];
static int module_count;
static char lib_names[8][20];
static int lib_count;

static char error[80];
static int last_exit;

static void set_error(const char *text)
{
    int i = 0;
    while (text[i] && i < (int)sizeof(error) - 1) {
        error[i] = text[i];
        i++;
    }
    error[i] = 0;
}

const char *rvmod_error(void) { return error; }
int rvmod_exit_code(void) { return last_exit; }
int rvmod_module_count(void) { return module_count; }
const char *rvmod_module_name(int index)
{
    return (index >= 0 && index < module_count) ? module_names[index] : "";
}

static void add_symbol(const char *name, uintptr_t address)
{
    if (sym_count >= RVMOD_MAX_SYMS)
        return;
    int i = 0;
    while (name[i] && i < RVMOD_NAME_MAX) {
        syms[sym_count].name[i] = name[i];
        i++;
    }
    syms[sym_count].name[i] = 0;
    syms[sym_count].address = address;
    sym_count++;
}

static uintptr_t find_symbol(const char *name)
{
    for (int i = 0; i < sym_count; i++)
        if (strcmp(syms[i].name, name) == 0)
            return syms[i].address;
    return 0;
}

static int read_exact(const char *path, u32 offset, void *buf, u32 len)
{
    return rvfs_read(path, offset, buf, len) == (int)len ? 0 : -1;
}

static uintptr_t alloc_image(u32 size)
{
    u32 base = arena_used;
    if (base + size > RVMOD_ARENA)
        return 0;
    arena_used = (base + size + 15u) & ~15u;
    return (uintptr_t)arena + base;
}

static u32 rd_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void wr_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u64 rd_u64(const u8 *p)
{
    return (u64)rd_u32(p) | ((u64)rd_u32(p + 4) << 32);
}

static void wr_u64(u8 *p, u64 v)
{
    wr_u32(p, (u32)v);
    wr_u32(p + 4, (u32)(v >> 32));
}

static void rebase_field(u8 *p, u8 size, u64 add)
{
    if (size == 8)
        wr_u64(p, rd_u64(p) + add);
    else
        wr_u32(p, rd_u32(p) + (u32)add);
}

static int load_module(const char *path, int want_lib)
{
    RvfsStat st;
    if (rvfs_stat(path, &st) != RVFS_OK || st.type != 1) {
        set_error("Module not found");
        return -1;
    }
    if (st.size < sizeof(RvModuleHeader)) {
        set_error("Module file is too short");
        return -1;
    }

    RvModuleHeader hdr;
    if (read_exact(path, 0, &hdr, sizeof(hdr)) != 0) {
        set_error("Cannot read module header");
        return -1;
    }
    if (hdr.magic != RVMOD_MAGIC_LIB && hdr.magic != RVMOD_MAGIC_EXE) {
        set_error("Bad module magic");
        return -1;
    }
    if (hdr.version != RVMOD_VERSION) {
        set_error("Unsupported module version");
        return -1;
    }
    if (want_lib && hdr.magic != RVMOD_MAGIC_LIB) {
        set_error("Not a library");
        return -1;
    }
    if (!want_lib && hdr.magic != RVMOD_MAGIC_EXE) {
        set_error("Not an executable");
        return -1;
    }
    if (hdr.image_size == 0 || hdr.image_size > 512u * 1024u) {
        set_error("Module image is too large");
        return -1;
    }
    if (hdr.image_off < sizeof(hdr) ||
        hdr.image_off + hdr.image_size > st.size) {
        set_error("Truncated module image");
        return -1;
    }
    if (hdr.sym_count > 64 || hdr.reloc_count > 4096) {
        set_error("Module tables are too large");
        return -1;
    }

    u32 mark = arena_used;
    uintptr_t base = alloc_image(hdr.image_size);
    if (!base) {
        set_error("Module arena is full");
        return -1;
    }
    u8 *img = (u8 *)base;
    if (read_exact(path, hdr.image_off, img, hdr.image_size) != 0) {
        set_error("Cannot read module image");
        return -1;
    }

    u32 sum = 0;
    for (u32 i = 0; i < hdr.image_size; i++)
        sum += img[i];
    if (sum != hdr.checksum) {
        set_error("Module checksum mismatch");
        return -1;
    }

    /* Absolute fields that must move with the module. */
    if (hdr.reloc_count) {
        for (u32 i = 0; i < hdr.reloc_count; i++) {
            RvModuleReloc rel;
            u32 file_off = (u32)sizeof(RvModuleHeader) +
                           i * (u32)sizeof(RvModuleReloc);
            if (read_exact(path, file_off, &rel, sizeof(rel)) != 0) {
                set_error("Cannot read relocation table");
                return -1;
            }
            u8 size = rel.size == 8 ? 8 : 4;
            if (rel.offset + size > hdr.image_size) {
                set_error("Relocation out of range");
                return -1;
            }
            rebase_field(img + rel.offset, size, base);
        }
    }

    /* Symbol table follows the relocations. */
    u32 sym_off = (u32)sizeof(RvModuleHeader) +
                  hdr.reloc_count * (u32)sizeof(RvModuleReloc);
    u32 str_off = sym_off + hdr.sym_count * (u32)sizeof(RvModuleSymbol);
    if (str_off + hdr.str_size > st.size || hdr.image_off < str_off) {
        set_error("Module symbol table is truncated");
        return -1;
    }

    static char namebuf[64];

    for (u32 i = 0; i < hdr.sym_count; i++) {
        RvModuleSymbol sym;
        if (read_exact(path, sym_off + i * (u32)sizeof(sym), &sym,
                       sizeof(sym)) != 0) {
            set_error("Cannot read module symbol");
            return -1;
        }
        if (sym.name_off >= hdr.str_size) {
            set_error("Symbol name out of range");
            return -1;
        }
        u32 n = 0;
        for (;;) {
            char c;
            if (sym.name_off + n >= hdr.str_size) { namebuf[n] = 0; break; }
            if (read_exact(path, str_off + sym.name_off + n, &c, 1) != 0)
                c = 0;
            namebuf[n] = c;
            if (!c || n >= 60) { namebuf[n] = 0; break; }
            n++;
        }
        if (!namebuf[0])
            continue;

        if (hdr.magic == RVMOD_MAGIC_LIB) {
            if (sym.value >= hdr.image_size) {
                set_error("Export out of range");
                return -1;
            }
            add_symbol(namebuf, base + sym.value);
            continue;
        }

        uintptr_t resolved = find_symbol(namebuf);
        if (!resolved) {
            set_error("Unresolved import: ");
            int e = (int)strlen(error);
            for (int k = 0; namebuf[k] && e < (int)sizeof(error) - 1; k++)
                error[e++] = namebuf[k];
            error[e] = 0;
            return -2;
        }

        for (u32 p = 0; p < sym.patch_count; p++) {
            RvModulePatch patch;
            if (read_exact(path, sym.patch_off + p * (u32)sizeof(patch),
                           &patch, sizeof(patch)) != 0) {
                set_error("Cannot read import patch table");
                return -1;
            }
            u8 size = patch.size == 8 ? 8 : 4;
            if (patch.offset + size > hdr.image_size) {
                set_error("Import patch out of range");
                return -1;
            }
            u8 *slot = img + patch.offset;
            if (patch.type == 1) {
                rebase_field(slot, size, resolved);
            } else if (patch.type == 2) {
                /* PC-relative call/jump: displacement to the target. */
                wr_u32(slot, rd_u32(slot) +
                        (u32)(resolved - (uintptr_t)slot));
            } else {
                set_error("Unknown import patch type");
                return -1;
            }
        }
    }

    if (hdr.magic == RVMOD_MAGIC_LIB) {
        if (module_count < 8) {
            int k = 0;
            while (hdr.name[k] && k < 19) {
                module_names[module_count][k] = hdr.name[k];
                k++;
            }
            module_names[module_count][k] = 0;
            if (lib_count < 8) {
                int j = 0;
                while (module_names[module_count][j] && j < 19) {
                    lib_names[lib_count][j] = module_names[module_count][j];
                    j++;
                }
                lib_names[lib_count][j] = 0;
                lib_count++;
            }
            module_count++;
        }
        serial_write("rvmod: loaded library ");
        serial_write(hdr.name);
        serial_write("\r\n");
        return 0;
    }

    /* Executable: switch stacks and call the entry point. */
    static u8 app_stack[64 * 1024] __attribute__((aligned(16)));
    uintptr_t entry = base + hdr.entry;
    if (hdr.entry >= hdr.image_size) {
        set_error("Executable has no valid entry point");
        return -1;
    }

    serial_write("rvmod: running ");
    serial_write(hdr.name);
    serial_write(" at ");
    serial_hex64(entry);
    serial_write("\r\n");

    last_exit = 0;
    rvmod_trampoline(entry, (uintptr_t)app_stack + sizeof(app_stack) - 16);
    last_exit = 0;
    arena_used = mark;                 /* executables are freed after exit */

    serial_write("rvmod: ");
    serial_write(hdr.name);
    serial_write(" returned\r\n");
    return 0;
}

int rvmod_load_lib(const char *path)
{
    return load_module(path, 1);
}

int rvmod_load_libs(const char *dir)
{
    int loaded = 0;
    for (u32 i = 0; i < 32; i++) {
        RvfsEntry e;
        int r = rvfs_readdir(dir, i, &e);
        if (r <= 0)
            break;
        if (e.type != 1)
            continue;
        u32 n = strlen(e.name);
        if (n < 5 || e.name[n - 4] != '.' || e.name[n - 3] != 'l' ||
            e.name[n - 2] != 'i' || e.name[n - 1] != 'b')
            continue;

        char path[160];
        int k = 0;
        while (dir[k] && k < 120) { path[k] = dir[k]; k++; }
        if (k && path[k - 1] != '/') path[k++] = '/';
        for (u32 j = 0; e.name[j] && k < 158; j++) path[k++] = e.name[j];
        path[k] = 0;

        int already = 0;
        for (int l = 0; l < lib_count; l++) {
            int j = 0;
            while (lib_names[l][j] && e.name[j] &&
                   lib_names[l][j] == e.name[j])
                j++;
            if (!lib_names[l][j] && e.name[j] == '.')
                already = 1;
        }
        if (already)
            continue;
        if (rvmod_load_lib(path) == 0)
            loaded++;
    }
    return loaded;
}

int rvmod_run(const char *path)
{
    static char loaded_dir[160];
    if (strcmp(loaded_dir, "/System/Libs") != 0) {
        rvmod_load_libs("/System/Libs");
        int k = 0;
        const char *d = "/System/Libs";
        while (d[k] && k < 159) { loaded_dir[k] = d[k]; k++; }
        loaded_dir[k] = 0;
    }
    rvapi_reset();
    return load_module(path, 0);
}

int rvmod_run_named(const char *dir, const char *name)
{
    char path[160];
    int k = 0;
    while (dir[k] && k < 120) { path[k] = dir[k]; k++; }
    if (k && path[k - 1] != '/') path[k++] = '/';
    for (int i = 0; name[i] && k < 158; i++) path[k++] = name[i];
    path[k] = 0;
    return rvmod_run(path);
}

void rvmod_init(void)
{
    arena_used = 0;
    sym_count = 0;
    module_count = 0;
    lib_count = 0;
    error[0] = 0;
    last_exit = 0;

    int n = rvapi_init();
    for (int i = 0; i < n; i++) {
        const RvApiSymbol *s = rvapi_symbol(i);
        add_symbol(s->name, s->address);
    }
    serial_write("rvmod: ");
    serial_dec((u32)n);
    serial_write(" kernel API symbols\r\n");
}
