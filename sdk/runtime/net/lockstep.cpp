// Online co-op in lockstep (lockstep.h): the session's setup, the frames' exchange of controls, the clocks.
//
// UDP. A joiner sends HELLO (protocol, a hash of this executable and of the game's xbe, of the password) until the
// host answers WELCOME (the input delay, the host's pinned settings, the size and hash of its saves) or REJECT with a
// reason. The saves are one blob, asked for 1 KB at a time (CHUNK_REQ, a window of them, asked again when late) and
// checked against the hash. PINGs measure the round trip, READY asks to start and GO answers it; both games then
// boot. Each frame each sends INPUT: its controls for the last 16 frames up to the one it is filling in (a lost packet
// costs nothing while the next one comes), the newest frame of the other's it has, and every 60 frames a hash of the
// game's state. While a frame waits the last INPUT goes again every 50 ms, and a thread of its own sends it every
// 250 ms whatever the game does (a level loading), so a silence means the other is gone. A game closing sends BYE.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "net/lockstep.h"

namespace kernel {
void SetClockData(uint64_t t100);  // xboxkrnl.cpp: KeTickCount, the interrupt and system times in guest memory
}
namespace dsound {
void MixFrame(uint32_t samples);  // xdk/dsound.cpp: a frame of the game's sound, mixed for the output to play
}

namespace lockstep {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x504E5354;  // "TSNP"
constexpr uint32_t kProtocol = 2;  // 2: HELLO carries the content hash
constexpr uint32_t kChunk = 1024;
constexpr int kRedundant = 16;           // frames of controls in each INPUT
constexpr uint32_t kRing = 1024;         // frames of controls kept
constexpr uint32_t kHashEvery = 60;
constexpr uint32_t kSamplesPerFrame = 48000 / 60;
constexpr int kMaxDelay = 12;

enum Type : uint8_t {
    kHello = 1, kWelcome, kReject, kChunkReq, kChunkData, kPing, kPong, kReady, kGo, kInput, kBye,
};

#pragma pack(push, 1)
struct WirePad {
    uint16_t buttons;
    uint8_t analog[8];
    int16_t lx, ly, rx, ry;
    float ax, ay;  // the mouse's turn
    uint8_t flags;  // 1: the keyboard and mouse
};
struct Head {
    uint32_t magic;
    uint8_t type;
};
struct Hello {
    Head h;
    uint32_t protocol;
    uint64_t exe, xbe, pass;
    uint64_t content;  // Content()
};
struct Welcome {
    Head h;
    uint32_t delay;
    uint64_t saves_size, saves_hash;
    uint32_t pins;
    uint32_t pin[32][2];  // key hash, value
};
struct Reject {
    Head h;
    char reason[200];
};
struct ChunkReq {
    Head h;
    uint32_t count;
    uint32_t index[64];
};
struct ChunkData {
    Head h;
    uint32_t index;
    uint16_t size;
    uint8_t data[kChunk];
};
struct Ping {
    Head h;
    uint64_t sent;
};
struct Go {
    Head h;
    uint32_t delay;
};
struct Input {
    Head h;
    uint32_t first;  // the frame of pad[0]
    uint8_t count;
    uint32_t have;   // the newest frame of the receiver's controls the sender has all of, +1 (0: none)
    uint32_t hash_frame;
    uint64_t hash;   // the sender's state at the start of hash_frame (0 none)
    WirePad pad[kRedundant];
};
#pragma pack(pop)

// the session
enum class Role { None, Host, Join };
Role g_role = Role::None;
std::string g_addr, g_pass;
uint16_t g_port = kDefaultPort;
int g_delay_arg = -1;
std::atomic<bool> g_active{false};
SOCKET g_sock = INVALID_SOCKET;
sockaddr_in g_peer{};
std::atomic<bool> g_have_peer{false};
uint32_t g_delay = 2;
uint32_t g_rtt = 0;
std::vector<Pin> g_pins;
uint64_t g_content = 0;
std::string g_content_what;
Capture g_capture = nullptr;

// the frames
std::mutex g_lock;
std::condition_variable g_cv;
struct Slot {
    int64_t frame = -1;
    WirePad pad{};
};
std::array<Slot, kRing> g_local, g_remote;
int64_t g_remote_have = -1;   // the newest of the other's frames held, with every one before it
int64_t g_remote_newest = -1;  // the newest frame the other has sent
std::atomic<uint64_t> g_frame{0};
std::atomic<int64_t> g_last_heard{0};  // steady ms
std::atomic<bool> g_bye{false};
WirePad g_current[kPlayers]{};
bool g_aim_taken[kPlayers]{};
std::vector<uint8_t> g_last_input;  // the last INPUT sent, again while waiting
std::map<uint32_t, uint64_t> g_hash_local, g_hash_remote;
std::atomic<int64_t> g_desync_frame{-1};
int64_t g_dump_at = -1;
std::vector<std::pair<uint32_t, uint32_t>> g_hash_ranges, g_hash_skips;
std::atomic<bool> g_waiting{false};
std::atomic<int64_t> g_wait_since{0};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

uint64_t Hash64(const void* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, b + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
std::string AddrText(const sockaddr_in& a) {
    char s[64];
    inet_ntop(AF_INET, &a.sin_addr, s, sizeof s);
    return std::string(s) + ":" + std::to_string(ntohs(a.sin_port));
}

uint32_t KeyHash(const char* key) { return uint32_t(Hash64(key, std::strlen(key))); }

WirePad ToWire(const input::Pad& p, float ax, float ay, bool keyboard) {
    WirePad w{};
    w.buttons = p.buttons;
    std::memcpy(w.analog, p.analog, 8);
    w.lx = p.lx, w.ly = p.ly, w.rx = p.rx, w.ry = p.ry;
    w.ax = ax, w.ay = ay;
    w.flags = keyboard ? 1 : 0;
    return w;
}

// sending

void SendRaw(const void* data, int size) {
    if (g_sock == INVALID_SOCKET || !g_have_peer) return;
    sendto(g_sock, static_cast<const char*>(data), size, 0, reinterpret_cast<const sockaddr*>(&g_peer), sizeof g_peer);
}
template <typename T>
void Send(T& packet, Type type, int size = sizeof(T)) {
    packet.h.magic = kMagic;
    packet.h.type = type;
    SendRaw(&packet, size);
}

// the setup's state, kept by the receiving thread
struct Setup {
    // host
    std::vector<uint8_t> saves;  // the blob
    uint64_t saves_hash = 0;
    uint64_t exe = 0, xbe = 0;
    bool ready = false;  // the joiner asked to start
    // joiner
    bool welcomed = false, rejected = false, go = false;
    std::string reason;
    Welcome welcome{};
    std::vector<uint8_t> got;     // the blob as it comes
    std::vector<uint8_t> have;    // which chunks came
    uint32_t chunks_left = 0;
    // both
    std::vector<uint32_t> rtts;
};
Setup g_setup;

void SendInputLocked(uint32_t newest);

// the receiving thread: setup packets until the session starts, then the other's controls
void Receive() {
    std::vector<uint8_t> buf(2048);
    for (;;) {
        sockaddr_in from{};
        int len = sizeof from;
        int n = recvfrom(g_sock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;  // an ICMP unreachable from a peer not up yet
            if (e == WSAENOTSOCK || e == WSAEINTR) return;
            continue;
        }
        if (n < int(sizeof(Head))) continue;
        Head h;
        std::memcpy(&h, buf.data(), sizeof h);
        if (h.magic != kMagic) continue;
        // the host takes its peer from the first HELLO; after that only the peer's packets count
        bool from_peer = g_have_peer && from.sin_addr.s_addr == g_peer.sin_addr.s_addr && from.sin_port == g_peer.sin_port;
        std::unique_lock lock(g_lock);
        if (h.type == kHello && g_role == Role::Host && n >= int(sizeof(Hello))) {
            Hello m;
            std::memcpy(&m, buf.data(), sizeof m);
            if (g_have_peer && !from_peer) continue;  // one player 2
            auto refuse = [&](const char* why) {
                Reject r{};
                std::snprintf(r.reason, sizeof r.reason, "%s", why);
                r.h = {kMagic, kReject};
                sendto(g_sock, reinterpret_cast<const char*>(&r), sizeof r, 0, reinterpret_cast<const sockaddr*>(&from), sizeof from);
                LOGR("net: refused %s: %s", AddrText(from).c_str(), why);
            };
            if (m.protocol != kProtocol || m.exe != g_setup.exe) {
                refuse("The host plays another version of the game. Both players need the same TSFP.exe.");
                continue;
            }
            if (m.xbe != g_setup.xbe) {
                refuse("The host's game files are different (default.xbe). Both players need the same US game.");
                continue;
            }
            if (m.pass != Hash64(g_pass.data(), g_pass.size())) {
                refuse("Wrong password.");
                continue;
            }
            // other mods load other files from the first frame: the games would drift apart at once
            if (m.content != g_content) {
                refuse(("The players' mods differ: both need the same ones switched on (the settings panel's Mods "
                        "page). The host's: " + g_content_what + ".").c_str());
                continue;
            }
            if (!g_have_peer) {
                g_peer = from;
                g_have_peer = true;
                LOGR("net: player 2 is %s", AddrText(from).c_str());
            }
            Welcome w{};
            w.delay = g_delay;
            w.saves_size = g_setup.saves.size();
            w.saves_hash = g_setup.saves_hash;
            w.pins = uint32_t(std::min<size_t>(g_pins.size(), 32));
            for (uint32_t i = 0; i < w.pins; i++) w.pin[i][0] = KeyHash(g_pins[i].key), w.pin[i][1] = g_pins[i].Get();
            Send(w, kWelcome);
            continue;
        }
        if (!from_peer) continue;
        g_last_heard = NowMs();
        switch (h.type) {
            case kWelcome:
                if (g_role == Role::Join && !g_setup.welcomed && n >= int(sizeof(Welcome))) {
                    std::memcpy(&g_setup.welcome, buf.data(), sizeof(Welcome));
                    g_setup.welcomed = true;
                    uint32_t chunks = uint32_t((g_setup.welcome.saves_size + kChunk - 1) / kChunk);
                    g_setup.got.assign(size_t(g_setup.welcome.saves_size), 0);
                    g_setup.have.assign(chunks, 0);
                    g_setup.chunks_left = chunks;
                }
                break;
            case kReject:
                if (g_role == Role::Join && n >= int(sizeof(Reject))) {
                    Reject r;
                    std::memcpy(&r, buf.data(), sizeof r);
                    r.reason[sizeof r.reason - 1] = 0;
                    g_setup.rejected = true;
                    g_setup.reason = r.reason;
                }
                break;
            case kChunkReq:
                if (g_role == Role::Host && n >= int(sizeof(Head) + 4)) {
                    ChunkReq q{};
                    std::memcpy(&q, buf.data(), std::min<size_t>(n, sizeof q));
                    for (uint32_t i = 0; i < std::min<uint32_t>(q.count, 64); i++) {
                        uint64_t at = uint64_t(q.index[i]) * kChunk;
                        if (at >= g_setup.saves.size()) continue;
                        ChunkData d{};
                        d.index = q.index[i];
                        d.size = uint16_t(std::min<uint64_t>(kChunk, g_setup.saves.size() - at));
                        std::memcpy(d.data, g_setup.saves.data() + at, d.size);
                        Send(d, kChunkData, int(offsetof(ChunkData, data)) + d.size);
                    }
                }
                break;
            case kChunkData:
                if (g_role == Role::Join && g_setup.welcomed && n >= int(offsetof(ChunkData, data))) {
                    ChunkData d;
                    std::memcpy(&d, buf.data(), std::min<size_t>(n, sizeof d));
                    uint64_t at = uint64_t(d.index) * kChunk;
                    if (d.index >= g_setup.have.size() || g_setup.have[d.index] || at + d.size > g_setup.got.size()) break;
                    std::memcpy(g_setup.got.data() + at, d.data, d.size);
                    g_setup.have[d.index] = 1;
                    g_setup.chunks_left--;
                }
                break;
            case kPing:
                if (n >= int(sizeof(Ping))) {
                    Ping p;
                    std::memcpy(&p, buf.data(), sizeof p);
                    Send(p, kPong);
                }
                break;
            case kPong:
                if (n >= int(sizeof(Ping))) {
                    Ping p;
                    std::memcpy(&p, buf.data(), sizeof p);
                    g_setup.rtts.push_back(uint32_t(std::max<int64_t>(NowMs() - int64_t(p.sent), 0)));
                }
                break;
            case kReady:
                if (g_role == Role::Host) {
                    g_setup.ready = true;
                    Go go{};
                    go.delay = g_delay;
                    Send(go, kGo);
                }
                break;
            case kGo:
                if (g_role == Role::Join && n >= int(sizeof(Go))) {
                    Go go;
                    std::memcpy(&go, buf.data(), sizeof go);
                    g_delay = std::clamp<uint32_t>(go.delay, 1, kMaxDelay);
                    g_setup.go = true;
                }
                break;
            case kInput:
                if (n >= int(offsetof(Input, pad))) {
                    Input in{};
                    std::memcpy(&in, buf.data(), std::min<size_t>(n, sizeof in));
                    uint32_t count = std::min<uint32_t>(in.count, kRedundant);
                    if (n < int(offsetof(Input, pad) + count * sizeof(WirePad))) break;
                    for (uint32_t i = 0; i < count; i++) {
                        int64_t f = int64_t(in.first) + i;
                        Slot& s = g_remote[size_t(f) % kRing];
                        if (f > g_remote_have && s.frame != f) s = {f, in.pad[i]};
                    }
                    if (count) g_remote_newest = std::max<int64_t>(g_remote_newest, int64_t(in.first) + count - 1);
                    while (g_remote[size_t(g_remote_have + 1) % kRing].frame == g_remote_have + 1) g_remote_have++;
                    if (in.hash) g_hash_remote[in.hash_frame] = in.hash;
                    g_cv.notify_all();
                }
                break;
            case kBye:
                if (!g_bye) LOGR("net: the other player left");
                g_bye = true;
                g_cv.notify_all();
                break;
            default:
                break;
        }
    }
}

// the status window of the setup: a line of text and Cancel
struct StatusWindow {
    HWND wnd = nullptr, text = nullptr, button = nullptr;
    bool cancelled = false;
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<StatusWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self && (m == WM_CLOSE || (m == WM_COMMAND && LOWORD(w) == 1))) {
            self->cancelled = true;
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
    void Open(const wchar_t* title) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"TSFPNetStatus";
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        RegisterClassW(&wc);
        RECT r{0, 0, 460, 130};
        AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
        int w = r.right - r.left, h = r.bottom - r.top;
        wnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, title, WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                              (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                              nullptr, nullptr, wc.hInstance, nullptr);
        SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 16, 428, 64, wnd, nullptr, wc.hInstance, nullptr);
        button = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 360, 90, 84, 26, wnd,
                                 reinterpret_cast<HMENU>(1), wc.hInstance, nullptr);
        SendMessageW(text, WM_SETFONT, WPARAM(font), TRUE);
        SendMessageW(button, WM_SETFONT, WPARAM(font), TRUE);
    }
    void Say(const std::string& s) {
        static std::string last;
        if (s == last) return;
        last = s;
        SetWindowTextA(text, s.c_str());
    }
    // pumps messages for a while; false once cancelled
    bool Pump(int ms) {
        auto until = Clock::now() + std::chrono::milliseconds(ms);
        do {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (!IsDialogMessageW(wnd, &m)) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
            }
            if (cancelled) return false;
            Sleep(5);
        } while (Clock::now() < until);
        return !cancelled;
    }
    void Close() {
        if (wnd) DestroyWindow(wnd);
        wnd = nullptr;
    }
};

uint64_t FileHash(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> buf(1 << 20);
    uint64_t h = 0xCBF29CE484222325ull;
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        h = Hash64(buf.data(), size_t(f.gcount()), h);
    }
    return h;
}

fs::path ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// the saves a session plays on: every title's on the hard disk's E: (UDATA and TDATA), as one blob of
// {path length, path, size, data}
const char* const kSaveDirs[] = {"E/UDATA", "E/TDATA"};
std::vector<uint8_t> PackSaves(const fs::path& root) {
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
    std::error_code ec;
    for (const char* dir : kSaveDirs) {
        fs::path d = root / dir;
        if (!fs::exists(d, ec)) continue;
        std::vector<fs::path> files;
        for (auto& e : fs::recursive_directory_iterator(d, ec))
            if (e.is_regular_file(ec)) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (auto& f : files) {
            std::string rel = fs::relative(f, root, ec).generic_string();
            std::ifstream in(f, std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            put32(uint32_t(rel.size()));
            out.insert(out.end(), rel.begin(), rel.end());
            put32(uint32_t(data.size()));
            out.insert(out.end(), data.begin(), data.end());
        }
    }
    return out;
}

// the joiner's own save folder for the session, the host's saves in it; nothing may climb out of it
bool UnpackSaves(const std::vector<uint8_t>& blob, const fs::path& root) {
    std::error_code ec;
    for (const char* dir : kSaveDirs) fs::remove_all(root / dir, ec);
    size_t at = 0;
    auto get32 = [&](uint32_t& v) {
        if (at + 4 > blob.size()) return false;
        std::memcpy(&v, blob.data() + at, 4);
        at += 4;
        return true;
    };
    fs::path canon = fs::weakly_canonical(root, ec);
    while (at < blob.size()) {
        uint32_t n, size;
        if (!get32(n) || at + n > blob.size()) return false;
        std::string rel(reinterpret_cast<const char*>(blob.data() + at), n);
        at += n;
        if (!get32(size) || at + size > blob.size()) return false;
        fs::path p = fs::weakly_canonical(root / fs::path(rel), ec);
        auto [a, b] = std::mismatch(canon.begin(), canon.end(), p.begin(), p.end());
        if (a != canon.end() || rel.find("..") != std::string::npos) {
            LOGE("net: a save path out of the session folder refused: %s", rel.c_str());
            return false;
        }
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(blob.data() + at), size);
        at += size;
    }
    return true;
}

bool OpenSocket(uint16_t port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    int big = 1 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof big);
    // an ICMP unreachable (the other not listening yet) must not end the receiving
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(g_sock, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);  // SIO_UDP_CONNRESET
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    return bind(g_sock, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
}

bool Resolve(const std::string& host, uint16_t port, sockaddr_in& out) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) || !res) return false;
    out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

uint32_t DelayFor(uint32_t rtt) {
    if (g_delay_arg > 0) return uint32_t(std::min(g_delay_arg, kMaxDelay));
    // half the round trip and 12 ms for the work and the two clocks' drift, in frames: a frame on a LAN, 2 at up to 40
    // ms of round trip. Each frame of it is a frame later that a player sees their own mouse turn the view
    return std::clamp<uint32_t>(uint32_t(std::ceil((rtt / 2.0 + 12.0) / 16.667)), 1, kMaxDelay);
}

void Fail(StatusWindow& w, const std::string& why) {
    LOGE("net: %s", why.c_str());
    w.Close();
    MessageBoxA(static_cast<HWND>(app::Window()), why.c_str(), "TSFP co-op", MB_ICONERROR);
}

void ApplyPins(const Welcome& w) {
    for (uint32_t i = 0; i < std::min<uint32_t>(w.pins, 32); i++)
        for (const Pin& p : g_pins)
            if (KeyHash(p.key) == w.pin[i][0] && p.Get() != w.pin[i][1]) {
                LOGR("net: %s is the host's, %u (was %u)", p.key, w.pin[i][1], p.Get());
                p.Set(w.pin[i][1]);
            }
}

void Heartbeat() {
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::lock_guard lock(g_lock);
            if (!g_last_input.empty()) SendRaw(g_last_input.data(), int(g_last_input.size()));
        }
    }).detach();
}

// the game's state for comparison
uint64_t StateHash() {
    uint64_t h = 0xCBF29CE484222325ull;
    for (auto [va, size] : g_hash_ranges) {
        for (uint32_t p = va; p < va + size; p += 0x1000) {
            uint32_t end = std::min(p + 0x1000, va + size);
            if (!mem::IsCommitted(p)) continue;
            for (uint32_t a = p; a < end;) {
                uint32_t next = end;
                bool skip = false;
                for (auto [sva, ssize] : g_hash_skips) {
                    if (a >= sva && a < sva + ssize) {
                        next = std::min(end, sva + ssize);
                        skip = true;
                        break;
                    }
                    if (sva > a && sva < next) next = sva;
                }
                if (!skip) h = Hash64(kernel::g_base + a, next - a, h);
                a = next;
            }
        }
    }
    return h;
}

// TSFP_NET_DUMP set: the hashed ranges as they were at each of the last hashes, the one that differed written out
std::map<uint32_t, std::vector<uint8_t>> g_snapshots;
void Snapshot(uint32_t frame) {
    static const bool dump = std::getenv("TSFP_NET_DUMP") != nullptr;
    if (!dump) return;
    std::vector<uint8_t>& out = g_snapshots[frame];
    for (auto [va, size] : g_hash_ranges) {
        size_t at = out.size();
        out.resize(at + size);
        for (uint32_t o = 0; o < size; o += 0x1000)
            if (mem::IsCommitted(va + o))
                std::memcpy(out.data() + at + o, kernel::g_base + va + o, std::min<uint32_t>(0x1000, size - o));
    }
    while (g_snapshots.size() > 8) g_snapshots.erase(g_snapshots.begin());
}
void WriteSnapshot(uint32_t frame) {
    auto it = g_snapshots.find(frame);
    if (it == g_snapshots.end()) return;
    fs::path p = app::ExeDir() / "logs" /
                 ("desync-" + std::to_string(frame) + (g_role == Role::Host ? "-host" : "-join") + ".data");
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(it->second.data()), std::streamsize(it->second.size()));
}

// every 64 KB of guest memory hashed, for a look at where the games parted
void DumpChunks(uint64_t frame) {
    fs::path p = app::ExeDir() / "logs" /
                 ("desync-" + std::to_string(frame) + (g_role == Role::Host ? "-host" : "-join") + ".txt");
    std::ofstream out(p);
    // TSFP_NET_DUMP set: the memory itself as well, {va, 64 KB} records, for a word by word comparison
    std::ofstream raw;
    if (std::getenv("TSFP_NET_DUMP")) raw.open(fs::path(p).replace_extension(".bin"), std::ios::binary);
    for (uint64_t va = 0x10000; va < 0x100000000ull; va += 0x10000) {
        if (!mem::IsCommitted(uint32_t(va))) continue;
        if (raw) {
            uint32_t v = uint32_t(va);
            raw.write(reinterpret_cast<const char*>(&v), 4);
            for (uint32_t pg = 0; pg < 0x10000; pg += 0x1000) {
                static const std::vector<char> zero(0x1000);
                bool c = mem::IsCommitted(uint32_t(va + pg));
                raw.write(c ? reinterpret_cast<const char*>(kernel::g_base + va + pg) : zero.data(), 0x1000);
            }
        }
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint32_t pg = 0; pg < 0x10000; pg += 0x1000)
            if (mem::IsCommitted(uint32_t(va + pg))) h = Hash64(kernel::g_base + va + pg, 0x1000, h);
        char line[48];
        std::snprintf(line, sizeof line, "%08X %016llX\n", uint32_t(va), static_cast<unsigned long long>(h));
        out << line;
    }
    LOGR("net: guest memory hashes at frame %llu written to %s", static_cast<unsigned long long>(frame), p.string().c_str());
}

// an INPUT with this machine's controls up to `newest`, g_lock held
void SendInputLocked(uint32_t newest) {
    Input in{};
    uint32_t first = newest + 1 >= kRedundant ? newest + 1 - kRedundant : 0;
    in.first = first;
    in.count = uint8_t(newest - first + 1);
    for (uint32_t f = first; f <= newest; f++) in.pad[f - first] = g_local[f % kRing].pad;
    in.have = uint32_t(g_remote_have + 1);
    if (!g_hash_local.empty()) {
        auto last = std::prev(g_hash_local.end());
        in.hash_frame = last->first;
        in.hash = last->second;
    }
    in.h = {kMagic, kInput};
    int size = int(offsetof(Input, pad) + in.count * sizeof(WirePad));
    g_last_input.assign(reinterpret_cast<uint8_t*>(&in), reinterpret_cast<uint8_t*>(&in) + size);
    SendRaw(&in, size);
}

}  // namespace

// the command line

bool Requested() {
    static const bool parsed = [] {
        // the command line's words, a quoted one whole
        std::vector<std::string> argv;
        std::string word;
        bool quoted = false, any = false;
        for (const wchar_t* w = GetCommandLineW(); ; w++) {
            if (*w == L'"') {
                quoted = !quoted, any = true;
            } else if (!*w || (!quoted && (*w == L' ' || *w == L'	'))) {
                if (any) argv.push_back(word);
                word.clear(), any = false;
                if (!*w) break;
            } else {
                word += char(*w < 128 ? *w : '?'), any = true;
            }
        }
        int argc = int(argv.size());
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i], next = i + 1 < argc ? argv[i + 1] : "";
            bool has_next = i + 1 < argc && next.rfind("--", 0) != 0;
            if (a == "--host") {
                g_role = Role::Host;
                if (has_next) g_port = uint16_t(std::atoi(next.c_str())), i++;
            } else if (a == "--join" && has_next) {
                g_role = Role::Join;
                size_t colon = next.rfind(':');
                g_addr = next.substr(0, colon);
                if (colon != std::string::npos) g_port = uint16_t(std::atoi(next.c_str() + colon + 1));
                i++;
            } else if (a == "--net-pass" && has_next) {
                g_pass = next;
                i++;
            } else if (a == "--net-delay" && has_next) {
                g_delay_arg = std::atoi(next.c_str());
                i++;
            }
        }
        if (!g_port) g_port = kDefaultPort;
        return true;
    }();
    (void)parsed;
    return g_role != Role::None;
}

bool Begin(const std::vector<uint8_t>& xbe, fs::path& data_dir) {
    if (!Requested()) return true;
    StatusWindow w;
    w.Open(L"TimeSplitters FP - online co-op");
    g_setup.exe = FileHash(ExePath());
    g_setup.xbe = Hash64(xbe.data(), xbe.size());
    if (!OpenSocket(g_role == Role::Host ? g_port : 0)) {
        Fail(w, g_role == Role::Host ? "Could not open UDP port " + std::to_string(g_port) +
                                           " (another program, or another copy of the game, has it)."
                                     : std::string("Could not open a network socket."));
        return false;
    }
    std::thread(Receive).detach();
    if (g_role == Role::Host) {
        g_setup.saves = PackSaves(data_dir);
        g_setup.saves_hash = Hash64(g_setup.saves.data(), g_setup.saves.size());
        LOGR("net: hosting on UDP port %u, saves %zu bytes", g_port, g_setup.saves.size());
        // until player 2 has joined and asked to start; meanwhile ping for the delay
        int64_t last_ping = 0;
        for (;;) {
            bool peer, ready;
            size_t pongs;
            {
                std::lock_guard lock(g_lock);
                peer = g_have_peer;
                ready = g_setup.ready;
                pongs = g_setup.rtts.size();
            }
            if (ready) break;
            w.Say(peer ? "Player 2 joined, sending the saves..."
                       : "Waiting for player 2 on UDP port " + std::to_string(g_port) +
                             ".\nPlayer 2 joins with your IP address (on a VPN such as Radmin, Tailscale or ZeroTier, its "
                             "address there). The port must be open to them.");
            if (peer && pongs < 5 && NowMs() - last_ping > 200) {
                Ping p{};
                p.sent = uint64_t(NowMs());
                Send(p, kPing);
                last_ping = NowMs();
                std::lock_guard lock(g_lock);
                if (!g_setup.rtts.empty()) {
                    std::vector<uint32_t> r = g_setup.rtts;
                    std::sort(r.begin(), r.end());
                    g_rtt = r[r.size() / 2];
                    g_delay = DelayFor(g_rtt);
                }
            }
            if (!w.Pump(20)) {
                w.Close();
                return false;
            }
        }
    } else {
        if (!Resolve(g_addr, g_port, g_peer)) {
            Fail(w, "Could not find the host " + g_addr + ".");
            return false;
        }
        g_have_peer = true;
        auto start = Clock::now();
        int64_t last_hello = 0, last_req = 0, last_ping = 0;
        bool unpacked = false;
        for (;;) {
            Setup s;
            {
                std::lock_guard lock(g_lock);
                s.welcomed = g_setup.welcomed;
                s.rejected = g_setup.rejected;
                s.reason = g_setup.reason;
                s.chunks_left = g_setup.chunks_left;
                s.go = g_setup.go;
                s.rtts = g_setup.rtts;
            }
            if (s.rejected) {
                Fail(w, "The host refused: " + s.reason);
                return false;
            }
            if (s.go) break;
            int64_t now = NowMs();
            if (!s.welcomed) {
                if (Clock::now() - start > std::chrono::seconds(60)) {
                    Fail(w, "No answer from " + g_addr + ":" + std::to_string(g_port) +
                                ". Is the host waiting, and its port open (or both on the same VPN)?");
                    return false;
                }
                w.Say("Joining " + g_addr + ":" + std::to_string(g_port) + "...");
                if (now - last_hello > 250) {
                    Hello h{};
                    h.protocol = kProtocol;
                    h.exe = g_setup.exe;
                    h.xbe = g_setup.xbe;
                    h.pass = Hash64(g_pass.data(), g_pass.size());
                    h.content = g_content;
                    Send(h, kHello);
                    last_hello = now;
                }
            } else if (s.chunks_left) {
                uint64_t total = g_setup.welcome.saves_size;
                w.Say("Receiving the host's saves: " + std::to_string((total - uint64_t(s.chunks_left) * kChunk) / 1024) +
                      " of " + std::to_string(total / 1024) + " KB");
                if (now - last_req > 60) {
                    ChunkReq q{};
                    std::lock_guard lock(g_lock);
                    for (uint32_t i = 0; i < g_setup.have.size() && q.count < 64; i++)
                        if (!g_setup.have[i]) q.index[q.count++] = i;
                    Send(q, kChunkReq);
                    last_req = now;
                }
            } else {
                if (!unpacked) {
                    std::vector<uint8_t> blob;
                    {
                        std::lock_guard lock(g_lock);
                        blob = g_setup.got;
                    }
                    if (Hash64(blob.data(), blob.size()) != g_setup.welcome.saves_hash) {
                        Fail(w, "The host's saves came damaged, try again.");
                        return false;
                    }
                    fs::path online = data_dir;
                    online += "-online";
                    if (!UnpackSaves(blob, online)) {
                        Fail(w, "The host's saves could not be written to " + online.string() + ".");
                        return false;
                    }
                    LOGR("net: the host's saves (%zu bytes) in %s", blob.size(), online.string().c_str());
                    data_dir = online;
                    ApplyPins(g_setup.welcome);
                    g_delay = g_setup.welcome.delay;
                    unpacked = true;
                }
                w.Say("Starting...");
                if (now - last_ping > 100) {
                    Ping p{};
                    p.sent = uint64_t(now);
                    Send(p, kPing);
                    struct {
                        Head h;
                    } ready{};
                    Send(ready, kReady);
                    last_ping = now;
                }
            }
            if (!w.Pump(10)) {
                w.Close();
                return false;
            }
        }
        std::lock_guard lock(g_lock);
        if (!g_setup.rtts.empty()) {
            std::vector<uint32_t> r = g_setup.rtts;
            std::sort(r.begin(), r.end());
            g_rtt = r[r.size() / 2];
        }
    }
    w.Close();
    // the first `delay` frames have no controls sent for them: idle on both machines
    for (uint32_t i = 0; i < g_delay; i++) g_local[i] = g_remote[i] = {int64_t(i), WirePad{}};
    g_remote_have = int64_t(g_delay) - 1;
    // the C runtime's sines and powers take an FMA path on a CPU with FMA3, which may round otherwise: off on both
    _set_FMA3_enable(0);
    g_last_heard = NowMs();
    g_active = true;
    Heartbeat();
    LOGR("net: co-op session as player %d with %s, input delay %u frames, round trip %u ms", LocalPort() + 1,
         AddrText(g_peer).c_str(), g_delay, g_rtt);
    return true;
}

bool Active() { return g_active; }
int LocalPort() { return g_role == Role::Join ? 1 : 0; }
int RemotePort() { return g_role == Role::Join ? 0 : 1; }
bool PeerOnThisPC() { return g_active && (ntohl(g_peer.sin_addr.s_addr) >> 24) == 127; }

void Pins(const Pin* pins, int count) { g_pins.assign(pins, pins + count); }
void Content(uint64_t hash, const std::string& what) {
    g_content = hash;
    g_content_what = what;
}
void SetCapture(Capture capture) { g_capture = capture; }

uint64_t Frame() { return g_frame; }

void FrameStart() {
    if (!g_active) return;
    static Clock::time_point t0 = Clock::now();
    static uint64_t paced = 0;
    uint64_t f = g_frame;
    // 60 frames a second: the game's time is the frame count
    auto due = t0 + std::chrono::nanoseconds(int64_t((f - paced) * 1000000000ull / 60));
    if (Clock::now() > due + std::chrono::milliseconds(50)) {
        t0 = Clock::now();  // fell behind (a load): start counting again
        paced = f;
    } else {
        std::this_thread::sleep_until(due);
    }
    // this machine's controls for a frame `delay` ahead
    float ax = 0, ay = 0;
    bool keyboard = false;
    input::Pad pad = g_capture ? g_capture(ax, ay, keyboard) : input::Pad{};
    std::unique_lock lock(g_lock);
    uint64_t target = f + g_delay;
    g_local[target % kRing] = {int64_t(target), ToWire(pad, ax, ay, keyboard)};
    // the other is ahead of this machine: stay back a little so neither waits
    if (g_remote_newest - int64_t(g_delay) > int64_t(f) + 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    SendInputLocked(uint32_t(target));
    // the other's controls for this frame
    if (g_remote[f % kRing].frame != int64_t(f)) {
        g_waiting = true;
        g_wait_since = NowMs();
        while (g_remote[f % kRing].frame != int64_t(f) && !g_bye) {
            g_cv.wait_for(lock, std::chrono::milliseconds(50));
            if (!g_last_input.empty()) SendRaw(g_last_input.data(), int(g_last_input.size()));
        }
        g_waiting = false;
        if (g_bye && g_remote[f % kRing].frame != int64_t(f)) g_remote[f % kRing] = {int64_t(f), WirePad{}};
    }
    g_current[LocalPort()] = g_local[f % kRing].pad;
    g_current[RemotePort()] = g_remote[f % kRing].pad;
    g_aim_taken[0] = g_aim_taken[1] = false;
    // the state as this frame starts, every second, compared with the other's
    if (f % kHashEvery == 0 && !g_hash_ranges.empty()) {
        g_hash_local[uint32_t(f)] = StateHash();
        Snapshot(uint32_t(f));
        while (g_hash_local.size() > 64) g_hash_local.erase(g_hash_local.begin());
    }
    for (auto it = g_hash_remote.begin(); it != g_hash_remote.end();) {
        auto mine = g_hash_local.find(it->first);
        if (mine != g_hash_local.end()) {
            if (mine->second != it->second && g_desync_frame < 0) {
                g_desync_frame = it->first;
                g_dump_at = int64_t(it->first) + 240;  // both machines dump at the same frame
                WriteSnapshot(it->first);
                LOGE("net: the games drifted apart by frame %u (state %016llX here, %016llX there)", it->first,
                     static_cast<unsigned long long>(mine->second), static_cast<unsigned long long>(it->second));
            }
            it = g_hash_remote.erase(it);
        } else if (g_hash_local.empty() || it->first < g_hash_local.begin()->first) {
            it = g_hash_remote.erase(it);
        } else {
            ++it;
        }
    }
    lock.unlock();
    if (g_dump_at == int64_t(f)) DumpChunks(f);
    // the clocks are this frame's, the sound mixes its frame
    g_frame = f + 1;
    kernel::SetClockData(Now100ns());
    dsound::MixFrame(kSamplesPerFrame);
}

input::Pad PortPad(int port) {
    input::Pad p;
    if (port < 0 || port >= kPlayers) return p;
    std::lock_guard lock(g_lock);
    const WirePad& w = g_current[port];
    p.connected = true;
    p.buttons = w.buttons;
    std::memcpy(p.analog, w.analog, 8);
    p.lx = w.lx, p.ly = w.ly, p.rx = w.rx, p.ry = w.ry;
    return p;
}

bool PortAim(int port, float& dx, float& dy) {
    dx = dy = 0;
    if (!g_active || port < 0 || port >= kPlayers) return false;
    std::lock_guard lock(g_lock);
    if (g_aim_taken[port]) return true;
    g_aim_taken[port] = true;
    dx = g_current[port].ax;
    dy = g_current[port].ay;
    return true;
}

bool PortKeyboard(int port) {
    if (!g_active || port < 0 || port >= kPlayers) return false;
    std::lock_guard lock(g_lock);
    return g_current[port].flags & 1;
}

uint64_t Now100ns() {
    // the frame's start: frame / 60 s
    uint64_t base = g_frame.load() * 10000000ull / 60;
    thread_local uint64_t last = 0;
    last = std::max(base, last + 10);
    return last;
}

void HashRange(uint32_t va, uint32_t size) { g_hash_ranges.push_back({va, size}); }
void HashSkip(uint32_t va, uint32_t size) { g_hash_skips.push_back({va, size}); }

std::string Notice() {
    if (!g_active) return {};
    if (g_bye) return "The other player left the game.";
    if (g_waiting && NowMs() - g_wait_since > 1500) {
        int64_t silent = NowMs() - g_last_heard;
        return silent > 5000 ? "No word from the other player for " + std::to_string(silent / 1000) + " s..."
                             : "Waiting for the other player...";
    }
    if (int64_t d = g_desync_frame; d >= 0)
        return "The games drifted apart at frame " + std::to_string(d) + " (see the log).";
    return {};
}

std::string Status() {
    if (!g_active) return {};
    return std::string(g_role == Role::Host ? "Hosting, player 2 at " : "Joined ") + AddrText(g_peer) + ", input delay " +
           std::to_string(g_delay) + " frames, round trip " + std::to_string(g_rtt) + " ms";
}

bool Restart(const std::string& spec, const std::string& pass) {
    // this executable again with the session's arguments, and --instance when this one has it
    std::wstring cmd = L"\"" + ExePath().wstring() + L"\"";
    auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
    if (spec.rfind("host:", 0) == 0) cmd += L" --host " + widen(spec.substr(5));
    else if (spec.rfind("join:", 0) == 0) cmd += L" --join " + widen(spec.substr(5));
    if (!spec.empty() && !pass.empty()) cmd += L" --net-pass \"" + widen(pass) + L"\"";
    if (!spec.empty() && g_delay_arg > 0) cmd += L" --net-delay " + std::to_wstring(g_delay_arg);
    if (app::Instance()) cmd += L" --instance " + std::to_wstring(app::Instance());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    std::wstring dir = app::ExeDir().wstring();
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        LOGE("net: could not start the game again (%lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOGR("net: restarting %s", spec.empty() ? "out of the session" : spec.c_str());
    Bye();
    logx::Flush();
    TerminateProcess(GetCurrentProcess(), 0);
    return true;
}

void Bye() {
    if (!g_active) return;
    Head h{kMagic, kBye};
    for (int i = 0; i < 3; i++) SendRaw(&h, sizeof h);
}

}  // namespace lockstep
