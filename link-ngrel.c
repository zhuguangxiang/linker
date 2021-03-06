#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "linker/elf.h"
#include "elf-ng.h"

static inline uintptr_t round_down(uintptr_t val, uintptr_t place) {
    return val & ~(place - 1);
}

static inline uintptr_t round_up(uintptr_t val, uintptr_t place) {
    return round_down(val + place - 1, place);
}

void *elf_sym_addr(elf_md *e, Elf_Sym *sym);

void elf_relocate(elf_md *e, Elf_Shdr *modify_section, Elf_Rela *rela) {
    size_t sym_index = ELF64_R_SYM(rela->r_info);
    Elf_Sym *sym = &e->symbol_table[sym_index];
    int rel_type = ELF64_R_TYPE(rela->r_info);
    const char *name = elf_symbol_name(e, sym);

    size_t section_offset = rela->r_offset;
    void *relocation = (char *)modify_section->sh_addr + section_offset;

    uint64_t A = rela->r_addend;
    uint64_t P = (uint64_t)relocation;
    uint64_t S = (uint64_t)elf_sym_addr(e, sym);

    // printf("A: %#lx\n", A);
    // printf("P: %#lx\n", P);
    // printf("S: %#lx\n", S);
    // printf("relocation @ %p ", relocation);
    // switch (rel_type) {
    // case R_X86_64_NONE: printf("none\n"); break;
    // case R_X86_64_64: printf("64: %#lx\n", S + A); break;
    // case R_X86_64_32: printf("32: %#x\n", (uint32_t)(S + A)); break;
    // case R_X86_64_32S: printf("32S: %#x\n", (int32_t)(S + A)); break;
    // case R_X86_64_PC32: // FALLTHROUGH
    // case R_X86_64_PLT32: printf("PC32: %#x\n", (uint32_t)(S + A - P)); break;
    // }

    switch (rel_type) {
    case R_X86_64_NONE: break;
    case R_X86_64_64: *(uint64_t *)relocation = S + A; break;
    case R_X86_64_32: *(uint32_t *)relocation = S + A; break;
    case R_X86_64_32S: *(int32_t *)relocation = S + A; break;
    case R_X86_64_PC32: // FALLTHROUGH
    case R_X86_64_PLT32: *(uint32_t *)relocation = S + A - P; break;
    default: printf("invalid relocation type: %i\n", rel_type);
    }
}


elf_md *elf_relo_load(elf_md *relo) {
    // get needed virtual allocation size (file size + sum of all common
    // symbol sizes)
    size_t relo_needed_virtual_size = relo->file_size;
    size_t relo_common_size = 0;
    assert(relo_needed_virtual_size > 0);

    assert(relo->image->e_type == ET_REL);
    assert(relo->symbol_table);
    assert(relo->string_table);

    // total bss size needed
    for (int i=0; i<relo->symbol_count; i++) {
        Elf_Sym *sym = relo->symbol_table + i;
        if (sym->st_shndx == SHN_COMMON) {
            // This math must match the bss symbol placement below
            relo_common_size = round_up(relo_common_size, sym->st_value);
            relo_common_size += sym->st_size;
        }
    }
    relo_needed_virtual_size += relo_common_size;

    void *relo_load = mmap(NULL, relo_needed_virtual_size,
            PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    memcpy(relo_load, relo->mem, relo->file_size);
    memset(relo->bss_base, 0, relo_common_size);

    munmap(relo->mem, relo->file_size);
    free(relo);
    relo = elf_parse(relo_load);

    relo->load_mem = relo_load;
    relo->bss_base = relo_load + relo->file_size;

    // Set shdr->sh_addr to their loaded addresses.
    for (int i=0; i<relo->image->e_shnum; i++) {
        Elf_Shdr *shdr = &relo->section_headers[i];
        shdr->sh_addr = shdr->sh_offset + (uintptr_t)relo->load_mem;
    }

    Elf_Shdr *bss = elf_find_section(relo, ".bss");
    bss->sh_addr = (uintptr_t)relo->bss_base;

    // Place bss symbols' st_values in bss region.
    for (int i=0; i<relo->symbol_count; i++) {
        size_t bss_offset = 0;
        Elf_Sym *sym = relo->symbol_table + i;
        if (sym->st_shndx == SHN_COMMON) {
            // This math must match the bss size calculation above
            // (relo_common_size)
            size_t value = sym->st_value;
            sym->st_value = round_up(bss_offset, value);
            bss_offset += sym->st_size;
        }
    }

    for (int i=0; i<relo->image->e_shnum; i++) {
        Elf_Shdr *sec = relo->section_headers + i;
        if (sec->sh_type != SHT_RELA) {
            continue;
        }

        Elf_Shdr *modify_section = relo->section_headers + sec->sh_info;
        size_t section_size = sec->sh_size;
        size_t rela_count = section_size / sizeof(Elf_Rela);
        for (size_t i=0; i<rela_count; i++) {
            Elf_Rela *rela = (Elf_Rela *)sec->sh_addr + i;
            elf_relocate(relo, modify_section, rela);
        }
    }

    return relo;
}

// For all as-yet undefined symbols in `module`, resolve them to a
// equivalently-named symbol in `main`
void elf_relo_resolve(elf_md *module, elf_md *main) {
    for (size_t i=0; i<module->symbol_count; i++) {
        Elf_Sym *sym = &module->symbol_table[i];
        if (sym->st_shndx == SHN_UNDEF) {
            const char *name = elf_symbol_name(module, sym);
            Elf_Sym *psym = elf_find_symbol(main, name);
            sym->st_value = psym->st_value;
            sym->st_shndx = SHN_ABS;
        }
    }
}

void *elf_sym_addr(elf_md *e, Elf_Sym *sym) {
    if (sym->st_shndx == SHN_COMMON) {
        return (char *)e->bss_base + sym->st_value;
    }
    Elf_Shdr *section = &e->section_headers[sym->st_shndx];
    return (char *)section->sh_addr + sym->st_value;
}


void test_mod_ld() {
    elf_md *relo = elf_open("lib.ko");
    elf_print(relo);
    relo = elf_relo_load(relo);
    elf_print(relo);

    Elf_Sym *lprint_sym = elf_find_symbol(relo, "lprint");
    void (*lprint)(const char *);
    lprint = (void (*)(const char *))elf_sym_addr(relo, lprint_sym);
    printf("lprint is %p\n", lprint);
    lprint("Hello World from the module\n");

    free(relo);
}

int main(int argc, char **argv) {
    test_mod_ld();
}
