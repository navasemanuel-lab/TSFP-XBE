// Mods (common/mods.h): the files of the mods beside the game served in place of the paks' own. The game finds a file
// four ways, each hooked where it is (game/briefing.cpp: FileSize 0x62CA0 and ReadFile 0x62D20; game/beta_cortez.cpp:
// MappedFile 0x624C0, a Story level's file in its pak loaded whole, and OpenStream 0x62E20, the characters streamed
// in Arcade and MapMaker): each asks File before the paks.
//
// In online co-op both machines must have the same mods switched on: a file one has and the other has not plays the
// level apart (the session's state check says so in the log).
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/mods.h"
#include "common/options.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace fs = std::filesystem;

namespace game {
std::vector<uint8_t> AdjustModel(const std::vector<uint8_t>& original, const mods::Adjust& a,
                                 std::vector<std::pair<uint32_t, uint32_t>>* changed);  // game/model_adjust.cpp
}

namespace {

struct Index {
    bool made = false;
    std::vector<game::mods::Mod> mods;
    std::map<std::string, fs::path> files;  // a pak path to the file giving it, of the mods switched on
    std::map<std::string, std::string> giver;  // a pak path to the folder of the mod giving it
    std::map<std::string, std::map<std::string, game::mods::Adjust>> adjust;  // a mod's folder: its models' adjustments
};

// a first-person model served, adjusted, and where the game read it
struct Served {
    game::mods::Adjust adjust;  // what `data` has
    std::vector<uint8_t> data;
    std::vector<uint32_t> buffers;  // the game's copies, the last read last
    uint64_t order = 0;             // when last read
};

std::mutex g_lock;
Index g_index;
std::map<fs::path, std::vector<uint8_t>> g_read;  // the files read, kept: the game may stream from them later
std::set<std::string> g_logged;
std::map<std::string, Served> g_served;
uint64_t g_reads = 0;

bool IsAdjustable(const std::string& key) {
    return key.size() > 9 && !key.compare(key.size() - 9, 9, "_anim.xbr");
}

std::string ModelName(const std::string& key) {
    size_t slash = key.rfind('/');
    return key.substr(slash + 1, key.size() - slash - 5);
}

// adjust.toml: a table per model name, [sbp500_anim] scale = 1.0 ...
void ReadAdjust(const fs::path& file, std::map<std::string, game::mods::Adjust>& out) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return;
    toml::table t;
    try {
        t = toml::parse_file(file.string());
    } catch (const toml::parse_error& e) {
        LOGR("mods: %s not read: %s", file.string().c_str(), std::string(e.description()).c_str());
        return;
    }
    for (auto&& [name, node] : t) {
        const toml::table* m = node.as_table();
        if (!m) continue;
        game::mods::Adjust a;
        auto f = [&](const char* k, float& v) {
            if (auto x = (*m)[k].value<double>()) v = float(*x);
        };
        f("scale", a.scale), f("length", a.length), f("girth", a.girth), f("yaw", a.yaw), f("pitch", a.pitch);
        f("roll", a.roll), f("x", a.x), f("y", a.y), f("z", a.z);
        out[std::string(name.str())] = a;
    }
}

void WriteAdjust(const fs::path& file, const std::map<std::string, game::mods::Adjust>& in) {
    toml::table t;
    for (const auto& [name, a] : in) {
        if (a == game::mods::Adjust{}) continue;
        t.insert(name, toml::table{{"scale", a.scale}, {"length", a.length}, {"girth", a.girth}, {"yaw", a.yaw},
                                   {"pitch", a.pitch}, {"roll", a.roll}, {"x", a.x}, {"y", a.y}, {"z", a.z}});
    }
    std::ofstream out(file);
    out << "# first-person models adjusted in the settings panel's Weapon models page\n" << t << "\n";
}

// as the paks name a file: lower case, '/', no "host0:", the loader's .raw asked for as the .xbt it is
std::string Key(std::string path) {
    if (size_t at = path.find("host0:"); at != std::string::npos) path = path.substr(at + 6);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (path.size() >= 4 && !path.compare(path.size() - 4, 4, ".raw")) path.replace(path.size() - 4, 4, ".xbt");
    return path;
}

std::set<std::string> Off() {
    std::set<std::string> off;
    std::stringstream s(game::Opt().mods_off);
    for (std::string f; std::getline(s, f, ',');)
        if (!f.empty()) off.insert(f);
    return off;
}

// mod.toml's lines `key = "value"`
void ReadAbout(const fs::path& file, game::mods::Mod& m) {
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        size_t eq = line.find('='), open = line.find('"', eq + 1), close = line.rfind('"');
        if (eq == std::string::npos || open == std::string::npos || close <= open) continue;
        std::string key = line.substr(0, eq);
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }), key.end());
        std::string value = line.substr(open + 1, close - open - 1);
        if (key == "name") m.name = value;
        else if (key == "author") m.author = value;
        else if (key == "description") m.description = value;
    }
}

void Make(Index& index) {
    index = {};
    index.made = true;
    std::error_code ec;
    fs::path root = app::ExeDir() / "mods";
    if (!fs::is_directory(root, ec)) return;
    std::vector<fs::path> folders;
    for (auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec)) folders.push_back(e.path());
    std::sort(folders.begin(), folders.end());
    std::set<std::string> off = Off();
    std::map<std::string, std::string> giver;  // a pak path to the mod giving it
    for (const fs::path& folder : folders) {
        game::mods::Mod m;
        m.folder = folder.filename().string();
        m.name = m.folder;
        m.on = !off.count(m.folder);
        ReadAbout(folder / "mod.toml", m);
        // the player's own adjust.toml, else the mod's shipped one (updates replace that, never the player's)
        if (m.on) {
            fs::path adjust = folder / "adjust.toml";
            ReadAdjust(fs::exists(adjust, ec) ? adjust : folder / "adjust.default.toml", index.adjust[m.folder]);
        }
        fs::path files = folder / "files";
        if (fs::is_directory(files, ec)) {
            for (auto& e : fs::recursive_directory_iterator(files, ec)) {
                if (!e.is_regular_file(ec)) continue;
                m.files++;
                if (!m.on) continue;
                std::string key = Key(fs::relative(e.path(), files, ec).generic_string());
                auto [it, added] = index.files.emplace(key, e.path());
                if (added) giver[key] = m.folder;
                else LOGR("mods: %s is in %s and %s, %s's is used", key.c_str(), giver[key].c_str(), m.folder.c_str(),
                          giver[key].c_str());
            }
        }
        LOGR("mods: %s (%s), %zu files, %s", m.folder.c_str(), m.name.c_str(), m.files, m.on ? "on" : "off");
        index.mods.push_back(std::move(m));
    }
    index.giver = std::move(giver);
}

game::mods::Adjust AdjustOf(const std::string& key) {
    auto g = g_index.giver.find(key);
    if (g == g_index.giver.end()) return {};
    const auto& models = g_index.adjust[g->second];
    auto a = models.find(ModelName(key));
    return a == models.end() ? game::mods::Adjust{} : a->second;
}

}  // namespace

namespace game::mods {

std::vector<Mod> List(bool rescan) {
    std::lock_guard lock(g_lock);
    if (!g_index.made || rescan) Make(g_index);
    return g_index.mods;
}

void SetOn(const std::string& folder, bool on) {
    std::set<std::string> off = Off();
    if (on) off.erase(folder);
    else off.insert(folder);
    std::string s;
    for (const std::string& f : off) s += (s.empty() ? "" : ",") + f;
    std::lock_guard lock(g_lock);
    Opt().mods_off = s;
    Make(g_index);
}

const std::vector<uint8_t>* File(const std::string& path) {
    std::lock_guard lock(g_lock);
    if (!g_index.made) Make(g_index);
    std::string key = Key(path);
    auto it = g_index.files.find(key);
    if (it == g_index.files.end()) return nullptr;
    auto read = g_read.find(it->second);
    if (read == g_read.end()) {
        std::ifstream in(it->second, std::ios::binary);
        if (!in) return nullptr;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        read = g_read.emplace(it->second, std::move(data)).first;
    }
    if (g_logged.insert(key).second)
        LOGR("mods: %s from %s (%zu bytes)", key.c_str(), it->second.string().c_str(), read->second.size());
    if (!IsAdjustable(key)) return &read->second;
    Served& s = g_served[key];
    Adjust a = AdjustOf(key);
    if (s.data.empty() || !(s.adjust == a)) {
        s.data = a == Adjust{} ? read->second : AdjustModel(read->second, a, nullptr);
        s.adjust = a;
    }
    return &s.data;
}

std::vector<std::string> Paths(const std::string& prefix) {
    std::lock_guard lock(g_lock);
    if (!g_index.made) Make(g_index);
    std::vector<std::string> out;
    for (auto it = g_index.files.lower_bound(prefix); it != g_index.files.end() && !it->first.compare(0, prefix.size(), prefix);
         ++it)
        out.push_back(it->first);
    return out;
}

void Read(const std::string& path, uint32_t buffer) {
    std::lock_guard lock(g_lock);
    auto s = g_served.find(Key(path));
    if (s == g_served.end()) return;
    auto& b = s->second.buffers;
    b.erase(std::remove(b.begin(), b.end(), buffer), b.end());
    b.push_back(buffer);
    if (b.size() > 8) b.erase(b.begin());
    s->second.order = ++g_reads;
}

// every first-person model of the mods switched on: the read ones, the last read first, then the rest by name
std::vector<Adjustable> Adjustables() {
    std::lock_guard lock(g_lock);
    if (!g_index.made) Make(g_index);
    std::vector<std::pair<uint64_t, Adjustable>> list;
    for (const auto& [key, file] : g_index.files) {
        if (!IsAdjustable(key)) continue;
        auto s = g_served.find(key);
        bool read = s != g_served.end() && s->second.order;
        list.push_back({read ? s->second.order : 0, {key, ModelName(key), AdjustOf(key), read && !s->second.buffers.empty()}});
    }
    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second.name < b.second.name;
    });
    std::vector<Adjustable> out;
    for (auto& [order, a] : list) out.push_back(std::move(a));
    return out;
}

// written into each copy the game read that still holds what was served; a copy whose memory went to something else
// is left alone and forgotten
void SetAdjust(const std::string& key, const Adjust& a, bool save) {
    std::lock_guard lock(g_lock);
    auto g = g_index.giver.find(key);
    if (g == g_index.giver.end()) return;
    g_index.adjust[g->second][ModelName(key)] = a;
    if (save) WriteAdjust(app::ExeDir() / "mods" / g->second / "adjust.toml", g_index.adjust[g->second]);
    // a model not read yet is served adjusted when it is
    auto f = g_index.files.find(key);
    auto s = g_served.find(key);
    auto r = f == g_index.files.end() ? g_read.end() : g_read.find(f->second);
    if (s == g_served.end() || r == g_read.end()) return;
    Served& sv = s->second;
    if (sv.adjust == a) return;
    std::vector<std::pair<uint32_t, uint32_t>> changed;
    std::vector<uint8_t> next = AdjustModel(r->second, a, &changed);
    auto holds = [&](uint32_t buffer, const std::vector<uint8_t>& d) {
        for (auto [at, n] : changed)
            if (std::memcmp(kernel::HostPtr(buffer + at), d.data() + at, n)) return false;
        return !changed.empty();
    };
    std::vector<uint32_t> kept;
    for (uint32_t buffer : sv.buffers) {
        if (!holds(buffer, sv.data)) continue;
        for (auto [at, n] : changed) std::memcpy(kernel::HostPtr(buffer + at), next.data() + at, n);
        kept.push_back(buffer);
    }
    sv.buffers = std::move(kept);
    sv.data = std::move(next);
    sv.adjust = a;
}

}  // namespace game::mods
