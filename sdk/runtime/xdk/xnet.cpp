// System link on host Winsock. The game reaches the network only through the XNet winsock thunks, replaced
// here. The title's XNet object stays null, so the XNet calls not replaced (Live QoS and the like) fail with
// WSANOTINITIALISED on their own, and the XOnline calls with XONLINE_E_NOT_INITIALIZED.
// Addresses: an XNADDR's ina is the peer's real IPv4, so secure addresses map to IPs one to one.
// With --instance N the game binds 127.0.0.N and a broadcast goes to the other loopback instances.
// Join IP (a host past the LAN): the system link search's broadcasts also go to that address, and the host's
// broadcasts (a LAN discovery may answer every search with one, to 255.255.255.255) go to the public addresses
// that have reached it. With one peer (co-op) a LAN address in an XNADDR, which only means something on its
// owner's LAN, stands for the peer's public one: the join address on the client, the address its packets come
// from on the host. Ports a NAT rewrote are answered on the port the peer's packets came from.
// A broadcast also goes to the broadcast address of every other network adapter up (a VPN's, such as Radmin's
// 26.255.255.255, Hamachi's or ZeroTier's): Windows sends 255.255.255.255 out of one adapter only, so players on
// such a VPN find each other's games without a join address. With such a VPN up, a game's own address (its XNADDR)
// is its address there, so every player of a System Link game of up to 16 reaches every other through it.
// Hosted link (link_mode, --link-host / --link-join): every game's UDP goes through one player's game, which relays
// it, so only the host's port must be reachable (UPnP asks the router for it) and the joiners need nothing open.
// Each game is 10.254.x.y to the title (the host 10.254.0.1), its XNADDR and every source address alike, so the
// addresses the title puts in its session data mean the same on every PC. A title socket is a host socket on a port
// of its own that sends each datagram to the host's relay with a header (the virtual source and destination, a port
// each), and says hello every 2 s so the relay knows where its port is; the relay sends a datagram to the socket
// that has its destination, or a broadcast to every socket of other games on that port.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <map>
#include <random>

#include "app/app.h"
#include "net/upnp.h"
#include "xdk/hle.h"
#include "xdk/xnet.h"

using kernel::HostPtr;
using kernel::R32;
using kernel::W32;

namespace {

// XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]
constexpr uint32_t kXnAddrSize = 0x24;
constexpr uint32_t kXnAddrEnet = 0x0A;
// WSAOVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent
constexpr uint32_t kOvlStatus = 0x00;
constexpr uint32_t kOvlBytes = 0x04;
constexpr uint32_t kOvlEvent = 0x10;
constexpr uint32_t kOvlPending = 0x103;
// XNDNS: iStatus, cina, aina[8]
constexpr uint32_t kXnDnsSize = 0x28;
// fd_set: fd_count, fd_array[64]
constexpr uint32_t kFdSetMax = 64;

constexpr uint32_t kSocketError = 0xFFFFFFFF;
constexpr uint32_t kLinkUp = 0x01 | 0x02 | 0x08;          // active, 100 Mbps, full duplex
constexpr uint32_t kXnAddrEthernet = 0x02 | 0x04;         // ethernet, static address
constexpr DWORD kUdpConnReset = 0x9800000C;  // SIO_UDP_CONNRESET: off, or a send to a closed port fails the next recv
constexpr int kLoopbackInstances = 8;

struct Socket {
    SOCKET s = INVALID_SOCKET;
    bool udp = false;
    bool nonblocking = false;  // the guest's FIONBIO; the host socket is always nonblocking
    uint16_t port = 0;         // bound, network order; hosted link: the title's port, not the host socket's
    sockaddr_in peer{};        // hosted link: a connected UDP socket's destination
};

struct Span {
    uint32_t ptr, len;
};

// the byte count and flags pointers are only written when the call completes at once, as in Winsock:
// a title may pass its flags on the stack
struct PendingRecv {
    uint32_t sock, ovl, from, fromlen;
    std::vector<Span> spans;  // copied: the title's WSABUF array may live on its stack
};

std::mutex g_lock;
std::unordered_map<uint32_t, Socket> g_sockets;
uint32_t g_next_socket = 0x100;
std::vector<PendingRecv> g_pending;

std::once_flag g_init;
int g_instance = 0;
uint32_t g_local = 0;  // network order
uint32_t g_route = 0;  // the address 255.255.255.255 leaves by
std::array<uint8_t, 8> g_kid{};
std::unordered_map<uint32_t, std::array<uint8_t, kXnAddrSize>> g_peers;

thread_local uint32_t t_error = 0;

std::atomic<uint32_t> g_remote{0};  // the last public address a packet came from
std::mutex g_nat_lock;
std::unordered_map<uint64_t, uint16_t> g_nat;  // (peer address, our port) -> the port its packets come from
std::vector<uint32_t> g_public;                // public addresses packets came from, the latest last
constexpr size_t kMaxPublic = 16;

std::string Ip(uint32_t a) {
    char s[32];
    std::snprintf(s, sizeof(s), "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    return s;
}
std::string Addr(const sockaddr_in& a) { return Ip(a.sin_addr.s_addr) + ":" + std::to_string(ntohs(a.sin_port)); }

bool Private(uint32_t a) {
    uint8_t b0 = a & 0xFF, b1 = (a >> 8) & 0xFF;
    return b0 == 10 || b0 == 127 || (b0 == 172 && (b1 & 0xF0) == 16) || (b0 == 192 && b1 == 168) ||
           (b0 == 169 && b1 == 254);
}

uint64_t NatKey(uint32_t address, uint16_t port) { return uint64_t(address) << 16 | port; }

// the F10 join address, resolved when it changes; 0 for none
uint32_t JoinAddress() {
    static std::mutex m;
    static std::string last;
    static uint32_t address = 0;
    std::string text;
    {
        std::lock_guard<std::recursive_mutex> ui(app::UiLock());
        text = app::Get().join_ip;
    }
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    std::lock_guard<std::mutex> lock(m);
    if (text == last) return address;
    last = text;
    address = 0;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (!text.empty() && getaddrinfo(text.c_str(), nullptr, &hints, &res) == 0) {
        address = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
    }
    if (!text.empty()) LOGI("xnet: join %s -> %s", text.c_str(), address ? Ip(address).c_str() : "not found");
    return address;
}

uint32_t PrimaryAddress() {
    uint32_t addr = htonl(INADDR_LOOPBACK);
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);  // no packet is sent: connect only picks the route
    sockaddr_in me{};
    int len = sizeof(me);
    if (s != INVALID_SOCKET && ::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && me.sin_addr.s_addr)
        addr = me.sin_addr.s_addr;
    if (s != INVALID_SOCKET) ::closesocket(s);
    return addr;
}

// the broadcast addresses of the adapters up but the one 255.255.255.255 leaves by, looked at again every 10 s
std::vector<uint32_t> AdapterBroadcasts() {
    static std::mutex m;
    static std::vector<uint32_t> found;
    static uint64_t looked = 0;
    std::lock_guard<std::mutex> lock(m);
    uint64_t now = GetTickCount64();
    if (looked && now - looked < 10000) return found;
    looked = now;
    std::vector<uint32_t> out;
    ULONG size = 16 << 10;
    std::vector<uint8_t> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r == NO_ERROR)
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                auto* in = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
                uint32_t ip = in->sin_addr.s_addr, bits = u->OnLinkPrefixLength;
                if (in->sin_family != AF_INET || ip == g_route || bits == 0 || bits >= 31) continue;
                if ((ip & 0xFFFF) == 0xFEA9) continue;  // 169.254: no address given
                uint32_t host_mask = bits >= 32 ? 0 : (0xFFFFFFFFu >> bits);
                uint32_t b = ip | htonl(host_mask);
                if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
            }
        }
    if (out != found)
        for (uint32_t b : out) LOGI("xnet: broadcasts also to %s", Ip(b).c_str());
    found = out;
    return found;
}

// a VPN adapter's address (Radmin VPN, ZeroTier, Hamachi), 0 for none up
uint32_t VpnAddress() {
    ULONG size = 32 << 10;
    std::vector<uint8_t> buf(size);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                             reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size) != NO_ERROR)
        return 0;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        std::wstring name = std::wstring(a->Description ? a->Description : L"") + L" " +
                            (a->FriendlyName ? a->FriendlyName : L"");
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
        if (name.find(L"radmin") == std::wstring::npos && name.find(L"zerotier") == std::wstring::npos &&
            name.find(L"hamachi") == std::wstring::npos)
            continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* in = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (in->sin_family == AF_INET && (in->sin_addr.s_addr & 0xFFFF) != 0xFEA9) return in->sin_addr.s_addr;
        }
    }
    return 0;
}

// hosted link
enum class Link { Off, Host, Join };
Link g_link = Link::Off;
sockaddr_in g_relay{};  // where a title socket's datagrams go: the host's relay (on the host, on 127.0.0.1)
std::string g_relay_text;
std::atomic<uint64_t> g_relay_heard{0};  // when the relay last answered a hello, GetTickCount64
std::atomic<int> g_relay_players{0};     // on the host: the games the relay knows, this one too
std::atomic<bool> g_relay_failed{false};
constexpr uint32_t kHostVip = 0x0100FE0A;     // 10.254.0.1
constexpr uint32_t kVipBroadcast = 0xFFFFFE0A;  // 10.254.255.255
constexpr uint32_t kTunData = 0x444C5354;     // "TSLD"
constexpr uint32_t kTunHello = 0x484C5354;    // "TSLH"
constexpr uint32_t kTunAck = 0x414C5354;      // "TSLA"
constexpr uint64_t kHelloMs = 2000, kRouteMs = 15000;

struct TunHead {
    uint32_t magic;
    uint32_t src, dst;      // virtual addresses
    uint16_t sport, dport;  // the title's ports, network order
};
static_assert(sizeof(TunHead) == 16);

bool Relayed(const Socket& k) { return g_link != Link::Off && k.udp; }

bool Wait(SOCKET s, bool write, int ms);

void Hello(SOCKET s, uint16_t port) {
    TunHead h{kTunHello, g_local, 0, port, 0};
    ::sendto(s, reinterpret_cast<const char*>(&h), sizeof h, 0, reinterpret_cast<const sockaddr*>(&g_relay),
             sizeof g_relay);
}

// the host's relay: where each (game, port) was last heard from, datagrams passed on to it
void Relay(SOCKET s) {
    struct Route {
        sockaddr_in at;
        uint64_t seen;
    };
    std::unordered_map<uint64_t, Route> routes;
    std::vector<uint8_t> buf(65536);
    uint64_t pruned = 0;
    for (;;) {
        uint64_t now = GetTickCount64();
        if (now - pruned > 1000) {
            pruned = now;
            std::vector<uint32_t> games;
            for (auto it = routes.begin(); it != routes.end();) {
                if (now - it->second.seen > kRouteMs) {
                    LOGI("xnet: link: %s port %u gone", Ip(uint32_t(it->first >> 16)).c_str(),
                         ntohs(uint16_t(it->first)));
                    it = routes.erase(it);
                    continue;
                }
                uint32_t vip = uint32_t(it->first >> 16);
                if (std::find(games.begin(), games.end(), vip) == games.end()) games.push_back(vip);
                ++it;
            }
            if (g_relay_players.exchange(int(games.size())) != int(games.size()))
                LOGR("xnet: link: %zu games connected", games.size());
        }
        if (!Wait(s, false, 200)) continue;
        sockaddr_in from{};
        int fl = sizeof from;
        int n = ::recvfrom(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from),
                           &fl);
        if (n < int(sizeof(TunHead))) continue;
        TunHead h;
        std::memcpy(&h, buf.data(), sizeof h);
        if (h.magic != kTunData && h.magic != kTunHello) continue;
        uint64_t key = NatKey(h.src, h.sport);
        auto it = routes.find(key);
        bool fresh = it == routes.end() || std::memcmp(&it->second.at, &from, sizeof from) != 0;
        routes[key] = {from, now};
        if (fresh) LOGI("xnet: link: %s port %u at %s", Ip(h.src).c_str(), ntohs(h.sport), Addr(from).c_str());
        if (h.magic == kTunHello) {
            TunHead ack{kTunAck, h.src, 0, h.sport, 0};
            ::sendto(s, reinterpret_cast<const char*>(&ack), sizeof ack, 0, reinterpret_cast<const sockaddr*>(&from),
                     sizeof from);
            continue;
        }
        auto pass = [&](const sockaddr_in& to) {
            ::sendto(s, reinterpret_cast<const char*>(buf.data()), n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
        };
        if (h.dst == INADDR_BROADCAST || h.dst == kVipBroadcast) {
            for (auto& [k, r] : routes)
                if (uint16_t(k) == h.dport && uint32_t(k >> 16) != h.src) pass(r.at);
        } else if (auto to = routes.find(NatKey(h.dst, h.dport)); to != routes.end()) {
            pass(to->second.at);
        }
    }
}

// --link-host [port] / --link-join <address>[:port] on the command line, else the settings
void LinkInit() {
    const app::Settings& set = app::Get();
    int mode = int(set.link_mode);
    std::string address = set.link_address;
    uint32_t port = set.link_port;
    std::wstring cmd = GetCommandLineW();
    auto arg = [&](const wchar_t* name) -> std::string {
        size_t at = cmd.find(name);
        if (at == std::wstring::npos) return "-";
        size_t a = cmd.find_first_not_of(L" \t", at + std::wcslen(name));
        if (a == std::wstring::npos || cmd[a] == L'-') return {};
        size_t b = cmd.find_first_of(L" \t", a);
        std::wstring w = cmd.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        return std::string(w.begin(), w.end());
    };
    if (std::string h = arg(L"--link-host"); h != "-") {
        mode = 1;
        if (!h.empty()) port = uint32_t(std::atoi(h.c_str()));
    } else if (std::string j = arg(L"--link-join"); j != "-" && !j.empty()) {
        mode = 2;
        address = j;
    }
    if (mode == 2) {
        address.erase(0, address.find_first_not_of(" \t"));
        address.erase(address.find_last_not_of(" \t") + 1);
        if (size_t colon = address.rfind(':'); colon != std::string::npos) {
            port = uint32_t(std::atoi(address.c_str() + colon + 1));
            address.resize(colon);
        }
    }
    if (!mode || !port || port > 65535 || (mode == 2 && address.empty())) return;
    g_relay.sin_family = AF_INET;
    g_relay.sin_port = htons(uint16_t(port));
    if (mode == 1) {
        g_link = Link::Host;
        g_local = kHostVip;
        g_relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        g_relay_text = "port " + std::to_string(port);
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_port = g_relay.sin_port;
        if (s == INVALID_SOCKET || ::bind(s, reinterpret_cast<sockaddr*>(&at), sizeof at) != 0) {
            LOGE("xnet: link: UDP port %u is taken (another copy of the game?), no relay", port);
            g_relay_failed = true;
        } else {
            BOOL off = FALSE;
            DWORD ret = 0;
            WSAIoctl(s, kUdpConnReset, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
            std::thread(Relay, s).detach();
            upnp::OpenUdp(uint16_t(port));
        }
    } else {
        g_link = Link::Join;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (getaddrinfo(address.c_str(), nullptr, &hints, &res) == 0) {
            g_relay.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        } else {
            LOGE("xnet: link: host %s not found", address.c_str());
            g_relay_failed = true;
        }
        g_relay_text = address + ":" + std::to_string(port);
        // 10.254.1.1 to 10.254.254.254: the host is 10.254.0.1
        std::mt19937 rng{std::random_device{}()};
        g_local = 0xFE0A | (1 + rng() % 254) << 16 | (1 + rng() % 254) << 24;
    }
    LOGR("xnet: link: %s %s, this game is %s", mode == 1 ? "hosting on" : "joining", g_relay_text.c_str(),
         Ip(g_local).c_str());
}

void Pump();

void Init() {
    std::call_once(g_init, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_instance = app::Instance();
        g_route = PrimaryAddress();
        uint32_t vpn = g_instance ? 0 : VpnAddress();
        g_local = g_instance ? htonl(0x7F000000u | uint32_t(g_instance)) : vpn ? vpn : g_route;
        LOGR("xnet: %s address %s", g_instance ? "loopback instance" : vpn ? "VPN" : "LAN", Ip(g_local).c_str());
        LinkInit();
        std::thread(Pump).detach();
    });
}

uint32_t Fail(int err) {
    t_error = uint32_t(err);
    return kSocketError;
}
int HostError() {
    int e = WSAGetLastError();
    return e == WSAECONNRESET ? WSAEWOULDBLOCK : e;  // a port unreachable reply from an earlier send
}

Socket Find(uint32_t sock) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sockets.find(sock);
    return it == g_sockets.end() ? Socket{} : it->second;
}

sockaddr_in GuestAddr(uint32_t va) {
    sockaddr_in a{};
    std::memcpy(&a, HostPtr(va), sizeof(a));
    return a;
}

void Signal(uint32_t event) {
    if (!event) return;
    if (auto o = kernel::FromHandle(event)) kernel::SetEvent(*o);
}

bool Wait(SOCKET s, bool write, int ms) {
    fd_set f;
    FD_ZERO(&f);
    FD_SET(s, &f);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return ::select(0, write ? nullptr : &f, write ? &f : nullptr, nullptr, &tv) > 0;
}

std::vector<Span> Spans(uint32_t bufs, uint32_t count) {
    std::vector<Span> v;
    for (uint32_t i = 0; i < count; i++) v.push_back({R32(bufs + 8 * i + 4), R32(bufs + 8 * i)});
    return v;
}

// side datagrams: game data beside the title's own (src/game/net_bots.cpp), picked out before the title reads
constexpr uint8_t kSideMagic[8] = {'T', 'S', 'F', 'S', 'I', 'D', 'E', '1'};
constexpr uint64_t kSidePeerMs = 10000;
struct SidePeer {
    uint32_t sock;
    sockaddr_in to;  // as the title addresses it
    uint64_t at;
};
std::mutex g_side_lock;
std::unordered_map<uint32_t, SidePeer> g_side_peers;  // by the peer's address
// those who take the side datagrams (game/net_bots.cpp, game/net_diag.cpp), set as the program starts
constexpr int kSideReceivers = 4;
void (*g_side_receivers[kSideReceivers])(const uint8_t*, size_t, uint32_t) = {};

// the datagrams and bytes each way, by the peer's address as the title sees it (0 a broadcast), since the last
// TrafficLine; the failed sends and receives and the datagrams cut to the title's buffer
struct Traffic {
    uint64_t out_n = 0, out_bytes = 0, in_n = 0, in_bytes = 0;
};
std::mutex g_traffic_lock;
std::map<uint32_t, Traffic> g_traffic;
std::atomic<uint32_t> g_send_errors{0}, g_recv_errors{0}, g_cut{0};
void Count(uint32_t peer, bool out, int bytes) {
    if (bytes < 0) return;
    std::lock_guard<std::mutex> lock(g_traffic_lock);
    Traffic& t = g_traffic[peer];
    (out ? t.out_n : t.in_n)++;
    (out ? t.out_bytes : t.in_bytes) += uint64_t(bytes);
}

// one datagram into guest buffers: bytes, or -1 with err; a side datagram is handed on and the next one read
int RecvOne(uint32_t sock, SOCKET s, const std::vector<Span>& spans, uint32_t from, uint32_t fromlen, int& err, bool& side);
int Recv(uint32_t sock, SOCKET s, const std::vector<Span>& spans, uint32_t from, uint32_t fromlen, int& err) {
    for (int i = 0; i < 64; i++) {
        bool side = false;
        int n = RecvOne(sock, s, spans, from, fromlen, err, side);
        if (!side) return n;
    }
    err = WSAEWOULDBLOCK;
    return -1;
}

int RecvOne(uint32_t sock, SOCKET s, const std::vector<Span>& spans, uint32_t from, uint32_t fromlen, int& err, bool& side) {
    thread_local std::vector<uint8_t> buf(65536);
    sockaddr_in src{};
    int sl = sizeof(src);
    int n = ::recvfrom(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&src), &sl);
    if (n < 0) {
        err = HostError();
        if (err != WSAEWOULDBLOCK) {
            g_recv_errors++;
            LOG_FIRST(20, "xnet: recv %X failed (%d)", sock, err);
        }
        return -1;
    }
    const uint8_t* data = buf.data();
    if (g_link != Link::Off) {
        // from the relay: a datagram with its header, or the answer to a hello
        TunHead h{};
        if (n >= int(sizeof h)) std::memcpy(&h, data, sizeof h);
        if (h.magic != kTunData) {
            if (h.magic == kTunAck && g_relay_heard.exchange(GetTickCount64()) == 0)
                LOGR("xnet: link: the host's relay answered");
            err = WSAEWOULDBLOCK;
            return -1;
        }
        data += sizeof h;
        n -= int(sizeof h);
        src = {};
        src.sin_family = AF_INET;
        src.sin_addr.s_addr = h.src;
        src.sin_port = h.sport;
    }
    Count(src.sin_addr.s_addr, false, n);
    if (n >= int(sizeof kSideMagic) && !std::memcmp(data, kSideMagic, sizeof kSideMagic)) {
        side = true;
        for (auto receiver : g_side_receivers)
            if (receiver) receiver(data + sizeof kSideMagic, size_t(n) - sizeof kSideMagic, src.sin_addr.s_addr);
        return -1;
    }
    int done = 0;
    for (const Span& sp : spans) {
        int c = std::min<int>(n - done, int(sp.len));
        if (c <= 0) break;
        std::memcpy(HostPtr(sp.ptr), data + done, size_t(c));
        done += c;
    }
    if (done < n) g_cut++;
    if (done < n) LOG_FIRST(10, "xnet: %d byte datagram cut to %d", n, done);
    uint32_t peer = src.sin_addr.s_addr;
    if (g_link == Link::Off && peer != g_local && !Private(peer)) {
        if (g_remote.exchange(peer) != peer) LOGI("xnet: peer %s", Addr(src).c_str());
        auto it = g_sockets.find(sock);
        std::lock_guard<std::mutex> lock(g_nat_lock);
        if (it != g_sockets.end() && it->second.port) g_nat[NatKey(peer, it->second.port)] = src.sin_port;
        std::erase(g_public, peer);
        g_public.push_back(peer);
        if (g_public.size() > kMaxPublic) g_public.erase(g_public.begin());
    }
    if (from) std::memcpy(HostPtr(from), &src, sizeof(src));
    if (fromlen) W32(fromlen, sizeof(src));
    return done;
}

int SendOnce(SOCKET s, const uint8_t* data, int len, const sockaddr_in* to) {
    if (!to) return ::send(s, reinterpret_cast<const char*>(data), len, 0);
    sockaddr_in d = *to;
    if (d.sin_addr.s_addr == INADDR_BROADCAST) {
        std::vector<sockaddr_in> also;
        if (!g_instance)
            for (uint32_t b : AdapterBroadcasts()) {
                also.push_back(d);
                also.back().sin_addr.s_addr = b;
            }
        if (uint32_t join = JoinAddress()) {
            also.push_back(d);
            also.back().sin_addr.s_addr = join;
        }
        {
            std::lock_guard<std::mutex> lock(g_nat_lock);
            for (uint32_t peer : g_public) {
                also.push_back(d);
                also.back().sin_addr.s_addr = peer;
                if (auto it = g_nat.find(NatKey(peer, d.sin_port)); it != g_nat.end()) also.back().sin_port = it->second;
            }
        }
        for (const sockaddr_in& a : also)
            ::sendto(s, reinterpret_cast<const char*>(data), len, 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    } else {
        std::lock_guard<std::mutex> lock(g_nat_lock);
        auto it = g_nat.find(NatKey(d.sin_addr.s_addr, d.sin_port));
        if (it != g_nat.end()) d.sin_port = it->second;
    }
    if (g_instance && d.sin_addr.s_addr == INADDR_BROADCAST) {
        for (int i = 1; i <= kLoopbackInstances; i++) {
            if (i == g_instance) continue;
            d.sin_addr.s_addr = htonl(0x7F000000u | uint32_t(i));
            ::sendto(s, reinterpret_cast<const char*>(data), len, 0, reinterpret_cast<sockaddr*>(&d), sizeof(d));
        }
        return len;
    }
    return ::sendto(s, reinterpret_cast<const char*>(data), len, 0, reinterpret_cast<const sockaddr*>(&d), sizeof(d));
}

// with g_lock held: a title port for a hosted link socket that has none, as the network stack picks one
uint16_t FreePort() {
    static std::mt19937 rng{std::random_device{}()};
    for (;;) {
        uint16_t p = htons(uint16_t(49152 + rng() % 16384));
        if (std::none_of(g_sockets.begin(), g_sockets.end(), [&](auto& e) { return e.second.port == p; })) return p;
    }
}

// a title datagram to the host's relay, its header first
int SendRelayed(uint32_t sock, SOCKET s, const uint8_t* data, int len, const sockaddr_in* to) {
    TunHead h{kTunData, g_local, 0, 0, 0};
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_sockets.find(sock);
        if (it == g_sockets.end()) {
            WSASetLastError(WSAENOTSOCK);
            return -1;
        }
        if (!it->second.port) it->second.port = FreePort();
        const sockaddr_in& d = to ? *to : it->second.peer;
        h.sport = it->second.port;
        h.dst = d.sin_addr.s_addr;
        h.dport = d.sin_port;
    }
    thread_local std::vector<uint8_t> out;
    out.resize(sizeof h + size_t(len));
    std::memcpy(out.data(), &h, sizeof h);
    std::memcpy(out.data() + sizeof h, data, size_t(len));
    int n = ::sendto(s, reinterpret_cast<const char*>(out.data()), int(out.size()), 0,
                     reinterpret_cast<const sockaddr*>(&g_relay), sizeof g_relay);
    return n < 0 ? n : len;
}

int Send(uint32_t sock, SOCKET s, const uint8_t* data, int len, const sockaddr_in* to) {
    Socket k = Find(sock);
    bool relayed = g_link != Link::Off && k.udp;
    // the peers the title talks to, each by the socket it last reached them from, for the side datagrams
    const sockaddr_in* d = to ? to : relayed ? &k.peer : nullptr;
    if (k.udp && d && d->sin_addr.s_addr && d->sin_addr.s_addr != INADDR_BROADCAST && d->sin_addr.s_addr != kVipBroadcast &&
        d->sin_addr.s_addr != g_local && (len < int(sizeof kSideMagic) || std::memcmp(data, kSideMagic, sizeof kSideMagic))) {
        std::lock_guard<std::mutex> lock(g_side_lock);
        g_side_peers[d->sin_addr.s_addr] = {sock, *d, GetTickCount64()};
    }
    int n = relayed ? SendRelayed(sock, s, data, len, to) : SendOnce(s, data, len, to);
    if (n < 0) {
        g_send_errors++;
        LOG_FIRST(20, "xnet: send %X failed (%d)", sock, WSAGetLastError());
    }
    Count(d ? d->sin_addr.s_addr : 0, true, n < 0 ? -1 : len);
    return n;
}

std::vector<uint8_t> Gather(uint32_t bufs, uint32_t count) {
    std::vector<uint8_t> v;
    for (const Span& sp : Spans(bufs, count)) {
        auto p = static_cast<const uint8_t*>(HostPtr(sp.ptr));
        v.insert(v.end(), p, p + sp.len);
    }
    return v;
}

void Complete(uint32_t ovl, uint32_t status, uint32_t bytes) {
    W32(ovl + kOvlBytes, bytes);
    W32(ovl + kOvlStatus, status);
    Signal(R32(ovl + kOvlEvent));
}

// with g_lock held: false while no datagram is waiting
bool TryComplete(PendingRecv& p) {
    auto it = g_sockets.find(p.sock);
    if (it == g_sockets.end()) {
        Complete(p.ovl, WSAENOTSOCK, 0);
        return true;
    }
    int err = 0;
    int n = Recv(p.sock, it->second.s, p.spans, p.from, p.fromlen, err);
    if (n < 0 && err == WSAEWOULDBLOCK) return false;
    Complete(p.ovl, n < 0 ? uint32_t(err) : 0, n < 0 ? 0 : uint32_t(n));
    return true;
}

void Pump() {
    uint64_t hello = 0;
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        {
            std::lock_guard<std::mutex> lock(g_lock);
            // hosted link: each title socket with a port tells the relay where it is, which keeps the router's way
            // back to it open too
            if (uint64_t now = GetTickCount64(); g_link != Link::Off && now - hello >= kHelloMs) {
                hello = now;
                for (auto& [id, k] : g_sockets)
                    if (Relayed(k) && k.port) Hello(k.s, k.port);
            }
            for (auto& p : g_pending) {
                auto it = g_sockets.find(p.sock);
                if (it != g_sockets.end() && set.fd_count < FD_SETSIZE) FD_SET(it->second.s, &set);
            }
        }
        if (!set.fd_count) {
            Sleep(1);
            continue;
        }
        timeval tv{0, 2000};
        if (::select(0, &set, nullptr, nullptr, &tv) <= 0) {
            Sleep(1);
            continue;
        }
        std::lock_guard<std::mutex> lock(g_lock);
        for (size_t i = 0; i < g_pending.size();) {
            auto it = g_sockets.find(g_pending[i].sock);
            bool ready = it == g_sockets.end() || FD_ISSET(it->second.s, &set);
            if (ready && TryComplete(g_pending[i]))
                g_pending.erase(g_pending.begin() + ptrdiff_t(i));
            else
                i++;
        }
    }
}

void OwnXnAddr(uint8_t* x) {
    std::memset(x, 0, kXnAddrSize);
    std::memcpy(x, &g_local, 4);
    // Microsoft's OUI, then the address's last three bytes
    const uint8_t mac[6] = {0x00, 0x50, 0xF2, uint8_t(g_local >> 8), uint8_t(g_local >> 16), uint8_t(g_local >> 24)};
    std::memcpy(x + kXnAddrEnet, mac, 6);
}

void Random(uint32_t va, uint32_t n) {
    static std::mutex m;
    static std::mt19937 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(m);
    auto p = static_cast<uint8_t*>(HostPtr(va));
    for (uint32_t i = 0; i < n; i++) p[i] = uint8_t(rng());
}

}  // namespace

namespace xnet {

void CloseSockets() {
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& [id, socket] : g_sockets) ::closesocket(socket.s);
    g_sockets.clear();
    g_pending.clear();
}

void SideSend(const void* data, size_t len) {
    std::vector<SidePeer> peers;
    {
        std::lock_guard<std::mutex> lock(g_side_lock);
        uint64_t now = GetTickCount64();
        std::erase_if(g_side_peers, [&](auto& e) { return now - e.second.at > kSidePeerMs; });
        for (auto& e : g_side_peers) peers.push_back(e.second);
    }
    if (peers.empty() || len > 4096) return;
    thread_local std::vector<uint8_t> out;
    out.assign(kSideMagic, kSideMagic + sizeof kSideMagic);
    out.insert(out.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
    for (const SidePeer& p : peers) {
        Socket k = Find(p.sock);
        if (k.s != INVALID_SOCKET) Send(p.sock, k.s, out.data(), int(out.size()), &p.to);
    }
}

void SideReceiver(void (*fn)(const uint8_t* data, size_t len, uint32_t from)) {
    for (auto& receiver : g_side_receivers)
        if (!receiver) {
            receiver = fn;
            return;
        }
}

std::string TrafficLine() {
    std::map<uint32_t, Traffic> t;
    {
        std::lock_guard<std::mutex> lock(g_traffic_lock);
        t.swap(g_traffic);
    }
    std::string line;
    char part[160];
    for (auto& [peer, c] : t) {
        std::snprintf(part, sizeof part, "%s%s out %llu/%lluB in %llu/%lluB", line.empty() ? "" : "; ",
                      peer ? Ip(peer).c_str() : "broadcast", static_cast<unsigned long long>(c.out_n),
                      static_cast<unsigned long long>(c.out_bytes), static_cast<unsigned long long>(c.in_n),
                      static_cast<unsigned long long>(c.in_bytes));
        line += part;
    }
    uint32_t se = g_send_errors.exchange(0), re = g_recv_errors.exchange(0), cut = g_cut.exchange(0);
    if (se || re || cut) {
        std::snprintf(part, sizeof part, "%sfailed sends %u, receives %u, cut %u", line.empty() ? "" : "; ", se, re, cut);
        line += part;
    }
    return line.empty() ? "nothing" : line;
}

std::string LinkStatus() {
    Init();
    if (g_link == Link::Off) return {};
    uint64_t heard = g_relay_heard;
    bool live = heard && GetTickCount64() - heard < 3 * kHelloMs;
    if (g_link == Link::Join) {
        if (g_relay_failed) return "Joining " + g_relay_text + ": that address was not found.";
        return "Joining " + g_relay_text + (live ? ": connected to the host." : ": no answer from the host yet.");
    }
    if (g_relay_failed) return "Hosting: " + g_relay_text + " is taken (another copy of the game?).";
    int others = std::max(0, g_relay_players.load() - 1);
    std::string router = upnp::Status();
    return "Hosting on UDP " + g_relay_text + ", " + std::to_string(others) + " other players connected. Router: " +
           (router.empty() ? "asking for the port by UPnP..." : router + ".");
}

}  // namespace xnet

HLE_STDCALL(XNetStartup, 1) {
    Init();
    return 0;
}
HLE_STDCALL(WSAStartup, 2) {
    Init();
    if (a[1]) {
        std::memset(HostPtr(a[1]), 0, 400);
        W32(a[1], 0x02020202);  // wVersion, wHighVersion
    }
    return 0;
}
HLE_STDCALL(XNetGetEthernetLinkStatus, 0) { return kLinkUp; }
HLE_STDCALL(XOnlineStartup, 1) { return 0; }
HLE_STDCALL(XOnlineCleanup, 0) { return 0; }
HLE_STDCALL(WSAGetLastError, 0) { return t_error; }

HLE_STDCALL(XNetRandom, 2) {
    Random(a[0], a[1]);
    return 0;
}
HLE_STDCALL(XNetCreateKey, 2) {
    Random(a[0], 8);
    Random(a[1], 16);
    kernel::G<uint8_t>(a[0]) &= 0x0F;
    return 0;
}
HLE_STDCALL(XNetRegisterKey, 2) {
    std::lock_guard<std::mutex> lock(g_lock);
    std::memcpy(g_kid.data(), HostPtr(a[0]), 8);
    return 0;
}
HLE_STDCALL(XNetUnregisterKey, 1) { return 0; }

HLE_STDCALL(XNetGetTitleXnAddr, 1) {
    Init();
    OwnXnAddr(static_cast<uint8_t*>(HostPtr(a[0])));
    return kXnAddrEthernet;
}
HLE_STDCALL(XNetXnAddrToInAddr, 3) {
    uint32_t ina = R32(a[0]);
    if (!ina) return WSAEINVAL;
    if (g_link == Link::Off && ina != g_local && Private(ina) && !g_instance) {
        uint32_t join = JoinAddress(), remote = g_remote;
        if (join && !Private(join)) ina = join;
        else if (remote) ina = remote;
    }
    {
        std::lock_guard<std::mutex> lock(g_lock);
        std::memcpy(g_peers[ina].data(), HostPtr(a[0]), kXnAddrSize);
    }
    W32(a[2], ina);
    return 0;
}
HLE_STDCALL(XNetInAddrToXnAddr, 3) {
    uint32_t ina = a[0];
    uint8_t x[kXnAddrSize];
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_peers.find(ina);
    if (!ina || ina == g_local) {
        OwnXnAddr(x);
    } else if (it != g_peers.end()) {
        std::memcpy(x, it->second.data(), kXnAddrSize);
    } else {
        std::memset(x, 0, kXnAddrSize);
        std::memcpy(x, &ina, 4);
    }
    if (a[1]) std::memcpy(HostPtr(a[1]), x, kXnAddrSize);
    if (a[2]) std::memcpy(HostPtr(a[2]), g_kid.data(), 8);
    return 0;
}

HLE_STDCALL(XNetDnsLookup, 3) {
    std::string name = kernel::ReadCString(a[0], 256);
    uint32_t dns = mem::AllocSystem(kXnDnsSize);
    std::memset(HostPtr(dns), 0, kXnDnsSize);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    uint32_t count = 0;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &res) == 0) {
        for (addrinfo* r = res; r && count < 8; r = r->ai_next)
            W32(dns + 8 + 4 * count++, reinterpret_cast<sockaddr_in*>(r->ai_addr)->sin_addr.s_addr);
        freeaddrinfo(res);
    }
    W32(dns, count ? 0 : WSAHOST_NOT_FOUND);
    W32(dns + 4, count);
    W32(a[2], dns);
    LOGI("xnet: dns %s -> %u addresses", name.c_str(), count);
    Signal(a[1]);
    return 0;
}
HLE_STDCALL(XNetDnsRelease, 1) {
    if (a[0]) mem::FreeSystem(a[0]);
    return 0;
}

static uint32_t AddSocket(SOCKET s, bool udp) {
    u_long on = 1;
    ::ioctlsocket(s, FIONBIO, &on);
    if (udp) {
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, kUdpConnReset, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    }
    std::lock_guard<std::mutex> lock(g_lock);
    uint32_t id = g_next_socket;
    g_next_socket += 4;
    g_sockets[id] = {s, udp, false, 0};
    return id;
}

HLE_STDCALL(socket, 3) {
    Init();
    uint32_t af = a[0], type = a[1];  // the protocol: UDP, or VDP, which is UDP between two copies of this
    if (af != AF_INET) return Fail(WSAEAFNOSUPPORT);
    bool udp = type == SOCK_DGRAM;
    if (!udp && type != SOCK_STREAM) return Fail(WSAESOCKTNOSUPPORT);
    SOCKET s = ::socket(AF_INET, int(type), udp ? IPPROTO_UDP : IPPROTO_TCP);
    if (s == INVALID_SOCKET) return Fail(HostError());
    return AddSocket(s, udp);
}
HLE_STDCALL(closesocket, 1) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sockets.find(a[0]);
    if (it == g_sockets.end()) return Fail(WSAENOTSOCK);
    for (size_t i = 0; i < g_pending.size();) {
        if (g_pending[i].sock == a[0])
            g_pending.erase(g_pending.begin() + ptrdiff_t(i));
        else
            i++;
    }
    ::closesocket(it->second.s);
    g_sockets.erase(it);
    return 0;
}
HLE_STDCALL(shutdown, 2) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    return ::shutdown(k.s, int(a[1])) == 0 ? 0 : Fail(HostError());
}
HLE_STDCALL(ioctlsocket, 3) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sockets.find(a[0]);
    if (it == g_sockets.end()) return Fail(WSAENOTSOCK);
    if (a[1] == uint32_t(FIONBIO)) {
        it->second.nonblocking = R32(a[2]) != 0;
        return 0;
    }
    u_long v = R32(a[2]);
    if (::ioctlsocket(it->second.s, long(a[1]), &v) != 0) return Fail(HostError());
    W32(a[2], uint32_t(v));
    return 0;
}
HLE_STDCALL(setsockopt, 5) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    if (::setsockopt(k.s, int(a[1]), int(a[2]), static_cast<const char*>(HostPtr(a[3])), int(a[4])) == 0) return 0;
    LOG_FIRST(10, "xnet: setsockopt %X level %X option %X ignored (%d)", a[0], a[1], a[2], WSAGetLastError());
    return 0;
}
HLE_STDCALL(getsockopt, 5) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    int len = int(R32(a[4]));
    if (::getsockopt(k.s, int(a[1]), int(a[2]), static_cast<char*>(HostPtr(a[3])), &len) != 0) return Fail(WSAGetLastError());
    W32(a[4], uint32_t(len));
    return 0;
}
HLE_STDCALL(getsockname, 3) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in me{};
    int len = sizeof(me);
    if (Relayed(k)) {
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = g_local;
        me.sin_port = k.port;
    } else if (::getsockname(k.s, reinterpret_cast<sockaddr*>(&me), &len) != 0) {
        return Fail(HostError());
    }
    std::memcpy(HostPtr(a[1]), &me, sizeof(me));
    W32(a[2], sizeof(me));
    return 0;
}
HLE_STDCALL(bind, 3) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in at = GuestAddr(a[1]);
    if (Relayed(k)) {
        // the title's port is only the relay's business: the host socket takes any
        uint16_t port = at.sin_port;
        at.sin_addr.s_addr = INADDR_ANY;
        at.sin_port = 0;
        if (::bind(k.s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) return Fail(HostError());
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_sockets.find(a[0]);
        if (it == g_sockets.end()) return Fail(WSAENOTSOCK);
        it->second.port = port ? port : FreePort();
        Hello(k.s, it->second.port);
        LOGI("xnet: bind %X to port %u (hosted link)", a[0], ntohs(it->second.port));
        return 0;
    }
    if (g_instance && at.sin_addr.s_addr == INADDR_ANY) at.sin_addr.s_addr = g_local;
    if (::bind(k.s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) {
        int e = HostError();
        LOGW("xnet: bind %X to %s failed (%d)", a[0], Addr(at).c_str(), e);
        return Fail(e);
    }
    LOGI("xnet: bind %X to %s", a[0], Addr(at).c_str());
    sockaddr_in me{};
    int len = sizeof(me);
    ::getsockname(k.s, reinterpret_cast<sockaddr*>(&me), &len);
    std::lock_guard<std::mutex> lock(g_lock);
    if (auto it = g_sockets.find(a[0]); it != g_sockets.end()) it->second.port = me.sin_port;
    return 0;
}
HLE_STDCALL(connect, 3) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in to = GuestAddr(a[1]);
    if (Relayed(k)) {
        std::lock_guard<std::mutex> lock(g_lock);
        if (auto it = g_sockets.find(a[0]); it != g_sockets.end()) it->second.peer = to;
        return 0;
    }
    if (::connect(k.s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) return 0;
    int e = HostError();
    if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
    if (!Wait(k.s, true, 10000)) return Fail(WSAETIMEDOUT);
    int err = 0, len = sizeof(err);
    ::getsockopt(k.s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    return err ? Fail(err) : 0;
}
HLE_STDCALL(listen, 2) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    return ::listen(k.s, int(a[1])) == 0 ? 0 : Fail(WSAGetLastError());
}
HLE_STDCALL(accept, 3) {
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        sockaddr_in from{};
        int len = sizeof(from);
        SOCKET s = ::accept(k.s, reinterpret_cast<sockaddr*>(&from), &len);
        if (s != INVALID_SOCKET) {
            if (a[1]) std::memcpy(HostPtr(a[1]), &from, sizeof(from));
            if (a[2]) W32(a[2], sizeof(from));
            LOGI("xnet: accept %X from %s", a[0], Addr(from).c_str());
            return AddSocket(s, false);
        }
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
        Wait(k.s, false, 50);
    }
}
HLE_STDCALL(select, 5) {
    uint32_t guest[3] = {a[1], a[2], a[3]};
    fd_set host[3];
    std::vector<std::pair<uint32_t, SOCKET>> used[3];
    int total = 0;
    for (int i = 0; i < 3; i++) {
        FD_ZERO(&host[i]);
        if (!guest[i]) continue;
        uint32_t n = std::min(R32(guest[i]), kFdSetMax);
        for (uint32_t j = 0; j < n; j++) {
            uint32_t id = R32(guest[i] + 4 + 4 * j);
            Socket k = Find(id);
            if (k.s == INVALID_SOCKET) continue;
            FD_SET(k.s, &host[i]);
            used[i].push_back({id, k.s});
            total++;
        }
    }
    timeval tv{};
    timeval* ptv = nullptr;
    if (a[4]) {
        tv.tv_sec = long(R32(a[4]));
        tv.tv_usec = long(R32(a[4] + 4));
        ptv = &tv;
    }
    int r = 0;
    if (!total) {
        if (ptv) Sleep(DWORD(tv.tv_sec * 1000 + tv.tv_usec / 1000));
    } else {
        r = ::select(0, guest[0] ? &host[0] : nullptr, guest[1] ? &host[1] : nullptr, guest[2] ? &host[2] : nullptr, ptv);
        if (r < 0) return Fail(HostError());
    }
    for (int i = 0; i < 3; i++) {
        if (!guest[i]) continue;
        uint32_t k = 0;
        for (auto& [id, s] : used[i])
            if (FD_ISSET(s, &host[i])) W32(guest[i] + 4 + 4 * k++, id);
        W32(guest[i], k);
    }
    return uint32_t(r);
}

HLE_STDCALL(recvfrom, 6) {
    std::vector<Span> spans{{a[1], a[2]}};
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        int err = 0;
        int n;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            n = Recv(a[0], k.s, spans, a[4], a[5], err);
        }
        if (n >= 0) return uint32_t(n);
        if (err != WSAEWOULDBLOCK || k.nonblocking) return Fail(err);
        Wait(k.s, false, 50);
    }
}
// stream sockets; a reset connection is an error here, not a stale port unreachable reply
HLE_STDCALL(recv, 4) {
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        int n = ::recv(k.s, static_cast<char*>(HostPtr(a[1])), int(a[2]), int(a[3]));
        if (n >= 0) return uint32_t(n);
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
        Wait(k.s, false, 50);
    }
}
HLE_STDCALL(send, 4) {
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        int n = ::send(k.s, static_cast<const char*>(HostPtr(a[1])), int(a[2]), int(a[3]));
        if (n >= 0) return uint32_t(n);
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
        Wait(k.s, true, 50);
    }
}
HLE_STDCALL(sendto, 6) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in to{};
    if (a[4]) to = GuestAddr(a[4]);
    int n = Send(a[0], k.s, static_cast<const uint8_t*>(HostPtr(a[1])), int(a[2]), a[4] ? &to : nullptr);
    return n < 0 ? Fail(HostError()) : uint32_t(n);
}

namespace {

uint32_t SendOverlapped(uint32_t sock, uint32_t bufs, uint32_t count, uint32_t sent, uint32_t to_va, uint32_t ovl) {
    Socket k = Find(sock);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    auto data = Gather(bufs, count);
    sockaddr_in to{};
    if (to_va) to = GuestAddr(to_va);
    int n = Send(sock, k.s, data.data(), int(data.size()), to_va ? &to : nullptr);
    if (n < 0) return Fail(HostError());
    if (sent) W32(sent, uint32_t(n));
    if (ovl) Complete(ovl, 0, uint32_t(n));
    return 0;
}

uint32_t RecvOverlapped(uint32_t sock, uint32_t bufs, uint32_t count, uint32_t received, uint32_t flags, uint32_t from,
                        uint32_t fromlen, uint32_t ovl) {
    PendingRecv p{sock, ovl, from, fromlen, Spans(bufs, count)};
    for (;;) {
        Socket k = Find(sock);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        std::unique_lock<std::mutex> lock(g_lock);
        int err = 0;
        int n = Recv(sock, k.s, p.spans, from, fromlen, err);
        if (n >= 0) {
            if (received) W32(received, uint32_t(n));
            if (flags) W32(flags, 0);
            if (ovl) Complete(ovl, 0, uint32_t(n));
            return 0;
        }
        if (err != WSAEWOULDBLOCK) return Fail(err);
        if (ovl) {
            W32(ovl + kOvlStatus, kOvlPending);
            if (auto o = kernel::FromHandle(R32(ovl + kOvlEvent))) kernel::ResetEvent(*o);
            g_pending.push_back(std::move(p));
            return Fail(WSA_IO_PENDING);
        }
        if (k.nonblocking) return Fail(err);
        lock.unlock();
        Wait(k.s, false, 50);
    }
}

}  // namespace

HLE_STDCALL(WSARecvFrom, 9) { return RecvOverlapped(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); }
HLE_STDCALL(WSARecv, 7) { return RecvOverlapped(a[0], a[1], a[2], a[3], a[4], 0, 0, a[5]); }
HLE_STDCALL(WSASendTo, 9) { return SendOverlapped(a[0], a[1], a[2], a[3], a[5], a[7]); }
HLE_STDCALL(WSASend, 7) { return SendOverlapped(a[0], a[1], a[2], a[3], 0, a[5]); }

HLE_STDCALL(WSAGetOverlappedResult, 5) {
    uint32_t ovl = a[1];
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            auto it = std::find_if(g_pending.begin(), g_pending.end(), [&](const PendingRecv& p) { return p.ovl == ovl; });
            if (it == g_pending.end() || TryComplete(*it)) {
                if (it != g_pending.end()) g_pending.erase(it);
                uint32_t status = R32(ovl + kOvlStatus);
                if (status == kOvlPending) {
                    t_error = WSA_IO_INCOMPLETE;
                    return 0;
                }
                if (a[2]) W32(a[2], R32(ovl + kOvlBytes));
                if (a[4]) W32(a[4], 0);
                if (status) {
                    t_error = status;
                    return 0;
                }
                return 1;
            }
        }
        if (!a[3]) {
            t_error = WSA_IO_INCOMPLETE;
            return 0;
        }
        Sleep(1);
    }
}
HLE_STDCALL(WSACancelOverlappedIO, 1) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (size_t i = 0; i < g_pending.size();) {
        if (g_pending[i].sock == a[0]) {
            Complete(g_pending[i].ovl, WSA_OPERATION_ABORTED, 0);
            g_pending.erase(g_pending.begin() + ptrdiff_t(i));
        } else {
            i++;
        }
    }
    return 0;
}
