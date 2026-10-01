/* SPDX-License-Identifier: 0BSD */
/* ELF64 结构定义（最小必要子集）。
 * 同时用于：内核自映像映射（M1）与用户态程序加载（M4）。
 */
#ifndef FE_ELF_H
#define FE_ELF_H

#include <fe/types.h>
#include <fe/compiler.h>

#define FE_ELF_MAGIC0 0x7f
#define FE_ELF_MAGIC1 'E'
#define FE_ELF_MAGIC2 'L'
#define FE_ELF_MAGIC3 'F'

#define FE_ELFCLASS64 2
#define FE_ELFDATA2LSB 1
#define FE_ET_EXEC 2
#define FE_ET_DYN  3
#define FE_EM_X86_64 62

#define FE_PT_NULL    0
#define FE_PT_LOAD    1
#define FE_PT_DYNAMIC 2
#define FE_PT_INTERP  3
#define FE_PT_PHDR    6
#define FE_PT_TLS     7
#define FE_PT_GNU_STACK 0x6474e551

#define FE_PF_X 1
#define FE_PF_W 2
#define FE_PF_R 4

struct fe_elf64_ehdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} FE_PACKED;

struct fe_elf64_phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} FE_PACKED;

struct fe_elf64_shdr {
    u32 sh_name;
    u32 sh_type;
    u64 sh_flags;
    u64 sh_addr;
    u64 sh_offset;
    u64 sh_size;
    u32 sh_link;
    u32 sh_info;
    u64 sh_addralign;
    u64 sh_entsize;
} FE_PACKED;

struct fe_elf64_sym {
    u32 st_name;
    u8  st_info;
    u8  st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
} FE_PACKED;

static inline bool fe_elf_check(const struct fe_elf64_ehdr *eh)
{
    return eh->e_ident[0] == FE_ELF_MAGIC0 && eh->e_ident[1] == FE_ELF_MAGIC1 &&
           eh->e_ident[2] == FE_ELF_MAGIC2 && eh->e_ident[3] == FE_ELF_MAGIC3 &&
           eh->e_ident[4] == FE_ELFCLASS64 && eh->e_ident[5] == FE_ELFDATA2LSB &&
           eh->e_machine == FE_EM_X86_64;
}

#endif /* FE_ELF_H */
