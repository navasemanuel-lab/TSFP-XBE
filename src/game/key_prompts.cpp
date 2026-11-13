// The keys in a prompt's text while the keyboard and mouse are in use. A prompt names a control by a text code (0x0B
// then a character): the face buttons and START are icons, which common/icons.cpp redraws as keys. The triggers, D-pad
// and sticks have no icon of their own: each key of one is written here as the null icon (code '8') and drawn as that
// key's picture, bound to the icon in turn (below); a key with no picture is written as its label ("[Q/V]"), as the
// TimeSplitters FP PC port does (TheDoffman/timesplitters-future-perfect-pc). Both the measure (0x3FCF0: width out,
// height out, text, first line only) and the draw (0x40FA0: position, baseline, wrap width, text, glyph callback, its
// state) get the same text, so wrapping and centring fit it. The rewritten text and a copy of the call's arguments go
// below the caller's stack.
#include <cstring>
#include <string>
#include <vector>

#include "common/controls.h"
#include "common/icons.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr size_t kLongest = 2048;
constexpr uint32_t kFlushSprites = 0x22FF0;  // submits the quads queued for the texture bound
constexpr uint32_t kViewPlayer = 0x7B0C7C;    // the player whose view is drawn: +4 its input slot
constexpr uint32_t kPlayers = 0x790950;

// the input slot of the player whose view is being drawn when several play on this screen, so each one's prompts show
// the device that player uses; -1 for one player, whose prompts follow the device used last
int ViewSlot() {
    if (kernel::R32(kPlayers) < 2) return -1;
    uint32_t viewer = kernel::R32(kViewPlayer);
    if (viewer < 0x10000 || !mem::IsCommitted(viewer + 8)) return -1;
    uint32_t slot = kernel::R32(viewer + 4);
    return slot < 4 ? int(slot) : -1;
}

bool HasCodes(uint32_t text) {
    for (size_t i = 0; i < kLongest; i++) {
        char c = kernel::G<char>(text + uint32_t(i));
        if (!c) return false;
        if (c == 0x0B) return true;
    }
    return false;
}

// a prompt code's keys as the null icon, one for each, their labels added to the pictures; empty when a key has none
std::string PromptPictures(char code, std::vector<std::string>& pictures) {
    std::vector<std::string> labels = controls::PromptLabels(code);
    if (labels.empty()) return {};
    for (const std::string& label : labels)
        if (!icons::HasKeyPicture(label)) return {};
    std::string out;
    for (const std::string& label : labels) {
        out += char(0x0B);
        out += '8';
        pictures.push_back(label);
    }
    return out;
}

// the text with its codes written as keys, empty when nothing changed
std::string Rewrite(uint32_t text, std::vector<std::string>& pictures) {
    std::string out;
    bool changed = false;
    pictures.clear();
    for (size_t i = 0; i < kLongest; i++) {
        char c = kernel::G<char>(text + uint32_t(i));
        if (!c) return changed ? out : std::string();
        if (c == 0x0B) {
            char code = kernel::G<char>(text + uint32_t(i) + 1);
            std::string keys = code ? PromptPictures(code, pictures) : std::string();
            if (keys.empty() && code) keys = controls::PromptKeys(code);
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

// calls the original with the text argument (0 the first) rewritten, when the keyboard is in use and it names keys; a
// draw gives the keys' pictures to the icon as it binds it
template <void (*Original)(X86Context&, uint8_t*)>
void WithKeys(X86Context& ctx, uint8_t* base, uint32_t text_arg, uint32_t args, bool draw) {
    uint32_t saved = ctx.esp, text = kernel::R32(saved + 4 + 4 * text_arg);
    std::vector<std::string> pictures;
    icons::Style style = controls::PromptStyleFor(ViewSlot());
    // the icons of this player's device: the quads queued for another's are drawn first
    if (draw && text && style != icons::Current() && HasCodes(text)) {
        kernel::CallGuest(ctx, kFlushSprites, {});
        icons::Update(style, false);
    }
    std::string keys = text && style == icons::Style::Keyboard ? Rewrite(text, pictures) : std::string();
    if (keys.empty()) {
        Original(ctx, base);
        return;
    }
    uint32_t scratch = (saved - uint32_t(keys.size() + 1) - 64) & ~15u, frame = scratch - (args + 1) * 4;
    std::memcpy(&kernel::G<char>(scratch), keys.c_str(), keys.size() + 1);
    std::memcpy(&kernel::G<char>(frame), &kernel::G<char>(saved), (args + 1) * 4);  // the return and the arguments
    kernel::W32(frame + 4 + 4 * text_arg, scratch);
    ctx.esp = frame;
    if (draw && !pictures.empty()) icons::BeginKeyPictures(pictures);
    Original(ctx, base);
    ctx.esp = saved + 4;  // its ret, from the caller's own frame (cdecl: the caller drops its arguments)
    if (draw && !pictures.empty() && icons::EndKeyPictures()) {
        kernel::CallGuest(ctx, kFlushSprites, {});  // the last picture's quad is drawn before the icon is given back
        icons::GiveBackNullIcon();
    }
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_0003FCF0);
X86_FUNC(sub_0003FCF0) { WithKeys<__imp__sub_0003FCF0>(ctx, base, 2, 4, false); }

X86_EXTERN_FUNC(__imp__sub_00040FA0);
X86_FUNC(sub_00040FA0) { WithKeys<__imp__sub_00040FA0>(ctx, base, 3, 6, true); }

// A texture bound as the current one for the quads that follow (the icons' too): the null icon, in a text with keys
// left to draw, first has the quads queued so far submitted (they must not take the next picture) and then takes it.
X86_EXTERN_FUNC(__imp__sub_0001C720);
X86_FUNC(sub_0001C720) {
    if (icons::KeyPictureWanted(kernel::R32(ctx.esp + 4))) {
        kernel::CallGuest(ctx, kFlushSprites, {});
        icons::PutKeyPicture();
    }
    __imp__sub_0001C720(ctx, base);
}
