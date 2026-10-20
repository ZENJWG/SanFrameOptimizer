#include "mws/DxvkConfigFile.hpp"
#include <Windows.h>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <vector>

namespace mws {
namespace {
std::filesystem::path ownershipPath(const std::filesystem::path& path) {
  auto result = path;
  result += L".SanFrameOptimizer.ownership";
  return result;
}
std::string readFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot read " + path.string());
  std::string text(std::istreambuf_iterator<char>(file), {});
  if (file.bad()) throw std::runtime_error("failed reading " + path.string());
  return text;
}
void atomicWrite(const std::filesystem::path& path, const std::string& text) {
  wchar_t name[MAX_PATH]{};
  if (!GetTempFileNameW(path.parent_path().c_str(), L"SFD", 0, name))
    throw std::runtime_error("cannot create temporary config file");
  const std::filesystem::path temporary(name);
  try {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!file) throw std::runtime_error("cannot write temporary config file");
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("cannot replace config file, Windows error " + std::to_string(GetLastError()));
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}
struct Ownership {
  std::string executable, before, after;
  DxvkValues keys;
};
Ownership readOwnership(const std::filesystem::path& path) {
  std::istringstream stream(readFile(path));
  std::string magic;
  std::getline(stream, magic);
  Ownership record;
  size_t count = 0;
  if (magic != "SANFRAME_DXVK_OWNERSHIP_V1" ||
      !(stream >> std::quoted(record.executable) >> std::quoted(record.before) >>
        std::quoted(record.after) >> count) || count > 64)
    throw std::runtime_error("invalid DXVK ownership record; config left unchanged");
  for (size_t i = 0; i < count; ++i) {
    std::string key;
    if (!(stream >> std::quoted(key))) throw std::runtime_error("truncated DXVK ownership record");
    record.keys.emplace(key, "");
  }
  return record;
}
std::string serialize(const Ownership& record) {
  std::ostringstream stream;
  stream << "SANFRAME_DXVK_OWNERSHIP_V1\n" << std::quoted(record.executable) << '\n'
      << std::quoted(record.before) << '\n' << std::quoted(record.after) << '\n'
      << record.keys.size() << '\n';
  for (const auto& [key, value] : record.keys) stream << std::quoted(key) << '\n';
  return stream.str();
}
struct Line {
  std::string text, identity;
};
std::vector<Line> lines(const std::string& input, const Ownership& record) {
  std::vector<Line> result;
  std::string section;
  std::map<std::string, unsigned> occurrences;
  size_t position = 0;
  while (position < input.size()) {
    auto end = input.find('\n', position);
    if (end == std::string::npos) end = input.size(); else ++end;
    Line line{input.substr(position, end - position), {}};
    const auto first = line.text.find_first_not_of(" \t\r\n",
        line.text.starts_with("\xef\xbb\xbf") ? 3 : 0);
    if (first != std::string::npos && line.text[first] == '[') {
      const auto close = line.text.find(']', first);
      section = close == std::string::npos ? "<invalid>" : line.text.substr(first + 1, close - first - 1);
    } else if (first != std::string::npos && (section.empty() || section == record.executable)) {
      const auto equal = line.text.find('=', first);
      if (equal != std::string::npos && equal > first) {
        const auto last = line.text.find_last_not_of(" \t", equal - 1);
        const auto key = line.text.substr(first, last - first + 1);
        if (record.keys.contains(key)) {
          const auto identity = section + "\x1f" + key;
          line.identity = identity + "\x1f" + std::to_string(occurrences[identity]++);
        }
      }
    }
    result.push_back(std::move(line));
    position = end;
  }
  return result;
}
std::string restoreOwned(const std::string& current, const Ownership& record, DxvkRestoreResult& result) {
  std::map<std::string, std::string> before, after;
  for (const auto& line : lines(record.before, record))
    if (!line.identity.empty()) before[line.identity] = line.text;
  for (const auto& line : lines(record.after, record))
    if (!line.identity.empty()) after[line.identity] = line.text;
  const auto currentLines = lines(current, record);
  const auto group = [](const std::string& identity) { return identity.substr(0, identity.rfind('\x1f')); };
  std::map<std::string, unsigned> appliedCounts, currentCounts;
  for (const auto& [identity, text] : after) ++appliedCounts[group(identity)];
  for (const auto& line : currentLines)
    if (!line.identity.empty()) ++currentCounts[group(line.identity)];
  std::string restored;
  for (const auto& line : currentLines) {
    const auto previous = before.find(line.identity);
    const auto applied = after.find(line.identity);
    const bool owned = !line.identity.empty() && applied != after.end() &&
        (previous == before.end() || previous->second != applied->second);
    // A user-added/removed duplicate makes occurrence matching ambiguous.
    // Preserve that key's current assignments instead of guessing ownership.
    if (owned && line.text == applied->second &&
        appliedCounts[group(line.identity)] == currentCounts[group(line.identity)]) {
      if (previous != before.end()) {
        restored += previous->second;
        if (!previous->second.empty() && previous->second.back() != '\n' && line.text.back() == '\n')
          restored += '\n';
      }
      ++result.restored;
    } else {
      restored += line.text;
      if (owned) ++result.preserved;
    }
  }
  return restored;
}
} // namespace

std::string mergeDxvkConfig(const std::string& input, const DxvkValues& values,
    const std::string& executable) {
  if (input.size() >= 2 && ((input[0] == '\xff' && input[1] == '\xfe') ||
      (input[0] == '\xfe' && input[1] == '\xff')))
    throw std::runtime_error("UTF-16 dxvk.conf is unsupported; convert to UTF-8 first");
  const std::string newline = input.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::string output;
  std::set<std::string> found;
  bool active = true;
  size_t firstSection = std::string::npos;
  // DXVK's parser does not consume a UTF-8 BOM.
  size_t pos = input.starts_with("\xef\xbb\xbf") ? 3 : 0;
  while (pos < input.size()) {
    const auto end = input.find('\n', pos);
    const auto count = (end == std::string::npos ? input.size() : end) - pos;
    std::string line = input.substr(pos, count);
    const auto start = line.find_first_not_of(" \t\r");
    if (start != std::string::npos && line[start] == '[') {
      if (firstSection == std::string::npos) firstSection = output.size();
      const auto close = line.rfind(']');
      active = close != std::string::npos && close > start &&
          line.substr(start + 1, close - start - 1) == executable;
    } else if (active && start != std::string::npos) {
      const auto equal = line.find('=', start);
      if (equal != std::string::npos) {
        const auto keyEnd = line.find_last_not_of(" \t", equal - 1);
        const auto it = values.find(line.substr(start, keyEnd - start + 1));
        if (it != values.end()) {
          found.insert(it->first);
          auto valueStart = line.find_first_not_of(" \t", equal + 1);
          if (valueStart == std::string::npos) valueStart = line.size();
          auto valueEnd = line.find_first_of(" \t\r#", valueStart);
          if (valueEnd == std::string::npos) valueEnd = line.size();
          line.replace(valueStart, valueEnd - valueStart, it->second);
        }
      }
    }
    output += line;
    if (end != std::string::npos) output += '\n';
    pos = end == std::string::npos ? input.size() : end + 1;
  }
  std::string missing;
  for (const auto& [key, value] : values)
    if (!found.contains(key)) missing += key + " = " + value + newline;
  if (!missing.empty()) {
    if (firstSection != std::string::npos) output.insert(firstSection, missing);
    else {
      if (!output.empty() && output.back() != '\n') output += newline;
      output += missing;
    }
  }
  return output;
}

std::string restoreDxvkOverrides(const std::string& current, const std::string& before,
    const std::string& after, const DxvkValues& keys) {
  const auto convert = [](std::string text) {
    for (char& c : text) if (c == ';') c = '\n';
    if (!text.empty() && text.back() != '\n') text += '\n';
    return text;
  };
  Ownership record{"", convert(before), convert(after), keys};
  DxvkRestoreResult ignored;
  auto result = restoreOwned(convert(current), record, ignored);
  if (!result.empty() && result.back() == '\n') result.pop_back();
  for (char& c : result) if (c == '\n') c = ';';
  return result;
}

bool writeDxvkConfig(const std::filesystem::path& path, const DxvkValues& values,
    const std::string& executable, std::string& error) {
  try {
    const bool exists = std::filesystem::exists(path);
    const std::string original = exists ? readFile(path) : "";
    const auto merged = mergeDxvkConfig(original, values, executable);
    if (merged == original) return true;
    Ownership record{executable, original, merged, values};
    const auto journal = ownershipPath(path);
    if (std::filesystem::exists(journal)) {
      const auto previous = readOwnership(journal);
      if (previous.executable != executable) throw std::runtime_error("DXVK ownership executable mismatch");
      DxvkRestoreResult ignored;
      record.before = restoreOwned(original, previous, ignored);
      record.keys.insert(previous.keys.begin(), previous.keys.end());
    }
    if (exists) {
      auto backup = path; backup += L".SanFrameOptimizer.bak";
      if (!std::filesystem::exists(backup)) std::filesystem::copy_file(path, backup);
    }
    // Record ownership before modifying dxvk.conf. A failed config replacement
    // leaves a recoverable record; restoration compares exact applied lines.
    atomicWrite(journal, serialize(record));
    atomicWrite(path, merged);
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

bool restoreDxvkConfig(const std::filesystem::path& path, const std::string& executable,
    DxvkRestoreResult& result, std::string& error) {
  result = {};
  try {
    const auto journal = ownershipPath(path);
    if (!std::filesystem::exists(journal)) {
      auto backup = path; backup += L".SanFrameOptimizer.bak";
      result.legacyBackup = std::filesystem::exists(backup);
      return true;
    }
    const auto record = readOwnership(journal);
    if (record.executable != executable) throw std::runtime_error("DXVK ownership executable mismatch");
    if (std::filesystem::exists(path)) {
      const auto current = readFile(path);
      const auto restored = restoreOwned(current, record, result);
      if (restored != current) atomicWrite(path, restored);
    }
    std::filesystem::remove(journal);
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}
} // namespace mws
