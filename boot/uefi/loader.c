/* ReverticalOS UEFI loader.
 *
 * A small x86-64 PE/EFI application (no gnu-efi, no GRUB).  It
 *   - opens the ESP root through the Simple File System protocol,
 *   - loads \KERNEL.BIN to its link address 0x100000,
 *   - selects a GOP mode that matches the kernel's pixel format,
 *   - passes the ACPI RSDP and the framebuffer in a BootInfo at 0x7000,
 *   - installs its own identity page tables and GDT,
 *   - leaves boot services and jumps to the kernel.
 */

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef unsigned long long uintptr_t;
typedef unsigned long long size_t;
typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef u64 UINTN;
typedef u64 EFI_PHYSICAL_ADDRESS;

#define EFIAPI __attribute__((ms_abi))
#define NULL ((void *)0)
#define EFI_SUCCESS 0

/* ------------------------------------------------------------------ */
/*  UEFI types                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    u32 data1;
    u16 data2;
    u16 data3;
    u8  data4[8];
} __attribute__((packed)) EFI_GUID;

typedef struct {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT;

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    u64 revision;
    void *reset;
    EFI_STATUS (EFIAPI *output_string)(EFI_SIMPLE_TEXT_OUTPUT *, u16 *);
    void *test_string;
    void *query_mode;
    void *set_mode;
    void *set_attribute;
    void *clear_screen;
    void *set_cursor_position;
    void *enable_cursor;
    void *mode;
};

typedef struct {
    EFI_TABLE_HEADER hdr;
    u16 *firmware_vendor;
    u32 firmware_revision;
    EFI_HANDLE console_in_handle;
    void *con_in;
    EFI_HANDLE console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT *con_out;
    EFI_HANDLE standard_error_handle;
    EFI_SIMPLE_TEXT_OUTPUT *std_err;
    void *runtime_services;
    struct EFI_BOOT_SERVICES *boot_services;
    UINTN number_of_table_entries;
    void *configuration_table;
} EFI_SYSTEM_TABLE;

typedef struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER hdr;
    void *raise_tpl;
    void *restore_tpl;
    EFI_STATUS (EFIAPI *allocate_pages)(u32 type, u32 mem_type,
                                        UINTN pages, EFI_PHYSICAL_ADDRESS *addr);
    EFI_STATUS (EFIAPI *free_pages)(EFI_PHYSICAL_ADDRESS addr, UINTN pages);
    EFI_STATUS (EFIAPI *get_memory_map)(UINTN *size, void *map, UINTN *key,
                                        UINTN *desc_size, u32 *desc_version);
    EFI_STATUS (EFIAPI *allocate_pool)(u32 pool_type, UINTN size, void **buf);
    EFI_STATUS (EFIAPI *free_pool)(void *buf);
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    EFI_STATUS (EFIAPI *handle_protocol)(EFI_HANDLE handle, EFI_GUID *guid,
                                         void **interface);
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    EFI_STATUS (EFIAPI *exit_boot_services)(EFI_HANDLE image, UINTN key);
    void *get_next_monotonic_count;
    void *stall;
    void *set_watchdog_timer;
    void *connect_controller;
    void *disconnect_controller;
    void *open_protocol;
    void *close_protocol;
    void *open_protocol_information;
    void *protocols_per_handle;
    void *locate_handle_buffer;
    EFI_STATUS (EFIAPI *locate_protocol)(EFI_GUID *guid, void *registration,
                                         void **interface);
    void *install_multiple_protocol_interfaces;
    void *uninstall_multiple_protocol_interfaces;
    void *calculate_crc32;
    void *copy_mem;
    void *set_mem;
} EFI_BOOT_SERVICES;

typedef struct {
    u64 revision;
    EFI_HANDLE parent_handle;
    EFI_SYSTEM_TABLE *system_table;
    EFI_HANDLE device_handle;
    void *file_path;
    void *reserved;
    u32 load_options_size;
    void *load_options;
    void *image_base;
    u64 image_size;
    u32 image_code_type;
    u32 image_data_type;
    void *unload;
} EFI_LOADED_IMAGE;

typedef struct EFI_FILE_PROTOCOL EFI_FILE;

struct EFI_FILE_PROTOCOL {
    u64 revision;
    EFI_STATUS (EFIAPI *open)(EFI_FILE *, EFI_FILE **, u16 *, u64, u64);
    EFI_STATUS (EFIAPI *close)(EFI_FILE *);
    EFI_STATUS (EFIAPI *delete)(EFI_FILE *);
    EFI_STATUS (EFIAPI *read)(EFI_FILE *, UINTN *, void *);
    EFI_STATUS (EFIAPI *write)(EFI_FILE *, UINTN *, void *);
    EFI_STATUS (EFIAPI *get_position)(EFI_FILE *, u64 *);
    EFI_STATUS (EFIAPI *set_position)(EFI_FILE *, u64);
    EFI_STATUS (EFIAPI *get_info)(EFI_FILE *, EFI_GUID *, UINTN *, void *);
    EFI_STATUS (EFIAPI *set_info)(EFI_FILE *, EFI_GUID *, UINTN, void *);
    EFI_STATUS (EFIAPI *flush)(EFI_FILE *);
};

typedef struct EFI_SIMPLE_FILE_SYSTEM EFI_SIMPLE_FILE_SYSTEM;
struct EFI_SIMPLE_FILE_SYSTEM {
    u64 revision;
    EFI_STATUS (EFIAPI *open_volume)(EFI_SIMPLE_FILE_SYSTEM *, EFI_FILE **);
};

typedef struct {
    u32 version;
    u32 horizontal_resolution;
    u32 vertical_resolution;
    u32 pixel_format;
    u32 red_mask;
    u32 green_mask;
    u32 blue_mask;
    u32 reserved_mask;
    u32 pixels_per_scan_line;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    u32 max_mode;
    u32 mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    UINTN size_of_info;
    EFI_PHYSICAL_ADDRESS frame_buffer_base;
    UINTN frame_buffer_size;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    EFI_STATUS (EFIAPI *query_mode)(void *, u32, UINTN *,
                                    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (EFIAPI *set_mode)(void *, u32);
    void *blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    EFI_GUID vendor_guid;
    void *vendor_table;
} EFI_CONFIGURATION_TABLE;

/* ------------------------------------------------------------------ */
/*  GUIDs                                                             */
/* ------------------------------------------------------------------ */
static const EFI_GUID guid_loaded_image =
    { 0x5B1B31A1, 0x9562, 0x11D2,
      { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID guid_simple_fs =
    { 0x964E5B22, 0x6459, 0x11D2,
      { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID guid_file_info =
    { 0x09576E92, 0x6D3F, 0x11D2,
      { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID guid_gop =
    { 0x9042A9DE, 0x23DC, 0x4A38,
      { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } };
static const EFI_GUID guid_acpi20 =
    { 0x8868E871, 0xE4F1, 0x11D3,
      { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };
static const EFI_GUID guid_acpi10 =
    { 0xEB9D2D30, 0x2D88, 0x11D3,
      { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

/* ------------------------------------------------------------------ */
/*  Image layout / BootInfo                                           */
/* ------------------------------------------------------------------ */
#define KERNEL_BASE   0x100000ull
#define KERNEL_MAX    (1536ull * 1024ull)
/* Top of the kernel's BSS plus headroom.  The kernel linker script puts BSS
   at 3 MiB and it currently ends below 0x229d000; the loader reserves this
   whole span at 1 MiB so the kernel's BSS clear cannot overwrite anything
   the firmware still owns.  keep it in sync with kernel/linker.ld (make
   checks __bss_end against this value). */
#ifndef KERNEL_RESERVE_END
#define KERNEL_RESERVE_END 0x2800000ull
#endif
#define BOOTINFO_ADDR 0x7000ull
#define PAGE_SIZE     4096ull
#define MAX_PDS       512            /* identity map up to 512 GiB */

typedef struct __attribute__((packed)) {
    u32 magic;
    u64 fb_phys;
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
    u16 mode;
    u8  red_pos;
    u8  red_size;
    u8  green_pos;
    u8  green_size;
    u8  blue_pos;
    u8  blue_size;
    u32 kernel_size;
    u32 boot_drive;
    u64 acpi_rsdp;
    u32 flags;
    u32 reserved;
} BootInfo;

#define BOOTINFO_MAGIC 0x4F425652u
#define BOOT_DRIVE_UEFI 0xFFFFFFFEu
#define BOOT_FLAG_UEFI 0x00000001u

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_SIMPLE_TEXT_OUTPUT *OUT;

static u8 loader_stack[64 * 1024] __attribute__((aligned(16)));

/* Identity page tables.  PDs are allocated only for the regions we map. */
static u64 pml4[512] __attribute__((aligned(4096)));
static u64 pdpt[512] __attribute__((aligned(4096)));
static u64 pds[MAX_PDS][512] __attribute__((aligned(4096)));

static void *memset(void *dst, int c, size_t n)
{
    u8 *d = (u8 *)dst;
    while (n--)
        *d++ = (u8)c;
    return dst;
}

static void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

static void puts16(const u16 *s)
{
    if (OUT)
        OUT->output_string(OUT, (u16 *)s);
}

static void puts8(const char *s)
{
    u16 buf[160];
    int n = 0;
    while (*s && n < 158)
        buf[n++] = (u16)(u8)*s++;
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = 0;
    puts16(buf);
}

static void hex64(u64 v, char *out)
{
    static const char digits[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = digits[(v >> (60 - i * 4)) & 0xF];
    out[18] = 0;
}

/* COM1 log.  Firmware normally leaves the UART usable; setting 115200 8N1
   explicitly means a headless UEFI boot is diagnosable from the VM serial
   port even when the graphical console is not visible. */
static inline void outb(u16 port, u8 value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void serial_init(void)
{
    outb(0x3F9, 0x00);          /* interrupts off */
    outb(0x3FB, 0x80);          /* DLAB on */
    outb(0x3F8, 0x01);          /* divisor 1 = 115200 */
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x03);          /* 8N1 */
    outb(0x3FA, 0xC7);
    outb(0x3FC, 0x0B);
}

static void serial_str(const char *s)
{
    while (*s)
        outb(0x3F8, (u8)*s++);
}

static int uuid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    for (int i = 0; i < 16; i++)
        if (pa[i] != pb[i])
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  File loading                                                      */
/* ------------------------------------------------------------------ */
static EFI_FILE *open_root(EFI_HANDLE image)
{
    EFI_LOADED_IMAGE *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM *fs = NULL;
    EFI_FILE *root = NULL;

    if (BS->handle_protocol(image, (EFI_GUID *)&guid_loaded_image,
                            (void **)&li) != EFI_SUCCESS || !li)
        return NULL;
    if (BS->handle_protocol(li->device_handle, (EFI_GUID *)&guid_simple_fs,
                            (void **)&fs) != EFI_SUCCESS || !fs)
        return NULL;
    if (fs->open_volume(fs, &root) != EFI_SUCCESS)
        return NULL;
    return root;
}

static EFI_FILE *open_file(EFI_FILE *root, const u16 *path)
{
    EFI_FILE *file = NULL;
    if (!root)
        return NULL;
    if (root->open(root, &file, (u16 *)path, 1, 0) != EFI_SUCCESS)
        return NULL;
    return file;
}

static u64 file_size(EFI_FILE *file)
{
    u8 info[256];
    UINTN size = sizeof(info);
    if (file->get_info(file, (EFI_GUID *)&guid_file_info, &size, info)
            != EFI_SUCCESS)
        return 0;
    /* EFI_FILE_INFO: UINT64 Size; UINT64 FileSize; ... */
    u64 value;
    memcpy(&value, info + 8, 8);
    return value;
}

static int load_kernel(EFI_FILE *file, u64 size, u32 *out_size)
{
    if (size < 512 || size > KERNEL_MAX)
        return -1;

    /* Reserve the image plus the kernel's BSS, so the kernel's own BSS
       clear cannot stomp on firmware data.  If the whole span is not free
       as one block, fall back to reserving just the loaded image. */
    UINTN span_pages = (UINTN)((KERNEL_RESERVE_END - KERNEL_BASE +
                                PAGE_SIZE - 1) / PAGE_SIZE);
    UINTN pages = (UINTN)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    EFI_PHYSICAL_ADDRESS addr = KERNEL_BASE;
    if (span_pages < pages)
        span_pages = pages;
    if (BS->allocate_pages(2 /* AllocateAddress */, 2 /* LoaderData */,
                           span_pages, &addr) != EFI_SUCCESS) {
        addr = KERNEL_BASE;
        if (BS->allocate_pages(2 /* AllocateAddress */, 2 /* LoaderData */,
                               pages, &addr) != EFI_SUCCESS)
            return -1;
    }

    u8 *dst = (u8 *)(uintptr_t)KERNEL_BASE;
    u64 left = size;
    while (left) {
        UINTN chunk = (UINTN)(left > 0x10000 ? 0x10000 : left);
        UINTN want = chunk;
        if (file->read(file, &chunk, dst) != EFI_SUCCESS || chunk != want)
            return -1;
        dst += chunk;
        left -= chunk;
    }
    *out_size = (u32)size;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Graphics                                                          */
/* ------------------------------------------------------------------ */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *gop_info;

static int choose_mode(void)
{
    if (!gop || !gop->mode)
        return -1;

    u32 best = 0xFFFFFFFFu;
    u64 best_pixels = 0;
    int best_bgr = 0;
    u32 max = gop->mode->max_mode;
    for (u32 i = 0; i < max; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN size = 0;
        if (gop->query_mode(gop, i, &size, &info) != EFI_SUCCESS || !info)
            continue;
        if (info->pixel_format == 2)          /* BitMask: kernel cannot use */
            continue;
        if (info->horizontal_resolution > 1600 ||
            info->vertical_resolution > 1200)
            continue;
        if (info->horizontal_resolution < 640 ||
            info->vertical_resolution < 480)
            continue;
        u64 pixels = (u64)info->horizontal_resolution *
                     info->vertical_resolution;
        /* The compositor writes 0x00RRGGBB words, which is exactly
           BlueGreenRedReserved8BitPerColor in memory.  Prefer that format
           strictly, then the largest area within it. */
        int is_bgr = info->pixel_format == 1;
        int better;
        if (best == 0xFFFFFFFFu)
            better = 1;
        else if (is_bgr != best_bgr)
            better = is_bgr;
        else
            better = pixels > best_pixels;
        if (better) {
            best = i;
            best_pixels = pixels;
            best_bgr = is_bgr;
        }
    }

    if (best == 0xFFFFFFFFu)
        return -1;
    if (gop->set_mode(gop, best) != EFI_SUCCESS)
        return -1;

    gop_info = gop->mode->info;
    if (!gop_info || gop_info->pixel_format == 2)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  ACPI                                                              */
/* ------------------------------------------------------------------ */
static u64 find_rsdp(void)
{
    EFI_CONFIGURATION_TABLE *tables =
        (EFI_CONFIGURATION_TABLE *)ST->configuration_table;
    for (UINTN i = 0; i < ST->number_of_table_entries; i++) {
        if (uuid_eq(&tables[i].vendor_guid, &guid_acpi20) ||
            uuid_eq(&tables[i].vendor_guid, &guid_acpi10))
            return (u64)(uintptr_t)tables[i].vendor_table;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Identity page tables                                              */
/* ------------------------------------------------------------------ */
static void map_identity(void)
{
    memset(pml4, 0, sizeof(pml4));
    memset(pdpt, 0, sizeof(pdpt));

    u64 fb_end = gop_info
        ? (u64)gop->mode->frame_buffer_base + gop->mode->frame_buffer_size
        : 0;
    u64 limit = 64ull * 1024 * 1024 * 1024;        /* always map 0..64 GiB */
    if (fb_end > limit)
        limit = (fb_end + 0x3FFFFFFFull) & ~0x3FFFFFFFull;

    u32 pd_count = (u32)((limit + 0x3FFFFFFFull) / 0x40000000ull);
    if (pd_count > MAX_PDS)
        pd_count = MAX_PDS;

    pml4[0] = (u64)(uintptr_t)pdpt | 0x3;
    for (u32 i = 0; i < pd_count; i++) {
        memset(pds[i], 0, sizeof(pds[i]));
        pdpt[i] = (u64)(uintptr_t)pds[i] | 0x3;
        for (u32 j = 0; j < 512; j++) {
            u64 phys = ((u64)i * 0x40000000ull) + ((u64)j * 0x200000ull);
            pds[i][j] = phys | 0x83;              /* P | RW | PS */
        }
    }
}

/* ------------------------------------------------------------------ */
/*  GDT and kernel handoff                                            */
/* ------------------------------------------------------------------ */
static u64 gdt[3] = {
    0,
    0x00AF9A000000FFFFull,      /* 0x08: 64-bit code */
    0x00CF92000000FFFFull,      /* 0x10: data */
};
static struct __attribute__((packed)) {
    u16 limit;
    u64 base;
} gdtr;

static void enter_kernel(u64 bootinfo)
{
    gdtr.limit = (u16)(sizeof(gdt) - 1);
    gdtr.base = (u64)(uintptr_t)gdt;

    __asm__ volatile (
        "cli\n"
        "lgdt %[gdtr]\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        "movq %[cr3], %%cr3\n"
        "movq %[stack], %%rsp\n"
        "xorq %%rbp, %%rbp\n"
        "movq %[bootinfo], %%rdi\n"
        "movq $0x100000, %%rax\n"
        "jmp *%%rax\n"
        :
        : [gdtr] "m"(gdtr),
          [cr3] "r"((u64)(uintptr_t)pml4),
          [stack] "r"((u64)(uintptr_t)loader_stack + sizeof(loader_stack) - 16),
          [bootinfo] "r"(bootinfo)
        : "rax", "memory");
    for (;;) { }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    ST = st;
    BS = st->boot_services;
    OUT = st->con_out;

    puts8("ReverticalOS UEFI loader");
    serial_init();
    serial_str("\r\nRVEFI: entry\r\n");

    if (BS->set_watchdog_timer)
        ((EFI_STATUS (EFIAPI *)(UINTN, u64, UINTN, u16 *))
            BS->set_watchdog_timer)(0, 0, 0, NULL);

    /* ---------------- kernel ---------------- */
    EFI_FILE *root = open_root(image);
    static const u16 kernel_path[] = {
        '\\', 'K', 'E', 'R', 'N', 'E', 'L', '.', 'B', 'I', 'N', 0
    };
    EFI_FILE *kernel_file = open_file(root, kernel_path);
    if (!kernel_file) {
        puts8("loader: \\KERNEL.BIN not found on the ESP");
        serial_str("RVEFI: KERNEL.BIN not found\r\n");
        return 1;
    }
    u64 ksize = file_size(kernel_file);
    u32 kloaded = 0;
    if (load_kernel(kernel_file, ksize, &kloaded) != 0) {
        puts8("loader: cannot load kernel at 0x100000");
        serial_str("RVEFI: kernel allocation failed\r\n");
        return 1;
    }
    kernel_file->close(kernel_file);
    serial_str("RVEFI: kernel loaded\r\n");

    /* ---------------- display ---------------- */
    if (BS->locate_protocol((EFI_GUID *)&guid_gop, NULL,
                            (void **)&gop) != EFI_SUCCESS || !gop) {
        puts8("loader: no Graphics Output Protocol");
        serial_str("RVEFI: no GOP\r\n");
        return 1;
    }
    if (choose_mode() != 0) {
        puts8("loader: no usable GOP mode");
        serial_str("RVEFI: no usable GOP mode\r\n");
        return 1;
    }
    serial_str("RVEFI: gop ready\r\n");

    /* ---------------- BootInfo ---------------- */
    BootInfo *bi = (BootInfo *)(uintptr_t)BOOTINFO_ADDR;
    memset(bi, 0, sizeof(*bi));
    bi->magic = BOOTINFO_MAGIC;
    bi->fb_phys = gop->mode->frame_buffer_base;
    bi->width = gop_info->horizontal_resolution;
    bi->height = gop_info->vertical_resolution;
    bi->pitch = gop_info->pixels_per_scan_line * 4u;
    bi->bpp = 32;
    bi->mode = 0xFFFF;
    bi->red_pos = gop_info->pixel_format == 1 ? 16 : 0;
    bi->red_size = 8;
    bi->green_pos = 8;
    bi->green_size = 8;
    bi->blue_pos = gop_info->pixel_format == 1 ? 0 : 16;
    bi->blue_size = 8;
    bi->kernel_size = kloaded;
    bi->boot_drive = BOOT_DRIVE_UEFI;
    bi->acpi_rsdp = find_rsdp();
    bi->flags = BOOT_FLAG_UEFI;

    char line[32];
    hex64(bi->fb_phys, line);
    puts8("loader: framebuffer");
    puts8(line);

    /* ---------------- leave firmware ---------------- */
    map_identity();

    UINTN map_size = 0, key = 0, desc_size = 0;
    u32 desc_ver = 0;
    BS->get_memory_map(&map_size, NULL, &key, &desc_size, &desc_ver);
    map_size += 8192;
    void *map = NULL;
    if (BS->allocate_pool(2, map_size, &map) != EFI_SUCCESS) {
        puts8("loader: cannot allocate memory map");
        return 1;
    }

    EFI_STATUS status = 1;
    for (int attempt = 0; attempt < 4; attempt++) {
        UINTN size = map_size;
        if (BS->get_memory_map(&size, map, &key, &desc_size,
                               &desc_ver) != EFI_SUCCESS)
            continue;
        status = BS->exit_boot_services(image, key);
        if (status == EFI_SUCCESS)
            break;
    }
    if (status != EFI_SUCCESS) {
        puts8("loader: ExitBootServices failed");
        serial_str("RVEFI: ExitBootServices failed\r\n");
        return 1;
    }

    serial_str("RVEFI: entering kernel\r\n");
    enter_kernel(BOOTINFO_ADDR);
    return 0;
}
