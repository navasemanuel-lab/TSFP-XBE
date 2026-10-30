// UPnP port mapping on plain sockets (upnp.h): SSDP's M-SEARCH for an Internet Gateway Device, its description
// over HTTP for the WAN connection service's control URL, then SOAP: AddPortMapping, GetExternalIPAddress, and
// DeletePortMapping at exit.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "net/upnp.h"

namespace upnp {
namespace {

std::mutex g_lock;
std::string g_status;
std::atomic<uint32_t> g_public{0};

// the router's mapping, to take off at exit
struct Mapping {
    std::string host, path, service;
    uint16_t router_port = 0, port = 0;
} g_mapping;

struct Url {
    std::string host, path = "/";
    uint16_t port = 80;
};

bool ParseUrl(const std::string& text, Url& u) {
    size_t at = text.find("://");
    if (at == std::string::npos) return false;
    std::string rest = text.substr(at + 3);
    size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hostport.find(':');
    u.host = hostport.substr(0, colon);
    u.port = colon == std::string::npos ? 80 : uint16_t(std::atoi(hostport.c_str() + colon + 1));
    return !u.host.empty();
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// the text between <tag> and </tag> from `from` on, empty for none; `end` past it
std::string Tag(const std::string& xml, const std::string& tag, size_t from = 0, size_t* end = nullptr) {
    std::string open = "<" + tag + ">", close = "</" + tag + ">";
    size_t a = xml.find(open, from);
    if (a == std::string::npos) return {};
    a += open.size();
    size_t b = xml.find(close, a);
    if (b == std::string::npos) return {};
    if (end) *end = b + close.size();
    std::string v = xml.substr(a, b - a);
    v.erase(0, v.find_first_not_of(" \t\r\n"));
    v.erase(v.find_last_not_of(" \t\r\n") + 1);
    return v;
}

std::string Dechunk(const std::string& body) {
    std::string out;
    size_t at = 0;
    for (;;) {
        size_t eol = body.find("\r\n", at);
        if (eol == std::string::npos) break;
        size_t n = std::strtoul(body.substr(at, eol - at).c_str(), nullptr, 16);
        if (!n || eol + 2 + n > body.size()) break;
        out += body.substr(eol + 2, n);
        at = eol + 2 + n + 2;
    }
    return out;
}

// one HTTP request: the status code (0 for no answer), the body, and this PC's address on the way to the router
int Http(const Url& u, const std::string& method, const std::string& extra, const std::string& body, std::string& out,
         uint32_t* local = nullptr) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0) return 0;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD ms = 4000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    bool ok = s != INVALID_SOCKET && ::connect(s, res->ai_addr, int(res->ai_addrlen)) == 0;
    freeaddrinfo(res);
    if (!ok) {
        if (s != INVALID_SOCKET) closesocket(s);
        return 0;
    }
    if (local) {
        sockaddr_in me{};
        int len = sizeof me;
        getsockname(s, reinterpret_cast<sockaddr*>(&me), &len);
        *local = me.sin_addr.s_addr;
    }
    std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                      "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra +
                      "\r\n" + body;
    ::send(s, req.data(), int(req.size()), 0);
    std::string resp;
    char buf[4096];
    for (int n; (n = ::recv(s, buf, sizeof buf, 0)) > 0;) resp.append(buf, size_t(n));
    closesocket(s);
    size_t head = resp.find("\r\n\r\n");
    if (resp.rfind("HTTP/", 0) != 0 || head == std::string::npos) return 0;
    int code = std::atoi(resp.c_str() + resp.find(' ') + 1);
    out = resp.substr(head + 4);
    if (Lower(resp.substr(0, head)).find("transfer-encoding: chunked") != std::string::npos) out = Dechunk(out);
    return code;
}

int Soap(const Url& control, const std::string& service, const std::string& action, const std::string& args,
         std::string& out, uint32_t* local = nullptr) {
    std::string body =
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + service +
        "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
    std::string extra = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service + "#" + action + "\"\r\n";
    return Http(control, "POST", extra, body, out, local);
}

// the description URLs of the gateways that answer an SSDP search within ~2.5 s
std::vector<std::string> Search() {
    std::vector<std::string> found;
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return found;
    DWORD ttl = 2, ms = 500;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    const char* kinds[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                           "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                           "urn:schemas-upnp-org:service:WANIPConnection:1",
                           "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (int round = 0; round < 5 && found.empty(); round++) {
        for (const char* st : kinds) {
            std::string m = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                        "MX: 2\r\nST: ") + st + "\r\n\r\n";
            ::sendto(s, m.data(), int(m.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
        }
        char buf[2048];
        for (int n; (n = ::recv(s, buf, sizeof buf - 1, 0)) > 0;) {
            buf[n] = 0;
            std::string r(buf), low = Lower(r);
            size_t at = low.find("\r\nlocation:");
            if (at == std::string::npos) continue;
            at += 11;
            size_t eol = r.find("\r\n", at);
            std::string loc = r.substr(at, eol - at);
            loc.erase(0, loc.find_first_not_of(" \t"));
            loc.erase(loc.find_last_not_of(" \t") + 1);
            if (std::find(found.begin(), found.end(), loc) == found.end()) found.push_back(loc);
        }
    }
    closesocket(s);
    return found;
}

std::string Ip(uint32_t a) {
    char s[32];
    std::snprintf(s, sizeof(s), "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    return s;
}

// an address no one past the router reaches: private, or a carrier's shared one (CGNAT, 100.64.0.0/10)
bool Unreachable(uint32_t a) {
    uint8_t b0 = a & 0xFF, b1 = (a >> 8) & 0xFF;
    return b0 == 10 || (b0 == 172 && (b1 & 0xF0) == 16) || (b0 == 192 && b1 == 168) || (b0 == 100 && (b1 & 0xC0) == 64);
}

void Say(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_status = s;
    LOGR("upnp: %s", s.c_str());
}

void Close() {
    Mapping m;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        m = g_mapping;
    }
    if (!m.port) return;
    Url control{m.host, m.path, m.router_port};
    std::string out;
    Soap(control, m.service, "DeletePortMapping",
         "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(m.port) +
             "</NewExternalPort><NewProtocol>UDP</NewProtocol>",
         out);
}

void Map(uint16_t port) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    std::vector<std::string> gateways = Search();
    if (gateways.empty()) {
        Say("no router answered UPnP: open UDP port " + std::to_string(port) + " on it by hand, or use a VPN");
        return;
    }
    for (const std::string& loc : gateways) {
        Url desc;
        std::string xml;
        if (!ParseUrl(loc, desc) || Http(desc, "GET", "", "", xml) != 200) continue;
        // the first WAN connection service, IP before PPP
        std::string service, control_path;
        for (const char* want : {"WANIPConnection:2", "WANIPConnection:1", "WANPPPConnection:1"})
            for (size_t at = 0; service.empty();) {
                size_t end = 0;
                std::string block = Tag(xml, "service", at, &end);
                if (!end) break;
                at = end;
                std::string type = Tag(block, "serviceType");
                if (type.find(want) != std::string::npos) service = type, control_path = Tag(block, "controlURL");
            }
        if (service.empty()) continue;
        Url control = desc;
        std::string base = Tag(xml, "URLBase");
        if (control_path.rfind("http", 0) == 0) ParseUrl(control_path, control);
        else {
            if (!base.empty()) ParseUrl(base, control);
            control.path = (control_path.empty() || control_path[0] != '/' ? "/" : "") + control_path;
        }
        std::string out;
        uint32_t local = 0;
        Soap(control, service, "GetExternalIPAddress", "", out, &local);
        uint32_t pub = 0;
        std::string ext = Tag(out, "NewExternalIPAddress");
        if (!ext.empty()) inet_pton(AF_INET, ext.c_str(), &pub);
        std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
                           "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(port) +
                           "</NewInternalPort><NewInternalClient>" + Ip(local) +
                           "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>TimeSplitters FP"
                           "</NewPortMappingDescription><NewLeaseDuration>";
        // a lease of 0 is for good (taken off at exit); some routers want one that ends
        int code = Soap(control, service, "AddPortMapping", args + "0</NewLeaseDuration>", out);
        if (code != 200) code = Soap(control, service, "AddPortMapping", args + "86400</NewLeaseDuration>", out);
        if (code != 200) {
            std::string err = Tag(out, "errorDescription");
            Say("the router refused to open UDP port " + std::to_string(port) + " (" +
                (err.empty() ? "HTTP " + std::to_string(code) : err) + "): open it by hand, or use a VPN");
            return;
        }
        g_public = pub;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_mapping = {control.host, control.path, service, control.port, port};
        }
        std::atexit(Close);
        std::string line = "UDP port " + std::to_string(port) + " opened on the router for " + Ip(local);
        if (!pub) line += ", its public address unknown";
        else if (Unreachable(pub))
            line += ", but the router's own address " + Ip(pub) +
                    " is not public (the provider shares one, CGNAT): players past it cannot reach you, use a VPN";
        else line += ", public address " + Ip(pub);
        Say(line);
        return;
    }
    Say("the router has no UPnP port mapping: open UDP port " + std::to_string(port) + " on it by hand, or use a VPN");
}

}  // namespace

void OpenUdp(uint16_t port) {
    static std::once_flag once;
    std::call_once(once, [port] { std::thread(Map, port).detach(); });
}

std::string Status() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_status;
}

uint32_t PublicAddress() { return g_public; }

}  // namespace upnp
