#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mws {
// Resource-less DXVK builds still export the annotation ABI at these ordinals.
// Parse the file without loading or executing a candidate DLL.
inline bool hasDxvkD3D9Exports(const std::filesystem::path& path, bool* refxOut = nullptr) {
  if (refxOut) *refxOut = false;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return false;
  const auto size = file.tellg();
  if (size < 256 || size > 128 * 1024 * 1024) return false;
  std::vector<unsigned char> data(static_cast<size_t>(size));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(data.data()), size)) return false;
  const auto read16 = [&](size_t pos) -> uint16_t {
    uint16_t v = 0;
    if (pos <= data.size() && data.size() - pos >= sizeof(v))
      std::memcpy(&v, data.data() + pos, sizeof(v));
    return v;
  };
  const auto read32 = [&](size_t pos) -> uint32_t {
    uint32_t v = 0;
    if (pos <= data.size() && data.size() - pos >= sizeof(v))
      std::memcpy(&v, data.data() + pos, sizeof(v));
    return v;
  };
  const size_t pe = read32(0x3c);
  if (read16(0) != 0x5a4d || pe > data.size() - 24 ||
      read32(pe) != 0x4550 || read16(pe + 4) != 0x14c ||
      !(read16(pe + 22) & 0x2000)) return false;
  const size_t optional = pe + 24;
  const size_t optionalSize = read16(pe + 20);
  if (optionalSize < 104 || optionalSize > data.size() - optional ||
      read16(optional) != 0x10b || read32(optional + 92) == 0) return false;
  const size_t sections = optional + optionalSize;
  const size_t sectionCount = read16(pe + 6);
  if (sectionCount > (data.size() - sections) / 40) return false;
  const auto offset = [&](uint32_t rva, size_t length) -> size_t {
    for (size_t i = 0; i < sectionCount; ++i) {
      const size_t section = sections + i * 40;
      const uint32_t start = read32(section + 12);
      const uint32_t rawSize = read32(section + 16);
      if (rva < start || rva - start >= rawSize) continue;
      const size_t delta = rva - start;
      const size_t raw = read32(section + 20);
      if (length <= rawSize - delta && raw <= data.size() &&
          delta <= data.size() - raw && length <= data.size() - raw - delta)
        return raw + delta;
    }
    return data.size();
  };
  const size_t exports = offset(read32(optional + 96), 40);
  if (exports == data.size()) return false;
  const uint32_t base = read32(exports + 16);
  const uint32_t functionCount = read32(exports + 20);
  const uint32_t nameCount = read32(exports + 24);
  if (functionCount > data.size() / 4 || nameCount > data.size() / 4) return false;
  const size_t functions = offset(read32(exports + 28), size_t(functionCount) * 4);
  const size_t names = offset(read32(exports + 32), size_t(nameCount) * 4);
  if (functions == data.size() || names == data.size()) return false;
  bool create9 = false;
  bool refxExport = false;
  for (size_t i = 0; i < nameCount; ++i) {
    const size_t name = offset(read32(names + i * 4), 1);
    if (name == data.size()) continue;
    const auto* begin = reinterpret_cast<const char*>(data.data() + name);
    const auto* end = static_cast<const char*>(std::memchr(begin, 0, data.size() - name));
    if (!end) continue;
    const std::string text(begin, end);
    create9 |= text == "Direct3DCreate9";
    refxExport |= text == "ReFX_DXVK_SubmitSunlightFrame";
  }
  const auto ordinalPresent = [&](uint32_t ordinal) {
    return ordinal >= base && ordinal - base < functionCount &&
        read32(functions + size_t(ordinal - base) * 4) != 0;
  };
  if (refxOut) *refxOut = create9 && refxExport;
  return create9 && (refxExport || (ordinalPresent(28257) && ordinalPresent(28258)));
}
} // namespace mws
