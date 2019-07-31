#include <Common/SymbolIndex.h>

#include <algorithm>
#include <optional>

#include <link.h>

//#include <iostream>
#include <filesystem>


namespace DB
{

namespace
{

/// Notes: "PHDR" is "Program Headers".
/// To look at program headers, run:
///  readelf -l ./clickhouse-server
/// To look at section headers, run:
///  readelf -S ./clickhouse-server
/// Also look at: https://wiki.osdev.org/ELF
/// Also look at: man elf
/// http://www.linker-aliens.org/blogs/ali/entry/inside_elf_symbol_tables/
/// https://stackoverflow.com/questions/32088140/multiple-string-tables-in-elf-object


/// Based on the code of musl-libc and the answer of Kanalpiroge on
/// https://stackoverflow.com/questions/15779185/list-all-the-functions-symbols-on-the-fly-in-c-code-on-a-linux-architecture
/// It does not extract all the symbols (but only public - exported and used for dynamic linking),
/// but will work if we cannot find or parse ELF files.
void collectSymbolsFromProgramHeaders(dl_phdr_info * info,
    std::vector<SymbolIndex::Symbol> & symbols)
{
    /* Iterate over all headers of the current shared lib
     * (first call is for the executable itself)
     */
    for (size_t header_index = 0; header_index < info->dlpi_phnum; ++header_index)
    {
        /* Further processing is only needed if the dynamic section is reached
         */
        if (info->dlpi_phdr[header_index].p_type != PT_DYNAMIC)
            continue;

        /* Get a pointer to the first entry of the dynamic section.
         * It's address is the shared lib's address + the virtual address
         */
        const ElfW(Dyn) * dyn_begin = reinterpret_cast<const ElfW(Dyn) *>(info->dlpi_addr + info->dlpi_phdr[header_index].p_vaddr);

        /// For unknown reason, addresses are sometimes relative sometimes absolute.
        auto correct_address = [](ElfW(Addr) base, ElfW(Addr) ptr)
        {
            return ptr > base ? ptr : base + ptr;
        };

        /* Iterate over all entries of the dynamic section until the
         * end of the symbol table is reached. This is indicated by
         * an entry with d_tag == DT_NULL.
         */

        size_t sym_cnt = 0;
        for (auto it = dyn_begin; it->d_tag != DT_NULL; ++it)
        {
            if (it->d_tag == DT_HASH)
            {
                const ElfW(Word) * hash = reinterpret_cast<const ElfW(Word) *>(correct_address(info->dlpi_addr, it->d_un.d_ptr));
                sym_cnt = hash[1];
                break;
            }
            else if (it->d_tag == DT_GNU_HASH)
            {
                /// This code based on Musl-libc.

                const uint32_t * buckets = nullptr;
                const uint32_t * hashval = nullptr;

                const ElfW(Word) * hash = reinterpret_cast<const ElfW(Word) *>(correct_address(info->dlpi_addr, it->d_un.d_ptr));

                buckets = hash + 4 + (hash[2] * sizeof(size_t) / 4);

                for (ElfW(Word) i = 0; i < hash[0]; ++i)
                    if (buckets[i] > sym_cnt)
                        sym_cnt = buckets[i];

                if (sym_cnt)
                {
                    sym_cnt -= hash[1];
                    hashval = buckets + hash[0] + sym_cnt;
                    do
                    {
                        ++sym_cnt;
                    }
                    while (!(*hashval++ & 1));
                }

                break;
            }
        }

        if (!sym_cnt)
            continue;

        const char * strtab = nullptr;
        for (auto it = dyn_begin; it->d_tag != DT_NULL; ++it)
        {
            if (it->d_tag == DT_STRTAB)
            {
                strtab = reinterpret_cast<const char *>(correct_address(info->dlpi_addr, it->d_un.d_ptr));
                break;
            }
        }

        if (!strtab)
            continue;

        for (auto it = dyn_begin; it->d_tag != DT_NULL; ++it)
        {
            if (it->d_tag == DT_SYMTAB)
            {
                /* Get the pointer to the first entry of the symbol table */
                const ElfW(Sym) * elf_sym = reinterpret_cast<const ElfW(Sym) *>(correct_address(info->dlpi_addr, it->d_un.d_ptr));

                /* Iterate over the symbol table */
                for (ElfW(Word) sym_index = 0; sym_index < sym_cnt; ++sym_index)
                {
                    /// We are not interested in empty symbols.
                    if (!elf_sym[sym_index].st_size)
                        continue;

                    /* Get the name of the sym_index-th symbol.
                     * This is located at the address of st_name relative to the beginning of the string table.
                     */
                    const char * sym_name = &strtab[elf_sym[sym_index].st_name];

                    if (!sym_name)
                        continue;

                    SymbolIndex::Symbol symbol;
                    symbol.address_begin = reinterpret_cast<const void *>(info->dlpi_addr + elf_sym[sym_index].st_value);
                    symbol.address_end = reinterpret_cast<const void *>(info->dlpi_addr + elf_sym[sym_index].st_value + elf_sym[sym_index].st_size);
                    symbol.name = sym_name;
                    symbols.push_back(std::move(symbol));
                }

                break;
            }
        }
    }
}


void collectSymbolsFromELFSymbolTable(
    dl_phdr_info * info,
    const Elf & elf,
    const Elf::Section & symbol_table,
    const Elf::Section & string_table,
    std::vector<SymbolIndex::Symbol> & symbols)
{
    /// Iterate symbol table.
    const ElfSym * symbol_table_entry = reinterpret_cast<const ElfSym *>(symbol_table.begin());
    const ElfSym * symbol_table_end = reinterpret_cast<const ElfSym *>(symbol_table.end());

    const char * strings = string_table.begin();

    for (; symbol_table_entry < symbol_table_end; ++symbol_table_entry)
    {
        if (!symbol_table_entry->st_name
            || !symbol_table_entry->st_value
            || !symbol_table_entry->st_size
            || strings + symbol_table_entry->st_name >= elf.end())
            continue;

        /// Find the name in strings table.
        const char * symbol_name = strings + symbol_table_entry->st_name;

        if (!symbol_name)
            continue;

        SymbolIndex::Symbol symbol;
        symbol.address_begin = reinterpret_cast<const void *>(info->dlpi_addr + symbol_table_entry->st_value);
        symbol.address_end = reinterpret_cast<const void *>(info->dlpi_addr + symbol_table_entry->st_value + symbol_table_entry->st_size);
        symbol.name = symbol_name;
        symbols.push_back(std::move(symbol));
    }
}


bool searchAndCollectSymbolsFromELFSymbolTable(
    dl_phdr_info * info,
    const Elf & elf,
    unsigned section_header_type,
    const char * string_table_name,
    std::vector<SymbolIndex::Symbol> & symbols)
{
    std::optional<Elf::Section> symbol_table;
    std::optional<Elf::Section> string_table;

    if (!elf.iterateSections([&](const Elf::Section & section, size_t)
        {
            if (section.header.sh_type == section_header_type)
                symbol_table.emplace(section);
            else if (section.header.sh_type == SHT_STRTAB && 0 == strcmp(section.name(), string_table_name))
                string_table.emplace(section);

            if (symbol_table && string_table)
                return true;
            return false;
        }))
    {
        return false;
    }

    collectSymbolsFromELFSymbolTable(info, elf, *symbol_table, *string_table, symbols);
    return true;
}


void collectSymbolsFromELF(dl_phdr_info * info,
    std::vector<SymbolIndex::Symbol> & symbols,
    std::vector<SymbolIndex::Object> & objects)
{
    std::string object_name = info->dlpi_name;

    /// If the name is empty - it's main executable.
    /// Find a elf file for the main executable.

    if (object_name.empty())
        object_name = "/proc/self/exe";

    std::error_code ec;
    std::filesystem::path canonical_path = std::filesystem::canonical(object_name, ec);

    if (ec)
        return;

    /// Debug info and symbol table sections may be splitted to separate binary.
    std::filesystem::path debug_info_path = std::filesystem::path("/usr/lib/debug") / canonical_path;

    object_name = std::filesystem::exists(debug_info_path) ? debug_info_path : canonical_path;

    SymbolIndex::Object object;
    object.elf = std::make_unique<Elf>(object_name);
    object.address_begin = reinterpret_cast<const void *>(info->dlpi_addr);
    object.address_end = reinterpret_cast<const void *>(info->dlpi_addr + object.elf->size());
    object.name = object_name;
    objects.push_back(std::move(object));

    searchAndCollectSymbolsFromELFSymbolTable(info, *objects.back().elf, SHT_SYMTAB, ".strtab", symbols);

    /// Unneeded because they were parsed from "program headers" of loaded objects.
    //searchAndCollectSymbolsFromELFSymbolTable(info, *objects.back().elf, SHT_DYNSYM, ".dynstr", symbols);
}


/* Callback for dl_iterate_phdr.
 * Is called by dl_iterate_phdr for every loaded shared lib until something
 * else than 0 is returned by one call of this function.
 */
int collectSymbols(dl_phdr_info * info, size_t, void * data_ptr)
{
    SymbolIndex::Data & data = *reinterpret_cast<SymbolIndex::Data *>(data_ptr);

    collectSymbolsFromProgramHeaders(info, data.symbols);
    collectSymbolsFromELF(info, data.symbols, data.objects);

    /* Continue iterations */
    return 0;
}


template <typename T>
const T * find(const void * address, const std::vector<T> & vec)
{
    /// First range that has left boundary greater than address.

    auto it = std::lower_bound(vec.begin(), vec.end(), address,
        [](const T & symbol, const void * addr) { return symbol.address_begin <= addr; });

    if (it == vec.begin())
        return nullptr;
    else
        --it; /// Last range that has left boundary less or equals than address.

    if (address >= it->address_begin && address < it->address_end)
        return &*it;
    else
        return nullptr;
}

}


void SymbolIndex::update()
{
    dl_iterate_phdr(collectSymbols, &data.symbols);

    std::sort(data.objects.begin(), data.objects.end(), [](const Object & a, const Object & b) { return a.address_begin < b.address_begin; });
    std::sort(data.symbols.begin(), data.symbols.end(), [](const Symbol & a, const Symbol & b) { return a.address_begin < b.address_begin; });

    /// We found symbols both from loaded program headers and from ELF symbol tables.
    data.symbols.erase(std::unique(data.symbols.begin(), data.symbols.end(), [](const Symbol & a, const Symbol & b)
    {
        return a.address_begin == b.address_begin && a.address_end == b.address_end;
    }), data.symbols.end());
}

const SymbolIndex::Symbol * SymbolIndex::findSymbol(const void * address) const
{
    return find(address, data.symbols);
}

const SymbolIndex::Object * SymbolIndex::findObject(const void * address) const
{
    return find(address, data.objects);
}

}
