#ifndef RVOS_RVMOD_H
#define RVOS_RVMOD_H

#include "types.h"

/* ReverticalOS module containers.
 *
 *   .lib  RVLB  relocatable library with named exports
 *   .rvx  RVEX  executable with named imports, entry point and stack size
 *
 * Both are flat images linked at address 0.  The loader places them in a
 * module arena, applies the absolute relocations, resolves imports against
 * the kernel API and previously loaded libraries, then calls the entry point
 * with a private stack.  Modules run in ring 0 in the kernel address space;
 * they use the kernel ABI by name, not by fixed address.
 *
 * Version 2 is the x86-64 container: relocations and import patches carry
 * an explicit width so both 64-bit pointers and 32-bit code-model absolute
 * references can be rebased into the arena.
 */

#define RVMOD_MAGIC_LIB 0x424C5652u   /* "RVLB" */
#define RVMOD_MAGIC_EXE 0x58455652u   /* "RVEX" */
#define RVMOD_VERSION   2
#define RVMOD_KIND_LIB  1
#define RVMOD_KIND_EXE  2

#define RVMOD_NAME_MAX  27
#define RVMOD_MAX_SYMS  192
#define RVMOD_ARENA     (1024u * 1024u)

typedef struct {
    u32 magic;
    u16 version;
    u16 kind;
    u32 image_size;
    u32 entry;            /* RVA of the entry point (executables) */
    u32 stack_size;       /* requested stack, rounded up by the loader */
    u32 reloc_count;      /* absolute words that need + base */
    u32 sym_count;        /* exports (libraries) or imports (executables) */
    u32 str_size;
    u32 image_off;
    char name[16];
    u32 checksum;         /* sum of the image bytes */
    u32 reserved[2];
} __attribute__((packed)) RvModuleHeader;

typedef struct {
    u32 name_off;         /* into the module string table */
    u32 value;            /* RVA for exports, 0 for imports */
    u32 patch_count;
    u32 patch_off;        /* file offset of the patch table */
} __attribute__((packed)) RvModuleSymbol;

/* A relocation inside the image that must be rebased by the load address. */
typedef struct {
    u32 offset;           /* RVA of the field */
    u8  size;             /* 8 or 4 bytes */
    u8  reserved;
    u16 pad;
} __attribute__((packed)) RvModuleReloc;

/* Patch applied when an import is resolved.
   type 1: absolute address (base already folded in by the rebase pass)
   type 2: PC-relative displacement to an external symbol */
typedef struct {
    u32 offset;           /* RVA of the field */
    u8  type;
    u8  size;             /* 8 or 4 bytes */
    u16 reserved;
} __attribute__((packed)) RvModulePatch;

void rvmod_init(void);
int  rvmod_load_lib(const char *path);
int  rvmod_load_libs(const char *dir);
int  rvmod_run(const char *path);
int  rvmod_run_named(const char *dir, const char *name);

const char *rvmod_output(void);
int  rvmod_output_len(void);
int  rvmod_exit_code(void);
const char *rvmod_error(void);
int  rvmod_module_count(void);
const char *rvmod_module_name(int index);

/* Kernel API surface available to modules (implemented in kernel/rvapi.c). */
typedef struct {
    const char *name;
    uintptr_t address;
} RvApiSymbol;

int rvapi_init(void);
void rvapi_reset(void);
int rvapi_count(void);
const RvApiSymbol *rvapi_symbol(int index);

#endif
