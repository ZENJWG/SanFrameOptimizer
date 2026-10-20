#include "mws/ExecutableInfo.hpp"
#include "mws/Logger.hpp"
#include <iomanip>
#include <sstream>

namespace mws {
ExecutableInfo ExecutableInfo::current() {
  ExecutableInfo out{};
  out.module = GetModuleHandleW(nullptr);
  out.base = out.module;
  if (!out.module) return out;

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(out.module);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;

  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      reinterpret_cast<const unsigned char*>(out.module) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return out;

  out.imageSize = nt->OptionalHeader.SizeOfImage;
  out.timeDateStamp = nt->FileHeader.TimeDateStamp;
  out.validPe = true;
  return out;
}

void ExecutableInfo::log() const {
  std::ostringstream os;
  os << "Executable PE: valid=" << (validPe ? 1 : 0)
     << ", base=0x" << std::hex << std::uppercase
     << reinterpret_cast<std::uintptr_t>(base)
     << ", imageSize=0x" << imageSize
     << ", timestamp=0x" << timeDateStamp;
  if (validPe) Logger::instance().info(os.str());
  else Logger::instance().error(os.str());
}
}
