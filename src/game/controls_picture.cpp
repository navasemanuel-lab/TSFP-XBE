// The Controls screen's picture of the controller, and the halo behind it, swapped for the controller in use: an Xbox
// Series, PlayStation 4, PlayStation 5 or Switch Pro controller drawn by pad_picture.cpp. The keyboard and other pads
// keep the game's Xbox controller.
// The screen draws them in 0x2E42F0 (x, y, menu): sprite 676, xbox_controllerglow (128 by 128), 240 by 152 about the
// point, tinted, then sprite 675, xbox_controller (512 by 256, DXT3), 229 by 144. A sprite's texture id is at +0xC of
// its row in the table 0x4D2D58 (0x18 bytes each); the texture objects (0x34 bytes, the D3D texture header first) sit
// in an array. Each time the screen draws, the two headers are pointed at the drawn textures, or back at the game's.
#include <cstdint>
#include <iterator>
#include <vector>

#include "common/icons.h"
#include "common/pad_picture.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kTextureObjects = 0x4B85C8;  // points at the pointer to the texture object array
constexpr uint32_t kObjectSize = 0x34;
constexpr uint32_t kSprites = 0x4D2D58, kSpriteSize = 0x18;
constexpr uint32_t kFormatA8R8G8B8 = 0x06;  // swizzled

struct Part {
    uint32_t sprite, w, h;  // drawn at twice the game's size for sharpness
    bool glow;
};
constexpr Part kParts[] = {{675, 1024, 512, false}, {676, 256, 256, true}};
constexpr size_t kStyles = 6;

struct Made {
    bool tried = false;
    uint32_t data = 0, format = 0;
};
Made g_made[kStyles][2];
uint32_t g_game_data[2] = {}, g_game_format[2] = {};  // the game's own, while ours is in place

uint32_t Log2(uint32_t v) {
    uint32_t n = 0;
    while ((1u << n) < v) n++;
    return n;
}

// the console's texel order for a power of two texture: x and y bits interleaved while both dimensions last
uint32_t Swizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t off = 0, bit = 0;
    for (uint32_t sx = 1, sy = 1; sx < w || sy < h;) {
        if (sx < w) {
            off |= (x & sx ? 1u : 0u) << bit++;
            sx <<= 1;
        }
        if (sy < h) {
            off |= (y & sy ? 1u : 0u) << bit++;
            sy <<= 1;
        }
    }
    return off;
}

// a style's picture or halo as a console texture in guest memory, once
const Made& Make(icons::Style style, size_t i) {
    Made& m = g_made[size_t(style)][i];
    if (m.tried) return m;
    m.tried = true;
    const Part& p = kParts[i];
    std::vector<uint32_t> px = p.glow ? pad_picture::Glow(style, p.w, p.h) : pad_picture::Picture(style, p.w, p.h);
    if (px.empty()) return m;
    uint32_t va = mem::AllocContiguous(p.w * p.h * 4, 0, 0xFFFFFFFF, 0x1000);
    if (!va) return m;
    auto* out = static_cast<uint32_t*>(kernel::HostPtr(va));
    for (uint32_t y = 0; y < p.h; y++)
        for (uint32_t x = 0; x < p.w; x++) out[Swizzle(x, y, p.w, p.h)] = px[size_t(y) * p.w + x];
    m.data = mem::PhysicalFromVirtual(va);
    // DMA, dimensions and border bits as the console's 2D textures have them, one level
    m.format = 0x29 | kFormatA8R8G8B8 << 8 | 1u << 16 | Log2(p.w) << 20 | Log2(p.h) << 24;
    LOGI("controls: drew the %s %s", icons::Name(style), p.glow ? "halo" : "controller");
    return m;
}

// the sprite's texture header, 0 while it is not loaded
uint32_t Header(uint32_t sprite) {
    uint32_t objects = kernel::R32(kTextureObjects);
    if (!objects || !(objects = kernel::R32(objects))) return 0;
    uint32_t id = kernel::R32(kSprites + sprite * kSpriteSize + 0xC);
    return id == 0xFFFFFFFF ? 0 : objects + id * kObjectSize;
}

bool Ours(size_t i, uint32_t data) {
    for (const auto& style : g_made)
        if (style[i].data && style[i].data == data) return true;
    return false;
}

void Swap() {
    icons::Style style = icons::Current();
    for (size_t i = 0; i < std::size(kParts); i++) {
        uint32_t hdr = Header(kParts[i].sprite);
        if (!hdr) continue;
        uint32_t data = kernel::R32(hdr + 4);
        bool ours = Ours(i, data);
        const Made& m = Make(style, i);
        if (!m.data) {
            // the game's picture for this device
            if (ours) {
                kernel::W32(hdr + 4, g_game_data[i]);
                kernel::W32(hdr + 12, g_game_format[i]);
            }
            continue;
        }
        if (data == m.data) continue;
        if (!ours) {
            g_game_data[i] = data;
            g_game_format[i] = kernel::R32(hdr + 12);
        }
        kernel::W32(hdr + 4, m.data);
        kernel::W32(hdr + 12, m.format);
    }
}

}  // namespace

// the Controls screen's controller and halo
X86_EXTERN_FUNC(__imp__sub_002E42F0);
X86_FUNC(sub_002E42F0) {
    Swap();
    __imp__sub_002E42F0(ctx, base);
}
