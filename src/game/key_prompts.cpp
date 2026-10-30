// The keys in a prompt's text while the keyboard and mouse are in use. A prompt names a control by a text code (0x0B
// then a character): the face buttons and START are icons, which common/icons.cpp redraws as keys, the triggers,
// D-pad and sticks are written here as their keys ("[Q/V]"), as the TimeSplitters FP PC port does
// (TheDoffman/timesplitters-future-perfect-pc). Both the measure (0x3FCF0: width out, height out, text, first line
// only) and the draw (0x40FA0: position, baseline, wrap width, text, glyph callback, its state) get the same text, so
// wrapping and centring fit it. The rewritten text and a copy of the call's arguments go below the caller's stack.
#include <cstring>
#include <string>

#include "common/controls.h"
#include "xdk/hle.h"

namespace {

constexpr size_t kLongest = 2048;

// the text with its codes written as keys, empty when nothing changed
std::string Rewrite(uint32_t text) {
    std::string out;
    bool changed = false;
    for (size_t i = 0; i < kLongest; i++) {
        char c = kernel::G<char>(text + uint32_t(i));
        if (!c) return changed ? out : std::string();
        if (c == 0x0B) {
            char code = kernel::G<char>(text + uint32_t(i) + 1);
            std::string keys = code ? controls::PromptKeys(code) : std::string();
            if (!keys.empty()) {
                out += keys;
                changed = true;
                i++;
                continue;
            }
        }
        out += c;
    }
    return {};  // no end found: left as it is
}

// calls the original with the text argument (0 the first) rewritten, when the keyboard is in use and it names keys
template <void (*Original)(X86Context&, uint8_t*)>
void WithKeys(X86Context& ctx, uint8_t* base, uint32_t text_arg, uint32_t args) {
    uint32_t saved = ctx.esp, text = kernel::R32(saved + 4 + 4 * text_arg);
    std::string keys = text && controls::KeyboardActive() ? Rewrite(text) : std::string();
    if (keys.empty()) {
        Original(ctx, base);
        return;
    }
    uint32_t scratch = (saved - uint32_t(keys.size() + 1) - 64) & ~15u, frame = scratch - (args + 1) * 4;
    std::memcpy(&kernel::G<char>(scratch), keys.c_str(), keys.size() + 1);
    std::memcpy(&kernel::G<char>(frame), &kernel::G<char>(saved), (args + 1) * 4);  // the return and the arguments
    kernel::W32(frame + 4 + 4 * text_arg, scratch);
    ctx.esp = frame;
    Original(ctx, base);
    ctx.esp = saved + 4;  // its ret, from the caller's own frame (cdecl: the caller drops its arguments)
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_0003FCF0);
X86_FUNC(sub_0003FCF0) { WithKeys<__imp__sub_0003FCF0>(ctx, base, 2, 4); }

X86_EXTERN_FUNC(__imp__sub_00040FA0);
X86_FUNC(sub_00040FA0) { WithKeys<__imp__sub_00040FA0>(ctx, base, 3, 6); }
