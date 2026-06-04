#pragma once

#include <string_view>
#include <cstdint>

// Initialize the ELF symbol resolver by loading libart.so's symbol tables.
// Must be called once before using ArtSymbolResolver / ArtSymbolPrefixResolver.
// Returns true on success.
bool InitElfSymbolResolver();

// Resolve an exact symbol name from libart.so.
// First tries dlsym (for .dynsym exports), then falls back to ELF .symtab parsing.
// Returns the symbol address or nullptr if not found.
void *ArtSymbolResolver(std::string_view symbol_name);

// Resolve the first symbol whose name starts with the given prefix.
// Searches both .dynsym and .symtab.
// Returns the symbol address or nullptr if not found.
void *ArtSymbolPrefixResolver(std::string_view symbol_prefix);
