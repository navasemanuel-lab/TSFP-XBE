// Mansion of Madness's pool table, as the preview had it: Dr. Pustule (the zombie scientist) at Blanche Deadwood on
// the table, each playing their half of a scene the final game cut. The final game kept Blanche and both halves'
// animations and dropped him and her half's partner.
//
// The mission's setup is data/mansion2.raw, loaded whole by LoadFile (0x62F50, from 0x69F30): "SETU", then sections
// {offset, count, size} from +0xC, the offsets from the file's start. Those this needs:
// - +0xC the characters, 0x98 each: +8 the name (4 characters), +0xC who it is for (0x200000 one player, 0x400000 two),
//   +0x10 its pad, +0x16 its type (the mission's character list, 0x53D2D4 + 2*0xA4: 7 Blanche, 0xD Dr. Pustule, whom
//   nothing names), +0x28 its group, +0x68 its sequence of animations. Blanche is PZ03 (pad 0x2355, group PZOM, PZ08 for
//   two, sequence SSGF); the preview had PZ02 beside her (pad 0x2356, type 0xD, group POOM, sequence BDSI), in two
//   records as hers, and her in group POMO.
// - +0xD8 the animations, 0x48 each: +0x20 the partner's group, +0x28 the pad they meet at, +0x34 flags, +0x30 the
//   sequence, +0x38 and +0x3C the animation. BDSI's first (TIFC 0x155) still has its partner, POMO at pad 0x429; SSGF's
//   first (TISH 0x153) lost POOM, the pad and flags (the preview's 0x81900, a flag added since: 0x103200).
// - +0x90 the events, 0xC8 each: +8 what it does (1 Jo-Beth says text +0x10, +0x18 who), +0x40 when (2 at once, 3 after
//   the event before it, 4 after the event named +0x44, 6 the character +0x44 near pad +0x48, 0xB group +0x44 dead),
//   +0x60 2 and a second condition +0x64 (its name +0x68), +0x58 an event that stops it (+0x5C 3), +0xA4 a delay,
//   +0xAC its name, +0xB4 who it is for (0x200000 one player, 0x400000 two, neither both). The preview's Jo-Beth said
//   four lines at the table (its events 92-98); the final game kept two of the texts with their voices (0x102E, 0x102F)
//   and dropped the events, and the other two texts, whose voices stayed (008_jobeth591b, 009_jobeth591c). Their texts
//   are here (ids past the game's last, 0x1DBB) and the voices given them by VoiceOf (0x461C0); the lines stop once the
//   two at the table are dead (DBID).
// - The room's events are the preview's (TableLines): Jo-Beth's lines at the door and at the table, the boogers, then
//   the zombies once the scene is over and she is at the table (ZB01), the wall blown at once and the zombie behind
//   it 6 s after; for two no scene, the zombies as a player comes to the table's side (ZB0C). The preview's "scene
//   over" was a condition (8, value 0xA) the final game no longer has: its table (0x7A2D24 + n*0x60) is now within
//   the characters' (0x7A2D00), and it never holds. Here those wait on condition 8 with a value of their own, which the
//   condition callback (0x235240) holds once Jo-Beth's last line is said and a breath (BGND) or one of the two at the
//   table is dead (PDD1, PDD2). An event is over with its runtime's (the array at [0x78CD90], 0x48 each, +0 the
//   record) +0x34 set (0x234270).
// The characters and events go at the file's end as sections of their own, the others where they were: nothing else
// moves.
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kGameMode = 0x7DE454;  // its second byte 10 in Story
constexpr char kSetup[] = "data/mansion2.raw";
constexpr uint32_t kCharacters = 0xC, kEvents = 0x90, kAnimations = 0xD8;  // their sections in the header
constexpr uint32_t kCharacterSize = 0x98, kEventSize = 0xC8, kAnimationSize = 0x48;
constexpr uint32_t kName = 8, kFor = 0xC, kPad = 0x10, kType = 0x14, kGroup = 0x28, kSequence = 0x68;
constexpr uint32_t kPartner = 0x20, kMeet = 0x28, kFlags = 0x34, kOwnSequence = 0x30, kAnimSet = 0x38, kAnim = 0x3C;
constexpr uint32_t kDoes = 8, kSays = 0x10, kWho = 0x18, kWhen = 0x40, kWhenName = 0x44, kWhenPad = 0x48,
                   kStopper = 0x58, kStopperKind = 0x5C, kAnd = 0x60, kWhen2 = 0x64, kWhen2Name = 0x68, kWhen2Pad = 0x6C,
                   kDelay = 0xA4,
                   kEventName = 0xAC, kEventFor = 0xB4;
constexpr uint32_t kSay = 1, kAtOnce = 2, kAfterLast = 3, kAfterEvent = 4, kNearPad = 6, kOldCondition = 8,
                   kGroupDead = 0xB;
constexpr uint32_t kSceneOver = 0x4D4F4D21;  // condition 8's value: the scene at the table over
constexpr uint32_t kNever = 0x4E4F4E45;      // and one that never holds: an event out of play
constexpr uint32_t kWall = 0x2C, kBlast = 0x42, kZombiesGo = 0x3A;  // what the wall's events after LAMO do
constexpr uint32_t kOnePlayer = 0x200700, kTwoPlayers = 0x400700, kBothPlayers = 0x700;
constexpr uint32_t kEww = 0x102E, kBoogers = 0x102F;  // the final game's texts, with their voices
struct Line {
    uint32_t text, voice;  // voice: the sound event (sounds_e.pak's sounddata, in order)
    const char* says;
};
constexpr Line kCutLines[] = {
    {0x1E00, 2215, "Oh!  err sorry!  We didn't realise you were err..."},  // SFX_008_JOBETH591B
    {0x1E01, 2216, "Ohhh!  He's just trying to pull it out"},             // SFX_009_JOBETH591C
};
uint32_t g_texts = 0;  // the lines' strings in guest memory, one after the other

// a name as the setup has it, the 4 characters read as a big-endian number
constexpr uint32_t Tag(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]);
}

bool Story() { return ((kernel::R32(kGameMode) >> 8) & 0xFF) == 10; }

uint32_t Get(const std::vector<uint8_t>& d, uint32_t at) {
    uint32_t v;
    std::memcpy(&v, &d[at], 4);
    return v;
}
void Put(std::vector<uint8_t>& d, uint32_t at, uint32_t v) { std::memcpy(&d[at], &v, 4); }

// a section's records, when the file has them all
bool Section(const std::vector<uint8_t>& d, uint32_t header, uint32_t size, uint32_t& at, uint32_t& count) {
    if (header + 12 > d.size()) return false;
    at = Get(d, header), count = Get(d, header + 4);
    return Get(d, header + 8) == size && uint64_t(at) + uint64_t(count) * size <= d.size();
}

uint32_t FromFloat(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

// a section's records moved to the file's end, with more after them
void MoveToEnd(std::vector<uint8_t>& d, uint32_t header, uint32_t size, uint32_t at, uint32_t count,
               const std::vector<uint8_t>& more) {
    uint32_t moved = (uint32_t(d.size()) + 15) & ~15u;
    std::vector<uint8_t> all(d.begin() + at, d.begin() + at + count * size);
    all.insert(all.end(), more.begin(), more.end());
    d.resize(moved);
    d.insert(d.end(), all.begin(), all.end());
    Put(d, header, moved);
    Put(d, header + 4, count + uint32_t(more.size() / size));
}

// The pool table's room as the preview's setup had it (its events 88 to 125, in its order), made from the final game's
// own records of that room (events 120 to 144): those are put out of play (a condition 8 that never holds, no name) and
// the preview's come after the setup's events, each a copy of the final game's record that does the same, with the
// preview's conditions, delays, stoppers and names. The preview's actions 0x2B and 0x39 are the final game's 0x2C and
// 0x3A; its texts are the final game's (Jo-Beth's two lost lines here, 0x1E00 and 0x1E01). The preview's wall (0x2B)
// blew by itself; the final game's has its blast apart (0x42, at pad 0x5B2), kept with it. Empty when the events are
// not the ones this knows.
std::vector<uint8_t> TableLines(std::vector<uint8_t>& d, uint32_t events, uint32_t nevents) {
    constexpr uint32_t kFirst = 120, kLast = 144;
    if (nevents <= kLast) return {};
    auto at = [&](uint32_t i) { return events + i * kEventSize; };
    auto is = [&](uint32_t i, uint32_t does, uint32_t players, uint32_t name) {
        uint32_t r = at(i);
        return Get(d, r + kDoes) == does && Get(d, r + kEventFor) == players && (!name || Get(d, r + kEventName) == name);
    };
    for (uint32_t i = 0; i < nevents; i++)
        if (Get(d, at(i) + kEventName) == Tag("DBID")) return {};
    if (!is(120, 0x12, kOnePlayer, 0) || !is(121, 0xD, kOnePlayer, 0) || !is(124, 0, kOnePlayer, Tag("HS01")) ||
        !is(125, 0xB, kBothPlayers, 0) || !is(126, kSay, kOnePlayer, Tag("MCJI")) ||
        !is(127, kSay, kOnePlayer, Tag("UYHC")) || !is(128, 0x12, kOnePlayer, 0) || !is(129, 0xD, kOnePlayer, 0) ||
        !is(130, 0, kOnePlayer, Tag("ZB01")) || !is(131, kZombiesGo, kOnePlayer, 0) || !is(132, kWall, kOnePlayer, 0) ||
        !is(133, kWall, kTwoPlayers, 0) || !is(134, kBlast, kOnePlayer, 0) || !is(135, kBlast, kTwoPlayers, 0) ||
        !is(136, 3, kOnePlayer, Tag("PTZW")) || !is(137, 0xB, kOnePlayer, 0) || !is(138, kSay, kOnePlayer, 0) ||
        !is(139, kSay, kOnePlayer, 0) || !is(140, kSay, kOnePlayer, 0) || !is(141, 0x12, kTwoPlayers, 0) ||
        !is(142, 0xD, kTwoPlayers, 0) || !is(143, kZombiesGo, kTwoPlayers, 0) || !is(144, 3, kTwoPlayers, 0))
        return {};
    std::vector<std::vector<uint8_t>> base(kLast + 1);
    for (uint32_t i = kFirst; i <= kLast; i++) base[i].assign(d.begin() + at(i), d.begin() + at(i) + kEventSize);
    // the final game's room out of play
    for (uint32_t i = kFirst; i <= kLast; i++) {
        uint32_t r = at(i);
        Put(d, r + kWhen, kOldCondition);
        Put(d, r + kWhenName, 0xFFFFFFFF);
        Put(d, r + kWhenPad, kNever);
        Put(d, r + kAnd, 0);
        Put(d, r + kEventName, 0xFFFFFFFF);
    }

    std::vector<uint8_t> out;
    struct Event {
        std::vector<uint8_t> e;
        Event& set(uint32_t at, uint32_t v) { return Put(e, at, v), *this; }
        Event& when(uint32_t kind, uint32_t name = 0xFFFFFFFF, uint32_t pad = 0) {
            return set(kWhen, kind).set(kWhenName, name).set(kWhenPad, pad);
        }
        Event& also(uint32_t kind, uint32_t name = 0xFFFFFFFF, uint32_t pad = 0) {
            return set(kAnd, 2).set(kWhen2, kind).set(kWhen2Name, name).set(kWhen2Pad, pad);
        }
        Event& alone() { return set(kAnd, 0); }
        Event& stop(uint32_t name) { return set(kStopper, name).set(kStopperKind, 3); }
        Event& after(float s) { return set(kDelay, FromFloat(s)); }
        Event& name(uint32_t n) { return set(kEventName, n); }
        Event& players(uint32_t p) { return set(kEventFor, p); }
    };
    auto from = [&](uint32_t i) { return Event{base[i]}; };
    // a check of its own, no action (as ZB01): the preview's waits and marks
    auto mark = [&](uint32_t players) {
        return from(130).when(kAtOnce).alone().stop(0).set(kStopperKind, 0).after(0).name(0xFFFFFFFF).players(players);
    };
    // a line of Jo-Beth's (as UYHC)
    auto line = [&](uint32_t text) { return from(127).set(kSays, text).when(kAfterLast).alone().after(0).name(0xFFFFFFFF); };
    auto add = [&](const Event& ev) { out.insert(out.end(), ev.e.begin(), ev.e.end()); };

    add(from(120).alone().players(kBothPlayers));                    // 88: the door shut as a player comes in
    add(from(121).players(kBothPlayers));                            // 89
    add(from(124));                                                  // 90 HS01: the room open (LAME)
    add(from(125));                                                  // 91
    add(line(kEww).when(kAfterEvent, Tag("HS01")).after(2.0f).name(Tag("ELSE")));             // 92 "Ewww..."
    add(mark(kOnePlayer).when(kOldCondition, 0xFFFFFFFF, kSceneOver).also(kAfterEvent, Tag("MCJI")).name(Tag("DSUU")));
    add(mark(kOnePlayer).when(kNearPad, Tag("JBTH"), 0x41F).stop(Tag("DBID")));               // 94: at the door
    add(line(kCutLines[0].text).after(0.5f).stop(Tag("DSUU")));                              // 95
    add(mark(kOnePlayer).when(kNearPad, Tag("JBTH"), 0x5B2).stop(Tag("DBID")));               // 96: at the table
    add(line(kCutLines[1].text).stop(Tag("DSUU")));                                          // 97
    add(line(kBoogers).after(2.0f).stop(Tag("DSUU")).name(Tag("UBGT")));                     // 98: the boogers
    add(from(126));                                                  // 99 MCJI: 3 s after ZB01
    add(line(Get(base[127], kSays)).stop(Tag("CHYU")).name(Tag("UYHC")));  // 100 "Oh no!"
    add(mark(kOnePlayer).when(kGroupDead, Tag("POOM")).also(kGroupDead, Tag("POMO")).name(Tag("DBID")));  // 101
    add(mark(kOnePlayer).when(kOldCondition, 0xFFFFFFFF, kSceneOver).also(kAfterEvent, Tag("UBGT")).name(Tag("SKYI")));
    add(mark(kOnePlayer).when(kAfterEvent, Tag("SKYI")).also(kNearPad, Tag("JBTH"), 0x429).name(Tag("BJKI")));  // 103
    add(mark(kOnePlayer).when(kOldCondition, 0xFFFFFFFF, kSceneOver).stop(Tag("UYHC")).name(Tag("LFIN")));    // 104
    add(line(Get(base[127], kSays)).after(1.0f).stop(Tag("ZB01")).name(Tag("CHYU")));        // 105 "Oh no!"
    add(from(128));                                                  // 106, 107: the room's other door
    add(from(129));
    add(from(130).when(kAfterEvent, Tag("BJKI")));                   // 108 ZB01: Jo-Beth at the table after the scene
    add(from(136).set(kDoes, 4).set(kSays, 0).set(kWho, 0).when(kAtOnce).after(0).name(0xFFFFFFFF));  // 109
    add(from(137).when(kAtOnce).after(3.0f));                        // 110
    add(from(131).when(kAfterLast).after(3.0f));                     // 111: the zombie behind the wall sets off
    add(from(132).when(kAfterEvent, Tag("ZB01")).after(0));          // 112: the wall, and its blast
    add(from(134).when(kAfterEvent, Tag("ZB01")).after(0));
    add(from(133).when(kAfterEvent, Tag("ZB0C")).after(0));          // 113: for two
    add(from(135).when(kAfterEvent, Tag("ZB0C")).after(0));
    add(from(136));                                                  // 114 PTZW
    add(mark(kOnePlayer).when(kAfterEvent, Tag("UYHC")).also(kAfterEvent, Tag("CHYU")));      // 115
    add(from(138).when(kAfterLast).after(2.0f));                     // 116-118
    add(from(139));
    add(from(140));
    add(from(141));                                                  // 120, 121: the other door for two
    add(from(142));
    Event zb0c = mark(kTwoPlayers);                                  // 122 ZB0C: a player at the table's side
    std::copy(base[141].begin() + kWhen, base[141].begin() + kAnd, zb0c.e.begin() + kWhen);
    add(zb0c.alone().name(Tag("ZB0C")));
    add(from(144).set(kDoes, 4).set(kSays, 0).set(kWho, 0).when(kAtOnce).after(0).name(0xFFFFFFFF));  // 123
    add(from(143).when(kAfterLast).after(3.0f));                     // 124
    add(from(144));                                                  // 125
    // condition 8's scene over (the preview's own is gone): Jo-Beth's last line said and a breath, or one of the two
    // at the table dead
    add(mark(kOnePlayer).when(kAfterEvent, Tag("UBGT")).after(4.5f).name(Tag("BGND")));
    add(mark(kBothPlayers).when(kGroupDead, Tag("POOM")).name(Tag("PDD1")));
    add(mark(kBothPlayers).when(kGroupDead, Tag("POMO")).name(Tag("PDD2")));
    return out;
}

// the setup with the scene back, empty when it is not the one this knows
std::vector<uint8_t> Restored(const std::vector<uint8_t>& file) {
    std::vector<uint8_t> d = file;
    uint32_t chars, nchars, anims, nanims, events, nevents;
    if (d.size() < 0x118 || std::memcmp(d.data(), "SETU", 4) || !Section(d, kCharacters, kCharacterSize, chars, nchars) ||
        !Section(d, kAnimations, kAnimationSize, anims, nanims) || !Section(d, kEvents, kEventSize, events, nevents))
        return {};

    // Blanche's two records, and no Dr. Pustule already
    std::vector<uint32_t> blanche;
    for (uint32_t i = 0; i < nchars; i++) {
        uint32_t r = chars + i * kCharacterSize, name = Get(d, r + kName);
        if (name == Tag("PZ02")) return {};
        if (name == Tag("PZ03") && Get(d, r + kPad) == 0x2355) blanche.push_back(r);
    }
    // her half of the scene, as the final game left it
    uint32_t hers = 0;
    for (uint32_t i = 0; i < nanims && !hers; i++) {
        uint32_t r = anims + i * kAnimationSize;
        if (Get(d, r + kOwnSequence) == Tag("SSGF") && Get(d, r + kAnimSet) == Tag("TISH") && Get(d, r + kAnim) == 0x153 &&
            Get(d, r + kPartner) == 0xFFFFFFFF)
            hers = r;
    }
    if (blanche.size() != 2 || !hers) return {};

    Put(d, hers + kPartner, Tag("POOM"));
    Put(d, hers + kMeet, 0x429);
    Put(d, hers + kFlags, 0x103200);

    // the characters, his two after them (each a copy of hers for the same players), at the end
    std::vector<uint8_t> his;
    for (uint32_t r : blanche) {
        Put(d, r + kGroup, Tag("POMO"));
        std::vector<uint8_t> one(d.begin() + r, d.begin() + r + kCharacterSize);
        Put(one, kName, Tag("PZ02"));
        Put(one, kPad, 0x2356);
        Put(one, kType, 0xD << 16 | (Get(one, kType) & 0xFFFF));
        Put(one, kGroup, Tag("POOM"));
        Put(one, kSequence, Tag("BDSI"));
        his.insert(his.end(), one.begin(), one.end());
    }
    std::vector<uint8_t> lines = TableLines(d, events, nevents);
    if (lines.empty()) LOGR("mansion: the events are not the ones known, the room left as the final game has it");
    MoveToEnd(d, kCharacters, kCharacterSize, chars, nchars, his);
    if (!lines.empty()) MoveToEnd(d, kEvents, kEventSize, events, nevents, lines);
    return d;
}
}  // namespace

namespace game {
// sub_0003D7B0's hook (video_menu.cpp): the text of a line the final game lost, 0 for any other
uint32_t MansionText(uint32_t id) {
    uint32_t at = 0;
    for (const Line& l : kCutLines) {
        if (l.text != id) {
            at += uint32_t(std::strlen(l.says) + 1);
            continue;
        }
        if (!g_texts) {
            std::string all;
            for (const Line& each : kCutLines) all.append(each.says, std::strlen(each.says) + 1);
            g_texts = mem::AllocSystem(uint32_t(all.size()));
            if (!g_texts) return 0;
            std::memcpy(kernel::HostPtr(g_texts), all.data(), all.size());
        }
        return g_texts + at;
    }
    return 0;
}

// LoadFile(name, heap, &size) has loaded a file (at, its size at size_at): Mansion of Madness's setup in Story given
// back with the pool table's scene, in memory of its own (the level's pak holds the file read in place, and no one
// frees it)
void MansionSetupLoaded(X86Context& ctx, uint32_t name, uint32_t size_at) {
    if (!ctx.eax || !Story() || !size_at || kernel::ReadCString(name, 64) != kSetup) return;
    uint32_t size = kernel::R32(size_at);
    if (!size || !mem::IsCommitted(ctx.eax) || !mem::IsCommitted(ctx.eax + size - 1)) return;
    std::vector<uint8_t> file(size);
    std::memcpy(file.data(), kernel::HostPtr(ctx.eax), size);
    std::vector<uint8_t> d = Restored(file);
    if (d.empty()) {
        LOGR("mansion: the setup is not the one known, left as it is");
        return;
    }
    // one block, reused: the mission is loaded afresh each time from the pak's copy
    static uint32_t block = 0, block_size = 0;
    if (!block || block_size < d.size()) {
        block = mem::AllocSystem(uint32_t(d.size()), 128);
        block_size = block ? uint32_t(d.size()) : 0;
    }
    if (!block) {
        LOGR("mansion: no memory for the setup, left as it is");
        return;
    }
    std::memcpy(kernel::HostPtr(block), d.data(), d.size());
    kernel::W32(size_at, uint32_t(d.size()));
    ctx.eax = block;
    LOGR("mansion: the pool table's scene restored (Dr. Pustule beside Blanche Deadwood, Jo-Beth's lines, the zombies "
         "and the wall as the preview had them)");
}
}  // namespace game

namespace {
constexpr uint32_t kRuntimes = 0x78CD90, kRuntimeCount = 0x75CCA8, kRuntimeSize = 0x48, kOver = 0x34;
constexpr uint32_t kObjective = 0x78CED0, kHeld = 0x2274C0;

bool Over(uint32_t name) {
    uint32_t runtimes = kernel::R32(kRuntimes), count = kernel::R32(kRuntimeCount);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t rt = runtimes + i * kRuntimeSize, record = kernel::R32(rt);
        if (record && kernel::R32(record + kEventName) == name) return kernel::R32(rt + kOver) != 0;
    }
    return false;
}
}  // namespace

// the events' condition callback (condition, 3 to test it): condition 8 with the scene's value held once it is over
X86_EXTERN_FUNC(__imp__sub_00235240);
X86_FUNC(sub_00235240) {
    uint32_t cond = kernel::R32(ctx.esp + 4);
    if (kernel::R32(ctx.esp + 8) == 3) {
        uint32_t rt = kernel::R32(kRuntimes) + kernel::R16(cond + 0x2A) * kRuntimeSize, record = kernel::R32(rt);
        uint32_t when = record + (kernel::R32(rt + 0xC) == kernel::R16(cond) ? kWhen : kWhen2);
        if (record && kernel::R32(when) == kOldCondition && kernel::R32(when + 8) == kNever) return kernel::Return(ctx, 0, 0);
        if (record && kernel::R32(when) == kOldCondition && kernel::R32(when + 8) == kSceneOver) {
            if (Over(Tag("BGND")) || Over(Tag("PDD1")) || Over(Tag("PDD2"))) {
                X86Context saved = ctx;
                kernel::CallGuest(ctx, kHeld, {kernel::R32(kObjective), 0});
                ctx = saved;
            }
            return kernel::Return(ctx, 0, 0);
        }
    }
    __imp__sub_00235240(ctx, base);
}

// VoiceOf(text): the sound event a text is said with (-1 none); the lost lines' own
X86_EXTERN_FUNC(__imp__sub_000461C0);
X86_FUNC(sub_000461C0) {
    uint32_t id = kernel::R32(ctx.esp + 4);
    for (const Line& l : kCutLines)
        if (l.text == id) return kernel::Return(ctx, l.voice, 0);
    __imp__sub_000461C0(ctx, base);
}
