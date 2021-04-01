﻿#include "vm.h"
#include "elfloadimage.hh"
 
ElfLoadImage::ElfLoadImage(const char *filename):LoadImageB(filename)
{
    filedata = (unsigned char *)file_load(filename, (int *)&filelen);
    if (!filedata) {
        printf("ElfLoadImage() failed open [%s]", filename);
        exit(0);
    }

    //isdata = bitset_new(filelen);
    cur_sym = -1;
}

ElfLoadImage::~ElfLoadImage()
{
    file_unload((char *)filedata);
}

int ElfLoadImage::loadFill(uint1 *ptr, int size, const Address &addr) 
{
    unsigned start = (unsigned)addr.getOffset();
    if ((start + size) > filelen) {
        /* FIXME: 我们对所有访问的超过空间的地址都返回 0xaabbccdd，这里不是BUG，是因为我们载入so的时候，是直接平铺着载入的
        但是实际在程序加载so的时候，会填充很多结构，并做一些扩展 */
        return -1;
    }

    /* FIXME: .got表我没有生成，这里直接根据IDA的结构，手动写了*/
    if ((start == 0xfe8c) && (size == 4)) {
        ptr[0] = 0x98;
        ptr[1] = 0x57;
        ptr[2] = 0x08;
        ptr[3] = 0x00;
        return 0;
    }

    memcpy(ptr, filedata + start, size);
    return 0;
}

bool ElfLoadImage::getNextSymbol(LoadImageFunc &record) 
{
    Elf32_Shdr *dynsymsh, *link_sh;
    Elf32_Sym *sym;
    Elf32_Ehdr *hdr = (Elf32_Ehdr *)filedata;
    int num;
    const char *name;

    cur_sym++;

    dynsymsh = elf32_shdr_get((Elf32_Ehdr *)filedata, SHT_DYNSYM);
    if (!dynsymsh) 
        vm_error("file[%s] have not .dymsym section", filename.c_str());

    link_sh = (Elf32_Shdr *)(filedata + hdr->e_shoff) + dynsymsh->sh_link;

    num = dynsymsh->sh_size / dynsymsh->sh_entsize;
    if (cur_sym >= num) {
        cur_sym = -1;
        return false;
    }

    sym = (Elf32_Sym *)(filedata + dynsymsh->sh_offset) + cur_sym;
    name = (char *)filedata + (link_sh->sh_offset + sym->st_name);

    record.address = Address(codespace, sym->st_value);
    record.name = string(name);
    record.size = sym->st_size;
    record.bufptr = filedata + sym->st_value;

    return true;
}

int ElfLoadImage::getSymbol(const char *symname, LoadImageFunc &record)
{
    Elf32_Sym *sym = elf32_sym_find2((Elf32_Ehdr *)filedata, symname);

    if (!sym)
        return -1;

    record.address = Address(codespace, sym->st_value);
    record.name = string(symname);
    record.size = sym->st_size;
    record.bufptr = filedata + sym->st_value;
    return 0;
}

int ElfLoadImage::saveSymbol(const char *symname, int size)
{
    Elf32_Sym *sym = elf32_sym_find2((Elf32_Ehdr *)filedata, symname);

    if (!sym)
        return -1;

    sym->st_size = size;

    return 0;
}

void ElfLoadImage::saveFile(const char *filename)
{
    file_save(filename, (char *)filedata, filelen);
}
