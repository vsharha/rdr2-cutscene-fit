// Copyright (c) 2026 Viktor Sharha

#include "framelog.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <vector>

#include "hook.h"
#include "ring.h"

namespace zoom::framelog {

namespace {

// A record with this flag set is a camera cut. 0x40 is set with it.
constexpr std::uint16_t kCutFlag = 0x80;
// Records before and after a cut that get the full set.
constexpr std::size_t kContext = 3;

std::uint64_t qword(const std::uint8_t* bytes, std::size_t index) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes + index * sizeof(value), sizeof(value));
    return value;
}

template <std::size_t N>
void append_qwords(std::string& out, const char* name, const std::array<std::uint8_t, N>& bytes,
                   bool valid, std::uint64_t pointer) {
    out += name;
    out += ':';
    if (pointer == 0) {
        out += " null\n";
    } else if (!valid) {
        out += " unreadable\n";
    } else {
        for (std::size_t i = 0; i < N / 8; ++i) {
            out += std::format(" {:016X}", qword(bytes.data(), i));
        }
        out += '\n';
    }
}

void append_header(std::string& out, std::size_t dump_number, std::size_t records, std::uint64_t oldest,
                   std::uint64_t newest, std::uint64_t total, double frequency,
                   const std::string& version) {
    out += std::format("# zoom-frames dump {}\n", dump_number);
    out += std::format("# game version {}, hook {}, mode {}\n", version,
                       zoom::hook::installed() ? "installed" : "removed",
                       zoom::mode_name(zoom::hook::mode()));
    out += std::format("# {} records, frame counters {}..{} of {} calls, qpc frequency {:.0f} Hz\n",
                       records, oldest, newest, total, frequency);
    out += "# R line (one per record), key=value separated by spaces:\n";
    out += "#   idx      position in this dump, 0 = oldest\n";
    out += "#   frame    call counter of the hook, counted from 0 since the hook was set up\n";
    out += "#   t_ms     time relative to the newest record (<= 0); dt_ms is the time since the record before\n";
    out += "#   full     1 if the byte lines below are complete, 0 if only changed frame qwords are listed\n";
    out += std::format("#   cut      1 if flags & 0x{:X}\n", kCutFlag);
    out += "#   p1, fp, p3, owner  the pointer arguments of the call (fp is the camera frame)\n";
    out += "#   weight   letterbox weight, float at anchor - 8\n";
    out += "#   enable   drawn enable byte at anchor + 0x21\n";
    out += "#   flags    u16 at frame + 0xCC after the call\n";
    out += "#   fov_in, fov_set, fov_out  frame + 0x60 before this tool's change, as set by it, after the call\n";
    out += "#   p3vt, ownervt  first qword at p3 and at owner (the vtable of the object) in hex, or null / unreadable\n";
    out += "#   mode     mode of this tool when the call ran\n";
    out += "#   valid    bit 0 frame bytes, 1 p1, 2 p3, 3 owner: set if copied completely\n";
    out += "# Lines after an R line, all values are little-endian 64-bit words in hex:\n";
    out += "#   frame:   (full) 32 qwords, frame + 0x00..0xFF, copied before the call\n";
    out += "#   p1: p3: owner:   (full) 8 qwords from the pointer, or null / unreadable\n";
    out += "#   fdiff:   (not full) offset=qword for each frame qword that differs from the previous\n";
    out += "#            record in this dump, offset in hex; \"none\" if nothing differs\n";
    out += "#   p3diff:  (not full, only if p3 is the same pointer as in the previous record and some of\n";
    out += "#            its 8 recorded qwords differ) offset=qword for each qword that differs\n";
    out += "# A record is full if it is the first, within 3 records of a cut, or within 3 records of a\n";
    out += "# record whose p3 differs from the record before.\n";
}

template <std::size_t N>
std::string first_qword(const std::array<std::uint8_t, N>& bytes, bool valid, std::uint64_t pointer) {
    if (pointer == 0) {
        return "null";
    }
    return valid ? std::format("0x{:X}", qword(bytes.data(), 0)) : "unreadable";
}

}  // namespace

Result write(const std::filesystem::path& file, int frames, double qpc_frequency,
             const std::string& game_version) {
    Result result;
    const std::uint64_t count = zoom::ring::count();
    const std::uint64_t wanted = static_cast<std::uint64_t>(frames);
    const std::uint64_t first = count > wanted ? count - wanted : 0;

    std::vector<Record> records;
    records.reserve(static_cast<std::size_t>(count - first));
    std::vector<std::uint64_t> skipped;
    for (std::uint64_t frame = first; frame < count; ++frame) {
        Record r;
        if (zoom::ring::read(frame, r)) {
            records.push_back(r);
        } else {
            skipped.push_back(frame);
        }
    }
    result.records = records.size();
    result.skipped = skipped.size();

    std::vector<bool> full(records.size(), false);
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (i == 0) {
            full[i] = true;
        }
        const bool p3_changed = i > 0 && records[i].p3 != records[i - 1].p3;
        if ((records[i].flags & kCutFlag) != 0 || p3_changed) {
            const std::size_t low = i > kContext ? i - kContext : 0;
            const std::size_t high = std::min(records.size() - 1, i + kContext);
            for (std::size_t j = low; j <= high; ++j) {
                full[j] = true;
            }
        }
    }

    static std::size_t dump_number = 0;
    ++dump_number;

    std::string out;
    out.reserve(records.size() * 400 + 4096);
    append_header(out, dump_number, records.size(), records.empty() ? 0 : records.front().frame,
                  records.empty() ? 0 : records.back().frame, count, qpc_frequency, game_version);
    for (const std::uint64_t frame : skipped) {
        out += std::format("# skipped frame {} (overwritten or being written)\n", frame);
    }

    const std::uint64_t newest_qpc = records.empty() ? 0 : records.back().qpc;
    const auto ms = [qpc_frequency](std::uint64_t later, std::uint64_t earlier) {
        const double delta = later >= earlier ? static_cast<double>(later - earlier)
                                              : -static_cast<double>(earlier - later);
        return delta * 1000.0 / qpc_frequency;
    };

    for (std::size_t i = 0; i < records.size(); ++i) {
        const Record& r = records[i];
        const bool cut = (r.flags & kCutFlag) != 0;
        const std::uint64_t before = i == 0 ? r.qpc : records[i - 1].qpc;
        out += std::format(
            "R idx={} frame={} t_ms={:.3f} dt_ms={:.3f} full={} cut={} p1=0x{:X} fp=0x{:X} p3=0x{:X} "
            "owner=0x{:X} p3vt={} ownervt={} weight={:.6f} enable={} flags=0x{:04X} fov_in={:.4f} fov_set={:.4f} "
            "fov_out={:.4f} mode={} valid=0x{:X}\n",
            i, r.frame, ms(r.qpc, newest_qpc), ms(r.qpc, before), full[i] ? 1 : 0, cut ? 1 : 0, r.p1,
            r.frame_pointer, r.p3, r.owner,
            first_qword(r.p3_bytes, (r.valid & kValidP3) != 0, r.p3),
            first_qword(r.owner_bytes, (r.valid & kValidOwner) != 0, r.owner), r.weight, static_cast<int>(r.letterbox), r.flags, r.input,
            r.corrected, r.output, zoom::mode_name(static_cast<Mode>(r.mode)), r.valid);

        const bool have_frame = (r.valid & kValidFrame) != 0;
        if (full[i]) {
            ++result.full_records;
            append_qwords(out, "frame", r.frame_bytes, have_frame, r.frame_pointer);
            append_qwords(out, "p1", r.p1_bytes, (r.valid & kValidP1) != 0, r.p1);
            append_qwords(out, "p3", r.p3_bytes, (r.valid & kValidP3) != 0, r.p3);
            append_qwords(out, "owner", r.owner_bytes, (r.valid & kValidOwner) != 0, r.owner);
        } else if (!have_frame) {
            out += "fdiff: unreadable\n";
        } else {
            out += "fdiff:";
            bool any = false;
            const Record& p = records[i - 1];
            for (std::size_t q = 0; q < kFrameBytes / 8; ++q) {
                const std::uint64_t now = qword(r.frame_bytes.data(), q);
                if ((p.valid & kValidFrame) == 0 || now != qword(p.frame_bytes.data(), q)) {
                    out += std::format(" {:X}={:016X}", q * 8, now);
                    any = true;
                }
            }
            out += any ? "\n" : " none\n";

            if (r.p3 != 0 && r.p3 == p.p3 && (r.valid & kValidP3) != 0 && (p.valid & kValidP3) != 0) {
                std::string changed;
                for (std::size_t q = 0; q < kAuxBytes / 8; ++q) {
                    const std::uint64_t now = qword(r.p3_bytes.data(), q);
                    if (now != qword(p.p3_bytes.data(), q)) {
                        changed += std::format(" {:X}={:016X}", q * 8, now);
                    }
                }
                if (!changed.empty()) {
                    out += "p3diff:" + changed + "\n";
                }
            }
        }
    }
    out += "# end of dump\n";

    std::ofstream stream(file, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream.is_open()) {
        return result;
    }
    stream.write(out.data(), static_cast<std::streamsize>(out.size()));
    stream.flush();
    result.written = stream.good();
    result.bytes = out.size();
    return result;
}

}  // namespace zoom::framelog
