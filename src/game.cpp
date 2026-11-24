// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#include "game.h"

#include <cstddef>
#include <vector>

#include "common/log.h"
#include "layout.h"
#include "pattern.h"

namespace game {
namespace {

struct Signature {
    const char* name;
    const char* text;
    // Module offset of the match on 1.0.1491.50.
    std::uintptr_t expected_rva;
};

// The prologue plus the first instructions after it. The 24-byte prologue alone is
// already unique in the 1.0.1491.50 image.
constexpr Signature kZoom{
    "zoom function",
    "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 30 0F 29 74 24 20 4D "
    "8B F1 49 8B F8 48 8B DA 48 8B F1 4D",
    0x3C6168};

constexpr Signature kUpdate{
    "letterbox update", "48 8B C4 48 89 58 08 57 48 83 EC 70 0F 29 70 E8 33 FF 40 38", 0x3200E8};

// `80 3D <disp32> 00` at +16 compares the drawn enable byte with zero.
constexpr Signature kDraw{
    "letterbox drawing", "48 8B C4 48 89 58 10 48 89 70 18 57 48 83 EC 50 80 3D ? ? ? ? 00 0F",
    0x3193CC};
constexpr std::size_t kDrawEnableCompare = 16;
constexpr std::size_t kCompareLength = 7;

// `mov byte ptr [rip + disp32], 0FFh`: the alpha store at the end of the update.
constexpr Signature kAlphaStore{"alpha store", "C6 05 ? ? ? ? FF 0F 28 74 24 60", 0x320545};
constexpr std::size_t kAlphaStoreLength = 7;

struct Match {
    std::uintptr_t address = 0;
    bool missing = false;
    bool ambiguous = false;
};

Match find(const std::vector<mem::Region>& regions, const mem::Region& module, const Signature& sig,
           bool verbose) {
    Match result;
    const auto parsed = pattern::parse(sig.text);
    if (!parsed) {
        logger::info("pattern {}: malformed signature", sig.name);
        result.ambiguous = true;
        return result;
    }

    const std::vector<std::uintptr_t> hits = pattern::find_all(regions, *parsed);
    if (hits.size() == 1) {
        result.address = hits.front();
        if (verbose) {
            const std::uintptr_t rva = hits.front() - module.base;
            logger::info("pattern {}: one match at +0x{:X}{}", sig.name, rva,
                         rva == sig.expected_rva ? "" : " (differs from the expected offset)");
        }
    } else if (hits.empty()) {
        result.missing = true;
        logger::info("pattern {}: no match", sig.name);
    } else {
        result.ambiguous = true;
        logger::info("pattern {}: {} matches, need exactly one", sig.name, hits.size());
        for (const std::uintptr_t hit : hits) {
            logger::info("pattern {}:   +0x{:X}", sig.name, hit - module.base);
        }
    }
    return result;
}

std::uintptr_t rip_target(std::uintptr_t instruction, std::size_t disp_offset, std::size_t length) {
    const auto disp = load<std::int32_t>(instruction + disp_offset);
    return instruction + length + static_cast<std::uintptr_t>(static_cast<std::int64_t>(disp));
}

}  // namespace

Resolve resolve(const mem::Region& module, Addresses& out, bool verbose) {
    std::vector<mem::Region> regions;
    for (const mem::Section& section : mem::executable_sections(module)) {
        regions.push_back(section.region);
    }

    const Match zoom = find(regions, module, kZoom, verbose);
    const Match update = find(regions, module, kUpdate, verbose);
    const Match draw = find(regions, module, kDraw, verbose);
    const Match store = find(regions, module, kAlphaStore, verbose);

    if (zoom.ambiguous || update.ambiguous || draw.ambiguous || store.ambiguous) {
        return Resolve::Failed;
    }
    if (zoom.missing || update.missing || draw.missing || store.missing) {
        if (store.missing) {
            logger::info("pattern {}: another plugin may have patched the alpha immediate", kAlphaStore.name);
        }
        return Resolve::Missing;
    }

    const std::uintptr_t anchor = rip_target(store.address, 2, kAlphaStoreLength);
    const std::uintptr_t drawn_enable = rip_target(draw.address + kDrawEnableCompare, 2, kCompareLength);
    logger::info("letterbox state: alpha byte at +0x{:X}, drawn enable byte read by the drawing at +0x{:X}",
                 anchor - module.base, drawn_enable - module.base);
    if (drawn_enable != anchor + letterbox::kDrawnEnable) {
        logger::info("the drawing reads its enable byte at anchor {:+#x}, expected {:+#x}; the layout "
                     "in layout.h does not fit this build",
                     static_cast<std::int64_t>(drawn_enable - anchor),
                     static_cast<std::int64_t>(letterbox::kDrawnEnable));
        return Resolve::Failed;
    }

    out.zoom = zoom.address;
    out.update = update.address;
    out.draw = draw.address;
    out.alpha_store = store.address;
    out.anchor = anchor;
    return Resolve::Found;
}

}  // namespace game
