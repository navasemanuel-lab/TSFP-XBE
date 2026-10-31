// MapMaker's cursor follows the mouse. Each player's cursor (0x7BA260 + player * 0x1E0) keeps its place as a percent
// at +0x18 and +0x1C and in UI canvas pixels at +0x20 and +0x24, within +0x48..+0x54; pixels are the percent times
// the scale at 0x7BA038 (x) and 0x7BA188 (y), plus the origin at 0x7BA180 and 0x7BA184. The stick still moves it:
// the mouse sets it only when it has moved.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "common/mouse.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game {
void MapMakerEditorSeen();  // game/discord.cpp: the presence shows the editor
}

namespace {

constexpr uint32_t kMenuPlayer = 0x7BA9E0;
constexpr uint32_t kCursors = 0x7BA260, kCursorSize = 0x1E0;
constexpr uint32_t kScaleX = 0x7BA038, kScaleY = 0x7BA188, kOriginX = 0x7BA180, kOriginY = 0x7BA184;
constexpr uint32_t kCanvasW = 0x4E8CF0, kCanvasH = 0x4E8CF4;

float F(uint32_t va) { return std::bit_cast<float>(kernel::R32(va)); }
void SetF(uint32_t va, float v) { kernel::W32(va, std::bit_cast<uint32_t>(v)); }

void FollowMouse() {
    if (kernel::R32(kMenuPlayer) != 0) return;  // the keyboard and mouse are player 1's
    mouse::InGameCursor();
    static float last_x = -1, last_y = -1;
    mouse::Pointer p = mouse::MenuPointer(false);
    if (!p.valid || (p.x == last_x && p.y == last_y)) return;
    last_x = p.x;
    last_y = p.y;
    uint32_t c = kCursors;
    float sx = F(kScaleX), sy = F(kScaleY), ox = F(kOriginX), oy = F(kOriginY);
    if (!(std::fabs(sx) > 1e-3f && std::fabs(sy) > 1e-3f && std::isfinite(ox) && std::isfinite(oy))) return;
    float x = p.x * float(kernel::R32(kCanvasW)), y = p.y * float(kernel::R32(kCanvasH));
    float x0 = F(c + 0x48), y0 = F(c + 0x4C), x1 = F(c + 0x50), y1 = F(c + 0x54);
    if (x0 < x1) x = std::clamp(x, x0, x1);
    if (y0 < y1) y = std::clamp(y, y0, y1);
    SetF(c + 0x20, x);
    SetF(c + 0x24, y);
    SetF(c + 0x18, (x - ox) / sx);
    SetF(c + 0x1C, (y - oy) / sy);
}

}  // namespace

// The editor's item limits: a table of the 37 item types (0x524BD8, 0x18 each: +8 the name's text, +0xC how many a map
// may hold) as on PS2; the title update raised some to 60 and 120. Each is 255 here, after the TSO Community Patch: the
// editor counts the map's items by type in 32-bit counts (0x303460) and reads the limit back for its message. The map's
// own size (below) still bounds the whole.
namespace {

constexpr uint32_t kItemLimits = 0x524BE4, kItemTypes = 37, kItemSize = 0x18, kItemLimit = 255;

void RaiseItemLimits() {
    for (uint32_t i = 0; i < kItemTypes; i++) {
        uint32_t at = kItemLimits + i * kItemSize;
        if (kernel::R32(at) < kItemLimit) kernel::W32(at, kItemLimit);
    }
}

}  // namespace

// PlaceItem(): whether the item at the cursor can be placed, by its type's limit
X86_EXTERN_FUNC(__imp__sub_00303460);
X86_FUNC(sub_00303460) {
    RaiseItemLimits();
    __imp__sub_00303460(ctx, base);
}

// the editor's other check against the table
X86_EXTERN_FUNC(__imp__sub_00303E10);
X86_FUNC(sub_00303E10) {
    RaiseItemLimits();
    __imp__sub_00303E10(ctx, base);
}

// The map's memory. A map is one block (0x784014, taken in the front end at 0x2FE200 and 0x2FE230 as 32 KB of the
// kind 2 memory): "C77667" and version 0x12D, then chunks (id, size, data) up to the "C7766A" end; the "C77668" chunk
// keeps at +0x248 the bytes the map uses. Every walk of the chunks stops at the block's 0x7FE0th byte, the editor
// refuses what would take the used bytes past it (its "memory full"), and a map file holds those 0x7FE0 bytes.
// Here the block is kMapSize, taken once from the console's memory and kept, and each of those places
// ([[hook]]s in config/game.toml) reads kMapEnd for 0x7FE0. Files saved smaller still load: a map file is read at each
// size kFileEnds lists, the console's 0x7FE0 last. System Link still sends a map's first 32 KB (0x3563A0), so a
// larger map is for this machine and co-op on it.
namespace {

constexpr uint32_t kMapSize = 0x80000, kOldEnd = 0x7FE0, kMapEnd = kMapSize - 0x20, kGrowth = kMapEnd - kOldEnd;
// the map bytes of the files this and earlier builds wrote, the largest first: 512 KB, 128 KB, the console's 32 KB
constexpr uint32_t kFileEnds[] = {kMapEnd, 0x20000 - 0x20, kOldEnd};
// a file's bytes beyond its map, its header and signature, are under this
constexpr uint32_t kFileExtra = 0x1000;
constexpr uint32_t kMeterScale = 0x49F7AC;

uint32_t MapBlock() {
    static uint32_t block = [] {
        uint32_t va = mem::AllocContiguous(kMapSize, 0, 0xFFFFFFFF, 0x1000);
        // the editor's memory bar (0x30C180) is the used bytes times 1/0x7FE0
        if (va) kernel::W32(kMeterScale, std::bit_cast<uint32_t>(1.0f / float(kMapEnd)));
        LOGR("mapmaker: the map's memory is %u KB at %08X", kMapSize >> 10, va);
        return va;
    }();
    return block;
}

}  // namespace

// the front end's new map: the block in place of the 32 KB just taken (eax), cleared as the game clears its own
void MapMakerMapBlock(X86Context& ctx, uint8_t*) {
    if (uint32_t block = MapBlock()) {
        std::memset(kernel::HostPtr(block), 0, kMapSize);
        ctx.eax = block;
    }
}

// a walk's end, 0x7FE0 past the map, made kMapEnd past it
void MapMakerEndEax(X86Context& ctx, uint8_t*) { ctx.eax += kGrowth; }
void MapMakerEndEcx(X86Context& ctx, uint8_t*) { ctx.ecx += kGrowth; }
void MapMakerEndEdx(X86Context& ctx, uint8_t*) { ctx.edx += kGrowth; }
void MapMakerEndEdi(X86Context& ctx, uint8_t*) { ctx.edi += kGrowth; }
void MapMakerEndEbp(X86Context& ctx, uint8_t*) { ctx.ebp += kGrowth; }

// the bytes a file save writes, pushed as 0x7FE0
void MapMakerEndPushed(X86Context& ctx, uint8_t*) { kernel::W32(ctx.esp, kMapEnd); }

// before "cmp eax, 0x7FE0" (signed, eax not read after): the bytes the map would use, as the test against kMapEnd
// would find them
void MapMakerFullEax(X86Context& ctx, uint8_t*) {
    int32_t used = int32_t(ctx.eax);
    ctx.eax = uint32_t(used < int32_t(kMapEnd) ? std::min(used, int32_t(kOldEnd) - 1) : std::max(used, int32_t(kOldEnd)));
}

// LoadFile(slot, name, format, buffer, size, flags): 1 when the file held exactly size bytes. A map file (0x7FE0
// asked) is read at each of kFileEnds until one fits, what lies past it cleared
X86_EXTERN_FUNC(__imp__sub_00025280);
X86_FUNC(sub_00025280) {
    uint32_t buffer = kernel::R32(ctx.esp + 0x10), size = kernel::R32(ctx.esp + 0x14);
    if (size != kOldEnd || !buffer || buffer != MapBlock()) {
        __imp__sub_00025280(ctx, base);
        return;
    }
    uint32_t sp = ctx.esp, ret = kernel::R32(sp);
    for (uint32_t end : kFileEnds) {
        ctx.esp = sp;
        kernel::W32(sp, ret);
        kernel::W32(sp + 0x14, end);
        __imp__sub_00025280(ctx, base);
        if (ctx.eax != 1) continue;
        std::memset(kernel::HostPtr(buffer + end), 0, kMapSize - end);
        if (end != kMapEnd) LOGR("mapmaker: a map file of %u KB", (end + 0x20) >> 10);
        break;
    }
    kernel::W32(sp + 0x14, kOldEnd);
}

// MapListEntry(list, file): a file larger than 0x8000 is marked, one under 0x7FE0 labelled; a file of one of
// kFileEnds is judged as one of the console's
X86_EXTERN_FUNC(__imp__sub_00190720);
X86_FUNC(sub_00190720) {
    uint32_t file = kernel::R32(ctx.esp + 8), size = file ? kernel::R32(file + 0x14C) : 0, judged = size;
    for (uint32_t end : kFileEnds)
        if (end != kOldEnd && size >= end && size - end < kFileExtra) {
            judged = size - end + kOldEnd;
            break;
        }
    if (judged != size) kernel::W32(file + 0x14C, judged);
    __imp__sub_00190720(ctx, base);
    if (judged != size) kernel::W32(file + 0x14C, size);
}

// A ghost NPC, as What Lies Below's: the character with the ghosts' behaviour (0x40000000, 0x2BA300), which floats,
// draws through the ghost effect (0x121BF0, 0x1263A0) and dies to the Ghost Gun. An NPC's AI mode (its entry +0x34,
// the chunk's +0x20) is the editor's "Type" (0x31CB90): Unused, Stationary, Patrol, Spawn & Attack, Spawn & Patrol
// (0 to 4; 5 is another mode's); kGhostMode is one more. When the map is played its NPCs become spawners (0x241DB9: +0 and +4 the
// behaviour mask, 0x1_10000000 for a creature), and a ghost's mask is the ghosts' instead, its anims loaded as the
// level loads them (0x210580).
namespace {

constexpr uint8_t kGhostMode = 6;
constexpr char kGhostName[] = "Ghost";
constexpr uint32_t kGhostBehaviour = 0x40000000;
constexpr uint32_t kGhostAnims[] = {0x249, 0x24A, 0x24B, 0x24C};
constexpr uint32_t kAddOption = 0x71800, kLoadAnim = 0xA5E40;
// a spawner's flags (+0x58): the Ghost Gun's beam (damage type 0x19) takes only characters whose spawner has this
// (0xBB561), as the level's ghosts have
constexpr uint32_t kSpawnerFlags = 0x58, kGhostGunTarget = 0x40000;
// not offered yet: the Ghost Gun does not trap a ghost of another skeleton (it stays in its dive, state 2)
constexpr bool kOfferGhost = false;

}  // namespace

// the Type list, before it is bound to the NPC's field (ebp): Ghost after the game's five, its text in a page of
// its own
void MapMakerGhostOption(X86Context& ctx, uint8_t*) {
    if (!kOfferGhost) return;
    static uint32_t name = [] {
        uint32_t va = mem::AllocContiguous(0x1000, 0, 0xFFFFFFFF, 0x1000);
        if (va) std::memcpy(kernel::HostPtr(va), kGhostName, sizeof(kGhostName));
        return va;
    }();
    if (name) kernel::CallGuest(ctx, kAddOption, {name, kGhostMode});
}

// a played map's NPC (ebp, its chunk) made a spawner (esi), before its patrol is set
void MapMakerGhostSpawner(X86Context& ctx, uint8_t*) {
    if (*static_cast<uint8_t*>(kernel::HostPtr(ctx.ebp + 0x20)) != kGhostMode) return;
    kernel::W32(ctx.esi, kGhostBehaviour);
    kernel::W32(ctx.esi + 4, 0);
    kernel::W32(ctx.esi + kSpawnerFlags, kernel::R32(ctx.esi + kSpawnerFlags) | kGhostGunTarget);
    for (uint32_t anim : kGhostAnims) kernel::CallGuest(ctx, kLoadAnim, {anim});
    LOGR("mapmaker: NPC at pad %u (cast slot %u) is a ghost", kernel::R32(ctx.esi + 0x10),
         uint32_t(kernel::R16(ctx.esi + 0x16)));
}

// DrawCursor(): the current menu player's cursor, each frame it shows
X86_EXTERN_FUNC(__imp__sub_0030CF60);
X86_FUNC(sub_0030CF60) {
    FollowMouse();
    game::MapMakerEditorSeen();
    __imp__sub_0030CF60(ctx, base);
}
