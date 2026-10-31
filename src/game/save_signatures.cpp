// Saves from an Xbox or xemu, read as this port's own.
//
// A save's data file (a profile's tsftrpft, a MapMaker level's .mkr) ends with a 20-byte HMAC-SHA1 of the rest, which
// the game checks when it reads one and calls the save damaged when it differs. An Xbox keys it with keys of Microsoft's
// and of the console, which this port lacks; its own are all zero, so every save it writes is keyed with zeros. A save
// keyed otherwise is taken as one from elsewhere and read with the signature this port gives it, worked out from the
// file as it is on disk; the file is left as it is until the game writes it again (then keyed with zeros). A file of
// another size than the game's is left alone, damaged as it reads.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "kernel/crypto.h"
#include "kernel/kernel.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kSignature = 20;
constexpr uint64_t kLevelSize = 32756, kProfileSize = 8040;

bool EndsWith(const std::string& s, const char* end) {
    size_t n = std::strlen(end);
    return s.size() >= n && !_stricmp(s.c_str() + s.size() - n, end);
}

// the signature this port gives the save at `file` (its Xbox path), when it is one and is keyed otherwise
bool Resigned(const std::string& file, uint8_t out[kSignature]) {
    uint64_t size = EndsWith(file, ".mkr") ? kLevelSize : EndsWith(file, "\\tsftrpft") ? kProfileSize : 0;
    if (!size) return false;
    std::string host = kernel::ResolvePath(file, 0);
    std::ifstream in(fs::u8path(host), std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() != size) return false;
    static const uint8_t kKey[16] = {};
    crypto::Hmac(kKey, sizeof(kKey), data.data(), data.size() - kSignature, nullptr, 0, out);
    if (!std::memcmp(out, data.data() + data.size() - kSignature, kSignature)) return false;
    static std::mutex lock;
    static std::set<std::string> logged;
    std::lock_guard guard(lock);
    if (logged.insert(file).second) LOGR("saves: %s was signed elsewhere, read with this port's signature", file.c_str());
    return true;
}

}  // namespace

namespace game {

// game_info.cpp Loaded: the reads of the saves' last 20 bytes given this port's signature, after any patch before
void InitSaveSignatures() {
    auto before = kernel::g_read_patch;
    kernel::g_read_patch = [before](const std::string& file, uint64_t pos, uint8_t* bytes, uint32_t n) {
        if (before) before(file, pos, bytes, n);
        uint64_t size = EndsWith(file, ".mkr") ? kLevelSize : EndsWith(file, "\\tsftrpft") ? kProfileSize : 0;
        if (!size || pos + n <= size - kSignature || pos >= size) return;
        uint8_t sig[kSignature];
        if (!Resigned(file, sig)) return;
        uint64_t from = std::max(pos, size - kSignature), to = std::min(pos + n, size);
        std::memcpy(bytes + (from - pos), sig + (from - (size - kSignature)), size_t(to - from));
    };
}

}  // namespace game
