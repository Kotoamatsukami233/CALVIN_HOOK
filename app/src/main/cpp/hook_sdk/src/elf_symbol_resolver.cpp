#include "elf_symbol_resolver.h"
#include "logging.h"
#include "xz/xz_dec.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#if defined(__LP64__)
using ElfW_Ehdr = Elf64_Ehdr;
using ElfW_Shdr = Elf64_Shdr;
using ElfW_Sym  = Elf64_Sym;
using ElfW_Phdr = Elf64_Phdr;
using ElfW_Dyn  = Elf64_Dyn;
#else
using ElfW_Ehdr = Elf32_Ehdr;
using ElfW_Shdr = Elf32_Shdr;
using ElfW_Sym  = Elf32_Sym;
using ElfW_Phdr = Elf32_Phdr;
using ElfW_Dyn  = Elf32_Dyn;
#endif

struct SymbolEntry {
    std::string name;
    uintptr_t addr;
};

static std::vector<SymbolEntry> g_symbols;

// Parse /proc/self/maps to find the first mapping (offset==0) of a library.
static bool FindLibraryInfo(const char *lib_name, uintptr_t *out_base,
                            std::string &out_path) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return false;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, lib_name)) continue;
        unsigned long start, end, offset;
        char perms[5] = {};
        char pathname[256] = {};
        int parsed = sscanf(line, "%lx-%lx %4s %lx %*x:%*x %*u %255[^\n]",
                            &start, &end, perms, &offset, pathname);
        if (parsed < 4) continue;
        if (offset != 0) continue;
        *out_base = static_cast<uintptr_t>(start);
        out_path = pathname;
        while (!out_path.empty() &&
               (out_path.back() == '\n' || out_path.back() == '\r' || out_path.back() == ' '))
            out_path.pop_back();
        fclose(fp);
        return true;
    }
    fclose(fp);
    return false;
}

// Compute load_bias from PT_LOAD segments.
static uintptr_t ComputeLoadBias(const void *file_map, uintptr_t base) {
    auto *ehdr = static_cast<const ElfW_Ehdr *>(file_map);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        auto *phdr = reinterpret_cast<const ElfW_Phdr *>(
            static_cast<const uint8_t *>(file_map) + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type == PT_LOAD) {
            return base - phdr->p_vaddr;
        }
    }
    return base;
}

// Parse .symtab or .dynsym from section headers.
static bool ParseSymtabFromSections(const void *file_map, uintptr_t load_bias,
                                    bool require_symtab) {
    auto *ehdr = static_cast<const ElfW_Ehdr *>(file_map);
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) return false;

    auto *shdrs = reinterpret_cast<const ElfW_Shdr *>(
        static_cast<const uint8_t *>(file_map) + ehdr->e_shoff);

    // Try SHT_SYMTAB first, then SHT_DYNSYM
    for (int target_type : {SHT_SYMTAB, SHT_DYNSYM}) {
        if (require_symtab && target_type != SHT_SYMTAB) continue;

        const ElfW_Shdr *sym_sh = nullptr;
        const ElfW_Shdr *str_sh = nullptr;

        for (int i = 0; i < ehdr->e_shnum; i++) {
            if (shdrs[i].sh_type == target_type) {
                sym_sh = &shdrs[i];
                str_sh = &shdrs[shdrs[i].sh_link];
                break;
            }
        }
        if (!sym_sh || !str_sh) continue;

        const char *strtab = static_cast<const char *>(file_map) + str_sh->sh_offset;
        const auto *syms = reinterpret_cast<const ElfW_Sym *>(
            static_cast<const uint8_t *>(file_map) + sym_sh->sh_offset);
        size_t count = sym_sh->sh_size / sym_sh->sh_entsize;

        for (size_t i = 0; i < count; i++) {
            if (syms[i].st_value == 0 || syms[i].st_shndx == SHN_UNDEF) continue;
            uint8_t type = syms[i].st_info & 0xf;
            if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) continue;
            const char *name = strtab + syms[i].st_name;
            if (!name || name[0] == '\0') continue;
            g_symbols.push_back({name, load_bias + syms[i].st_value});
        }

        LOGI("Parsed %s: %zu symbols from section headers",
             target_type == SHT_SYMTAB ? ".symtab" : ".dynsym", g_symbols.size());
        return !g_symbols.empty();
    }
    return false;
}

// Parse .dynsym via PT_DYNAMIC (works even when section headers are stripped).
static bool ParseDynsymFromDynamic(const void *file_map, size_t file_size,
                                   uintptr_t load_bias) {
    auto *ehdr = static_cast<const ElfW_Ehdr *>(file_map);

    // Find PT_DYNAMIC
    const ElfW_Phdr *dyn_phdr = nullptr;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        auto *phdr = reinterpret_cast<const ElfW_Phdr *>(
            static_cast<const uint8_t *>(file_map) + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type == PT_DYNAMIC) {
            dyn_phdr = phdr;
            break;
        }
    }
    if (!dyn_phdr) return false;

    // Build vaddr-to-file-offset converter from PT_LOAD segments
    auto vaddr_to_foff = [&](uintptr_t vaddr) -> off_t {
        for (int i = 0; i < ehdr->e_phnum; i++) {
            auto *phdr = reinterpret_cast<const ElfW_Phdr *>(
                static_cast<const uint8_t *>(file_map) + ehdr->e_phoff + i * ehdr->e_phentsize);
            if (phdr->p_type == PT_LOAD &&
                vaddr >= phdr->p_vaddr &&
                vaddr < phdr->p_vaddr + phdr->p_memsz) {
                return static_cast<off_t>(phdr->p_offset + (vaddr - phdr->p_vaddr));
            }
        }
        return -1;
    };

    // Parse dynamic entries
    auto *dyn = reinterpret_cast<const ElfW_Dyn *>(
        static_cast<const uint8_t *>(file_map) + dyn_phdr->p_offset);

    uintptr_t dt_symtab = 0, dt_strtab = 0, dt_hash = 0, dt_gnu_hash = 0;
    for (auto *d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_SYMTAB:   dt_symtab = d->d_un.d_ptr; break;
            case DT_STRTAB:   dt_strtab = d->d_un.d_ptr; break;
            case DT_HASH:     dt_hash = d->d_un.d_ptr; break;
            case DT_GNU_HASH: dt_gnu_hash = d->d_un.d_ptr; break;
        }
    }
    if (!dt_symtab || !dt_strtab) return false;

    off_t symtab_foff = vaddr_to_foff(dt_symtab);
    off_t strtab_foff = vaddr_to_foff(dt_strtab);
    if (symtab_foff < 0 || strtab_foff < 0) return false;

    // Determine symbol count from hash table
    size_t sym_count = 0;
    if (dt_hash) {
        off_t hash_foff = vaddr_to_foff(dt_hash);
        if (hash_foff >= 0) {
            auto *hash_data = reinterpret_cast<const uint32_t *>(
                static_cast<const uint8_t *>(file_map) + hash_foff);
            sym_count = hash_data[1]; // nchain
        }
    } else if (dt_gnu_hash) {
        off_t ghash_foff = vaddr_to_foff(dt_gnu_hash);
        if (ghash_foff >= 0) {
            auto *ghash = reinterpret_cast<const uint32_t *>(
                static_cast<const uint8_t *>(file_map) + ghash_foff);
            uint32_t nbuckets = ghash[0];
            uint32_t sym_offset = ghash[1];
            uint32_t bloom_size = ghash[2];
            const uint32_t *buckets = ghash + 4 + bloom_size;
            const uint32_t *chains = buckets + nbuckets;

            uint32_t max_sym = 0;
            for (uint32_t i = 0; i < nbuckets; i++) {
                if (buckets[i] > max_sym) max_sym = buckets[i];
            }
            if (max_sym == 0) return false;
            for (uint32_t i = max_sym - sym_offset; ; i++) {
                if (chains[i] & 1) {
                    sym_count = sym_offset + i + 1;
                    break;
                }
            }
        }
    }
    if (sym_count == 0) return false;

    auto *syms = reinterpret_cast<const ElfW_Sym *>(
        static_cast<const uint8_t *>(file_map) + symtab_foff);
    const char *strtab = reinterpret_cast<const char *>(
        static_cast<const uint8_t *>(file_map) + strtab_foff);

    for (size_t i = 0; i < sym_count; i++) {
        if (syms[i].st_value == 0 || syms[i].st_shndx == SHN_UNDEF) continue;
        uint8_t type = syms[i].st_info & 0xf;
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) continue;
        const char *name = strtab + syms[i].st_name;
        if (!name || name[0] == '\0') continue;
        g_symbols.push_back({name, load_bias + syms[i].st_value});
    }

    LOGI("Parsed .dynsym via PT_DYNAMIC: %zu symbols", g_symbols.size());
    return !g_symbols.empty();
}

// Parse .symtab from a memory buffer (used for decompressed .gnu_debugdata).
static bool ParseSymtabFromMemory(const void *elf_data, size_t elf_size, uintptr_t load_bias) {
    auto *ehdr = static_cast<const ElfW_Ehdr *>(elf_data);
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) return false;
    if (ehdr->e_shoff + (size_t)ehdr->e_shnum * ehdr->e_shentsize > elf_size) return false;

    auto *shdrs = reinterpret_cast<const ElfW_Shdr *>(
        static_cast<const uint8_t *>(elf_data) + ehdr->e_shoff);

    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type != SHT_SYMTAB) continue;
        const ElfW_Shdr *sym_sh = &shdrs[i];
        if (sym_sh->sh_link >= (size_t)ehdr->e_shnum) break;
        const ElfW_Shdr *str_sh = &shdrs[sym_sh->sh_link];

        if (sym_sh->sh_offset + sym_sh->sh_size > elf_size) break;
        if (str_sh->sh_offset + str_sh->sh_size > elf_size) break;

        const char *strtab = static_cast<const char *>(elf_data) + str_sh->sh_offset;
        const auto *syms = reinterpret_cast<const ElfW_Sym *>(
            static_cast<const uint8_t *>(elf_data) + sym_sh->sh_offset);
        size_t count = sym_sh->sh_size / sym_sh->sh_entsize;

        for (size_t j = 0; j < count; j++) {
            if (syms[j].st_value == 0 || syms[j].st_shndx == SHN_UNDEF) continue;
            uint8_t type = syms[j].st_info & 0xf;
            if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) continue;
            const char *name = strtab + syms[j].st_name;
            if (!name || name[0] == '\0') continue;
            g_symbols.push_back({name, load_bias + syms[j].st_value});
        }

        LOGI("Parsed .symtab from .gnu_debugdata: %zu symbols", g_symbols.size());
        return !g_symbols.empty();
    }
    return false;
}

// Try to extract and parse .gnu_debugdata (XZ-compressed mini-debuginfo).
static bool ParseGnuDebugdata(const void *file_map, size_t file_size, uintptr_t load_bias) {
    auto *ehdr = static_cast<const ElfW_Ehdr *>(file_map);
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) return false;
    if (ehdr->e_shoff + (size_t)ehdr->e_shnum * ehdr->e_shentsize > file_size) return false;

    auto *shdrs = reinterpret_cast<const ElfW_Shdr *>(
        static_cast<const uint8_t *>(file_map) + ehdr->e_shoff);

    // Find .gnu_debugdata section
    // Need section name string table
    if (ehdr->e_shstrndx >= ehdr->e_shnum) return false;
    const ElfW_Shdr *shstrtab_sh = &shdrs[ehdr->e_shstrndx];
    if (shstrtab_sh->sh_offset + shstrtab_sh->sh_size > file_size) return false;
    const char *shstrtab = static_cast<const char *>(file_map) + shstrtab_sh->sh_offset;

    const ElfW_Shdr *debugdata_sh = nullptr;
    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char *name = shstrtab + shdrs[i].sh_name;
        if (strcmp(name, ".gnu_debugdata") == 0) {
            debugdata_sh = &shdrs[i];
            break;
        }
    }
    if (!debugdata_sh) return false;
    if (debugdata_sh->sh_offset + debugdata_sh->sh_size > file_size) return false;

    LOGI("Found .gnu_debugdata at offset %lu size %lu",
         (unsigned long)debugdata_sh->sh_offset, (unsigned long)debugdata_sh->sh_size);

    const uint8_t *xz_data = static_cast<const uint8_t *>(file_map) + debugdata_sh->sh_offset;
    size_t xz_size = debugdata_sh->sh_size;

    // Query decompressed size first
    size_t dec_size = xz_decompress(xz_data, xz_size, nullptr, 0);
    if (dec_size == 0) {
        LOGE("Failed to query .gnu_debugdata decompressed size");
        return false;
    }

    LOGI("Decompressing .gnu_debugdata: %zu -> %zu bytes", xz_size, dec_size);

    auto *dec_buf = static_cast<uint8_t *>(malloc(dec_size));
    if (!dec_buf) return false;

    size_t actual = xz_decompress(xz_data, xz_size, dec_buf, dec_size);
    if (actual == 0) {
        LOGE("XZ decompression failed");
        free(dec_buf);
        return false;
    }

    // Verify it's a valid ELF
    if (actual < sizeof(ElfW_Ehdr) || memcmp(dec_buf, ELFMAG, SELFMAG) != 0) {
        LOGE("Decompressed data is not a valid ELF");
        free(dec_buf);
        return false;
    }

    bool ok = ParseSymtabFromMemory(dec_buf, actual, load_bias);
    free(dec_buf);
    return ok;
}

// Try to load ELF symbols from a specific file path.
static bool TryLoadFromFile(const std::string &path, uintptr_t load_bias) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    off_t file_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    void *map = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return false;

    auto *ehdr = static_cast<const ElfW_Ehdr *>(map);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        munmap(map, file_size);
        return false;
    }

    LOGI("Reading ELF: %s (shoff=%lu shnum=%d)", path.c_str(),
         (unsigned long)ehdr->e_shoff, ehdr->e_shnum);

    bool ok = false;

    // Method 1: .symtab via section headers
    if (ehdr->e_shoff > 0 && ehdr->e_shnum > 0) {
        ok = ParseSymtabFromSections(map, load_bias, true);
    }

    // Method 2: .gnu_debugdata (XZ-compressed .symtab) — high priority, has non-exported symbols
    if (!ok) {
        ok = ParseGnuDebugdata(map, file_size, load_bias);
    }

    // Method 3: .dynsym via section headers (only exported symbols, fallback)
    if (!ok && ehdr->e_shoff > 0 && ehdr->e_shnum > 0) {
        ok = ParseSymtabFromSections(map, load_bias, false);
    }

    // Method 4: .dynsym via PT_DYNAMIC (last resort)
    if (!ok) {
        ok = ParseDynsymFromDynamic(map, file_size, load_bias);
    }

    munmap(map, file_size);
    return ok;
}

bool InitElfSymbolResolver() {
    uintptr_t base = 0;
    std::string primary_path;
    if (!FindLibraryInfo("libart.so", &base, primary_path)) {
        LOGE("Failed to find libart.so in /proc/self/maps");
        return false;
    }
    LOGI("libart.so base: %p path: %s", (void *)base, primary_path.c_str());

    // Compute load_bias from the in-memory ELF
    auto *mem_ehdr = reinterpret_cast<const ElfW_Ehdr *>(base);
    uintptr_t load_bias = base;
    for (int i = 0; i < mem_ehdr->e_phnum; i++) {
        auto *phdr = reinterpret_cast<const ElfW_Phdr *>(
            base + mem_ehdr->e_phoff + i * mem_ehdr->e_phentsize);
        if (phdr->p_type == PT_LOAD) {
            load_bias = base - phdr->p_vaddr;
            break;
        }
    }

    // Try loading from multiple possible paths (ordered by likelihood of having .symtab)
    std::vector<std::string> paths = {
        primary_path,
        "/apex/com.android.art/debug/lib64/libart.so",
        "/apex/com.android.art/lib64/libart.so",
        "/system/lib64/libart.so",
    };

    for (const auto &path : paths) {
        if (path == primary_path || !path.empty()) {
            size_t before = g_symbols.size();
            if (TryLoadFromFile(path, load_bias)) {
                LOGI("Loaded symbols from %s (%zu total)", path.c_str(), g_symbols.size());
                return true;
            }
        }
    }

    LOGW("Could not find .symtab in any libart.so path; relying on dlsym only");
    return true;  // Non-fatal: dlsym may still resolve many symbols
}

// Extract the last name component from a C++ mangled symbol.
// e.g., "RegisterNative" from "_ZN3art11ClassLinker14RegisterNativeE..."
static std::string ExtractMethodName(std::string_view mangled) {
    if (mangled.size() < 4 || mangled[0] != '_' || mangled[1] != 'Z') return {};
    // Skip _ZN
    auto e_pos = mangled.find('E');
    if (e_pos == std::string_view::npos) return {};
    auto qualified = mangled.substr(3, e_pos - 3); // skip _ZN, take up to E
    size_t pos = 0;
    std::string_view last_name;
    while (pos < qualified.size()) {
        if (!std::isdigit(static_cast<unsigned char>(qualified[pos]))) break;
        size_t len_start = pos;
        while (pos < qualified.size() && std::isdigit(static_cast<unsigned char>(qualified[pos]))) pos++;
        if (pos == len_start) break;
        int len = 0;
        for (size_t i = len_start; i < pos; i++) {
            len = len * 10 + (qualified[i] - '0');
        }
        if (pos + static_cast<size_t>(len) > qualified.size()) break;
        last_name = qualified.substr(pos, len);
        pos += static_cast<size_t>(len);
    }
    return std::string(last_name);
}

void *ArtSymbolResolver(std::string_view symbol_name) {
    // 1. dlsym
    void *addr = dlsym(RTLD_DEFAULT, symbol_name.data());
    if (addr) return addr;

    // 2. Exact match from parsed symbol table
    for (const auto &sym : g_symbols) {
        if (sym.name == symbol_name) {
            return reinterpret_cast<void *>(sym.addr);
        }
    }

    // 3. Prefix match for symbols with compiler-added unique suffixes (.__uniq.XXXXX)
    for (const auto &sym : g_symbols) {
        if (sym.name.compare(0, symbol_name.size(), symbol_name) == 0) {
            return reinterpret_cast<void *>(sym.addr);
        }
    }

    // 4. Fuzzy match: extract method name and search for any symbol containing it.
    // This handles Android 16+ where C++ ABI names change due to parameter/namespace refactoring.
    auto method_name = ExtractMethodName(symbol_name);
    if (!method_name.empty()) {
        void *best = nullptr;
        std::string best_name;
        for (const auto &sym : g_symbols) {
            if (sym.name.find(method_name) != std::string::npos) {
                // Prefer symbols in the art namespace
                if (sym.name.find("_ZN3art") != std::string::npos) {
                    if (!best || sym.name.size() < best_name.size()) {
                        best = reinterpret_cast<void *>(sym.addr);
                        best_name = sym.name;
                    }
                }
            }
        }
        if (best) {
            LOGW("ArtSymbolResolver: fuzzy match '%.*s' -> '%s'",
                 (int)symbol_name.size(), symbol_name.data(), best_name.c_str());
            return best;
        }
    }

    LOGW("ArtSymbolResolver: symbol not found: %.*s", (int)symbol_name.size(), symbol_name.data());
    return nullptr;
}

void *ArtSymbolPrefixResolver(std::string_view symbol_prefix) {
    void *addr = dlsym(RTLD_DEFAULT, symbol_prefix.data());
    if (addr) return addr;
    for (const auto &sym : g_symbols) {
        if (sym.name.compare(0, symbol_prefix.size(), symbol_prefix) == 0) {
            return reinterpret_cast<void *>(sym.addr);
        }
    }
    return nullptr;
}
