#pragma once
#include <cstdint>
#include <string>

// A UDP port opened on the router by UPnP (an Internet Gateway Device's WANIPConnection or WANPPPConnection): the
// router is found by SSDP, its description read and AddPortMapping asked, all on plain sockets. A router without
// UPnP, or with it off, leaves the port as it was: the player opens it by hand.
namespace upnp {

// in the background: maps `port` to this PC, and takes it off again when the program exits normally
void OpenUdp(uint16_t port);
// what came of it: empty while it is being tried, then a line for the player ("port 7778 opened on the router,
// public address 1.2.3.4", or why not)
std::string Status();
// the router's public address, 0 until known (network order)
uint32_t PublicAddress();

}  // namespace upnp
