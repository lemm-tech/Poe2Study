#include "WorldArea.h"
#include <stdexcept>
#include <utility>

namespace {
std::string Utf8(const std::wstring& text) {
    const auto count = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count,
                                        nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid UTF-16 world area text");
    std::string value(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count,
                            value.data(), size, nullptr, nullptr))
        throw std::runtime_error("World area text conversion failed");
    return value;
}
struct Reader {
    const mem::GameMemory& memory;
    template<class T> T Read(uint64_t address) const {
        T value{};
        if (!memory.ReadInto(address, &value, sizeof(value)))
            throw std::runtime_error("World area field is unreadable");
        return value;
    }
    uint64_t Pointer(uint64_t address) const {
        const auto value = Read<uint64_t>(address);
        if (!memory.IsPlausible(value)) throw std::runtime_error("World area pointer unavailable");
        return value;
    }
    uint64_t World(uint64_t base) const {
        auto value = Pointer(base + BaseOffset);
        for (int i = 0; i < kWorldAreaWorldChainDepth; ++i)
            value = Pointer(value + kMapObjectChain[i]);
        return value;
    }
    uint64_t MapOwner(uint64_t world) const {
        auto value = world;
        for (int i = kWorldAreaWorldChainDepth; i <= kLocalPlayerChainDepth; ++i)
            value = Pointer(value + kMapObjectChain[i]);
        return value;
    }
    std::wstring Text(uint64_t address) const {
        std::wstring result;
        for (size_t i = 0; i < 128; ++i) {
            const auto ch = Read<wchar_t>(address + i * sizeof(wchar_t));
            if (!ch) {
                if (result.empty()) throw std::runtime_error("Empty world area text");
                return result;
            }
            if (ch < 0x20) throw std::runtime_error("Invalid world area text");
            result += ch;
        }
        throw std::runtime_error("World area text is too long or unterminated");
    }
};
} // namespace

namespace poe {
bool ReadWorldArea(const mem::GameMemory& memory, uint64_t base,
                   WorldAreaSnapshot& out, std::string& error) {
    out = {}; error.clear();
    try {
        const Reader r{memory};
        WorldAreaSnapshot value;
        value.world = r.World(base);
        value.mapOwner = r.MapOwner(value.world);
        value.record = r.Pointer(value.world + kWorldAreaRecordOff);
        const auto table = r.Pointer(value.world + kWorldAreaTableOff);
        if (r.Text(r.Pointer(table + kWorldAreaTableNameOff)) != L"Data/Balance/WorldAreas.dat")
            throw std::runtime_error("World area table identity mismatch");
        const auto idAddress = r.Pointer(value.record + kWorldAreaIdOff);
        const auto nameAddress = r.Pointer(value.record + kWorldAreaNameOff);
        value.id = Utf8(r.Text(idAddress)); value.name = Utf8(r.Text(nameAddress));
        for (const auto ch : value.id)
            if (static_cast<unsigned char>(ch) > 0x7E || ch == ' ')
                throw std::runtime_error("World area ID is not an internal ASCII identifier");
        // A loading screen can replace the world or row while strings are read.
        if (r.World(base) != value.world || r.MapOwner(value.world) != value.mapOwner ||
            r.Pointer(value.world + kWorldAreaRecordOff) != value.record ||
            r.Pointer(value.world + kWorldAreaTableOff) != table ||
            r.Pointer(value.record + kWorldAreaIdOff) != idAddress ||
            r.Pointer(value.record + kWorldAreaNameOff) != nameAddress)
            throw std::runtime_error("World area changed while reading");
        out = std::move(value);
        return true;
    } catch (const std::exception& e) {
        error = e.what(); return false;
    }
}
} // namespace poe
