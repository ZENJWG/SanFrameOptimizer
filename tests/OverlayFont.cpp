#include "mws/OverlayFont.hpp"
#include "resource.h"
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int wmain(int argc, wchar_t** argv) {
    HMODULE resourceModule{};
    std::filesystem::path renamed;
    try {
        check(argc == 4, "ASI/font test arguments missing");
        const bool embedded = std::wstring(argv[2]) == L"TRUE";
        check(mws::OverlayFont::containingModule(reinterpret_cast<const void*>(&check)) == GetModuleHandleW(nullptr),
              "module lookup does not identify the code's actual owner");
        // Load resources only: never execute DllMain or install game hooks.
        renamed = std::filesystem::path(argv[1]).parent_path() / L"font-test-renamed.asi";
        std::filesystem::copy_file(argv[1], renamed, std::filesystem::copy_options::overwrite_existing);
        resourceModule = LoadLibraryExW(renamed.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
        check(resourceModule != nullptr, "renamed ASI resources could not be loaded");
        ImGui::CreateContext();
        auto& atlas = *ImGui::GetIO().Fonts;
        auto* font = mws::OverlayFont::addEmbedded(atlas, resourceModule, IDR_BANKGOTHIC_BOLD_TTF, 28.0f);
        check((font != nullptr) == embedded, "embedded resource missing or build policy not respected");
        if (embedded) {
            std::ifstream input(std::filesystem::path(argv[3]), std::ios::binary);
            const std::vector<char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            const auto* source = font->Sources[0];
            check(!bytes.empty() && source->FontDataSize == static_cast<int>(bytes.size()) &&
                  !std::memcmp(source->FontData, bytes.data(), bytes.size()), "embedded TTF differs from local font");
            check(!source->FontDataOwnedByAtlas, "resource data incorrectly owned by ImGui");
            const auto* glyph = font->GetFontBaked(28.0f)->FindGlyph('A');
            check(glyph && glyph->AdvanceX > 0, "embedded font cannot bake real glyphs");
        }
        auto* fallback = mws::OverlayFont::addFallback(atlas, 28.0f);
        check(fallback && fallback->LegacySize == 28.0f, "fallback ignores configured font size");
        check(fallback->GetFontBaked(28.0f)->FindGlyph('A') != nullptr, "fallback cannot bake glyphs");
        ImGui::DestroyContext(); // Borrowed font resource must not be freed.
        FreeLibrary(resourceModule); resourceModule = nullptr;
        std::filesystem::remove(renamed);
        std::cout << "Renamed ASI font resource, glyph baking, lifetime and fallback size checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        if (resourceModule) FreeLibrary(resourceModule);
        if (!renamed.empty()) { std::error_code unused; std::filesystem::remove(renamed, unused); }
        std::cerr << error.what() << '\n'; return 1;
    }
}
