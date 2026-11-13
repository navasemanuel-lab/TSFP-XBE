// System Link: what the Xbox read as zeros in guest page 0 and the PC faults on, passed over while a player's records,
// block or actors are not made yet (a level's start, a movie).
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

using kernel::R32;

constexpr uint32_t kSessionUp = 0x76B108, kLocalNode = 0x774028;
constexpr uint32_t kScene = 0x79094C, kPlayers = 0x790950;
constexpr uint32_t kPlayerArray = 0x7B0C48, kPlayerSize = 0x1584, kActors = 0x7356D8, kActorSize = 0xEA0;

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

}  // namespace

// NetworkSlot(local): the network player of this game's local player, through its record (local node [0x774028]:
// +0x20 the records' count, +0xC the records, +0x3C a record's player): the local index before the records are made
X86_EXTERN_FUNC(__imp__sub_00356800);
X86_FUNC(sub_00356800) {
    uint32_t local = R32(ctx.esp + 4), node = R32(kLocalNode);
    if (R32(kScene) != 0x65 && R32(kSessionUp) && Valid(node, 0x24) && local < 4 &&
        int32_t(R32(node + 0x20)) <= int32_t(R32(kPlayers)) && !Valid(R32(node + 0xC + local * 4), 0x40))
        return kernel::Return(ctx, local, 0);
    __imp__sub_00356800(ctx, base);
}

// SendPlayerBlock(record): sends the 0x7C bytes its player's +0x1500 points at when they change: nothing while there
// are none
X86_EXTERN_FUNC(__imp__sub_00358EB0);
X86_FUNC(sub_00358EB0) {
    uint32_t record = R32(ctx.esp + 4);
    if (R32(kSessionUp) && Valid(record, 0x40) && Valid(R32(kPlayerArray), 4)) {
        uint32_t player = R32(kPlayerArray) + R32(record + 0x3C) * kPlayerSize;
        if (!Valid(player + 0x1500, 4) || !Valid(R32(player + 0x1500), 0x7C)) return kernel::Return(ctx, 0, 0);
    }
    __imp__sub_00358EB0(ctx, base);
}

// SendActorStates(): every player's actor (the pool at 0x7356D8) packed for the others: nothing until the pool is made
X86_EXTERN_FUNC(__imp__sub_0035A480);
X86_FUNC(sub_0035A480) {
    if (R32(kSessionUp) && !Valid(R32(kActors), kActorSize)) return kernel::Return(ctx, 0, 0);
    __imp__sub_0035A480(ctx, base);
}
