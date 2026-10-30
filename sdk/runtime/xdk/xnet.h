#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace xnet {

void CloseSockets();
// System Link past the local network (app::Settings link_mode, or --link-host [port] / --link-join <address>[:port]
// on the command line), as a line for the player: empty when off
std::string LinkStatus();

// Side datagrams, game data beside the title's own (at most 4096 bytes): sent to every peer the title sent a UDP
// datagram to in the last 10 s, from the socket it last used for that peer, so they go where the title's own do
// (the LAN, a VPN, the hosted link). The receiver gets them on a network thread, with the sender's address as the
// title sees it; the title never reads them.
void SideSend(const void* data, size_t len);
void SideReceiver(void (*fn)(const uint8_t* data, size_t len, uint32_t from));  // up to 4 of them

// the traffic since the last call, as one line: datagrams/bytes out and in by peer, failures
std::string TrafficLine();

}  // namespace xnet
