#include "core/archive.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <map>

#include "core/error.hpp"
#include "core/strings.hpp"

namespace cpm::archive {

namespace {

// ---------------------------------------------------------------------------------------------
// tar

constexpr std::size_t kBlock = 512;

// `value` in octal, zero-padded to width - 1 digits and NUL-terminated, as ustar headers store numbers.
void put_octal(std::string& header, std::size_t offset, std::size_t width, std::uint64_t value) {
    std::string digits = std::format("{:0{}o}", value, width - 1);
    if (digits.size() > width - 1) throw Error(std::format("tar: value {} does not fit the header", value));
    std::copy(digits.begin(), digits.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
}

void put_text(std::string& header, std::size_t offset, std::string_view text) {
    std::copy(text.begin(), text.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::string make_header(std::string_view name, std::string_view prefix, char type, std::uint64_t size, unsigned mode) {
    std::string h(kBlock, '\0');
    put_text(h, 0, name);
    put_octal(h, 100, 8, mode);
    put_octal(h, 108, 8, 0);     // uid
    put_octal(h, 116, 8, 0);     // gid
    put_octal(h, 124, 12, size);
    put_octal(h, 136, 12, 0);    // mtime
    put_text(h, 148, "        "); // checksum, counted as spaces
    h[156] = type;
    put_text(h, 257, std::string_view("ustar\0" "00", 8));
    put_octal(h, 329, 8, 0);     // devmajor
    put_octal(h, 337, 8, 0);     // devminor
    put_text(h, 345, prefix);
    unsigned sum = 0;
    for (char c : h) sum += static_cast<unsigned char>(c);
    put_octal(h, 148, 7, sum);   // six digits, then a NUL and a space
    h[154] = '\0';
    h[155] = ' ';
    return h;
}

void pad(std::string& out) {
    if (std::size_t rem = out.size() % kBlock) out.append(kBlock - rem, '\0');
}

// Splits `path` into a ustar prefix and name (prefix + "/" + name), or returns false if it cannot fit.
bool split_ustar(const std::string& path, std::string& prefix, std::string& name) {
    if (path.size() <= 100) {
        prefix.clear();
        name = path;
        return true;
    }
    for (std::size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        if (slash <= 155 && path.size() - slash - 1 <= 100 && slash + 1 < path.size()) {
            prefix = path.substr(0, slash);
            name = path.substr(slash + 1);
            return true;
        }
    }
    return false;
}

// One pax record, "<length> path=<value>\n", where <length> counts the whole record including itself.
std::string pax_record(std::string_view key, std::string_view value) {
    std::string body = std::format(" {}={}\n", key, value);
    std::size_t length = body.size() + 1;
    while (std::to_string(length).size() + body.size() != length) ++length;
    return std::to_string(length) + body;
}

void write_member(std::string& out, const std::string& path, const Entry& e) {
    std::string member = e.directory ? path + "/" : path;
    std::string prefix, name;
    if (!split_ustar(member, prefix, name)) {
        std::string records = pax_record("path", member);
        out += make_header("PaxHeader", "", 'x', records.size(), 0644);
        out += records;
        pad(out);
        prefix.clear();
        name = member.substr(0, 100);
    }
    out += make_header(name, prefix, e.directory ? '5' : '0', e.directory ? 0 : e.data.size(), e.directory ? 0755 : 0644);
    if (!e.directory) {
        out += e.data;
        pad(out);
    }
}

void check_path(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos || path.find(':') != std::string::npos)
        throw Error(std::format("tar: invalid member path '{}'", path));
    for (const auto& segment : str::split(path, '/')) {
        if (segment.empty() || segment == "." || segment == "..")
            throw Error(std::format("tar: invalid member path '{}'", path));
    }
}

// ---------------------------------------------------------------------------------------------
// DEFLATE with fixed Huffman codes (RFC 1951 §3.2.6)

class BitWriter {
public:
    // `count` bits of `bits`, least significant first.
    void put(std::uint32_t bits, int count) {
        acc_ |= static_cast<std::uint64_t>(bits) << n_;
        n_ += count;
        while (n_ >= 8) {
            out_.push_back(static_cast<char>(acc_ & 0xFF));
            acc_ >>= 8;
            n_ -= 8;
        }
    }
    // Huffman codes go out most significant bit first.
    void put_code(std::uint32_t code, int length) {
        std::uint32_t reversed = 0;
        for (int i = 0; i < length; ++i) {
            reversed = (reversed << 1) | (code & 1);
            code >>= 1;
        }
        put(reversed, length);
    }
    std::string finish() {
        if (n_ > 0) out_.push_back(static_cast<char>(acc_ & 0xFF));
        acc_ = 0;
        n_ = 0;
        return std::move(out_);
    }

private:
    std::string out_;
    std::uint64_t acc_ = 0;
    int n_ = 0;
};

void put_symbol(BitWriter& w, int symbol) {
    if (symbol < 144) w.put_code(0x30 + symbol, 8);
    else if (symbol < 256) w.put_code(0x190 + (symbol - 144), 9);
    else if (symbol < 280) w.put_code(symbol - 256, 7);
    else w.put_code(0xC0 + (symbol - 280), 8);
}

constexpr std::array<int, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<int, 30> kDistanceBase = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                               33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                               1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr std::array<int, 30> kDistanceExtra = {0, 0, 0, 0, 1, 1, 2, 2, 3,  3,  4,  4,  5,  5,  6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void put_match(BitWriter& w, int length, int distance) {
    int li = static_cast<int>(kLengthBase.size()) - 1;
    while (kLengthBase[li] > length) --li;
    put_symbol(w, 257 + li);
    if (kLengthExtra[li]) w.put(static_cast<std::uint32_t>(length - kLengthBase[li]), kLengthExtra[li]);
    int di = static_cast<int>(kDistanceBase.size()) - 1;
    while (kDistanceBase[di] > distance) --di;
    w.put_code(static_cast<std::uint32_t>(di), 5);
    if (kDistanceExtra[di]) w.put(static_cast<std::uint32_t>(distance - kDistanceBase[di]), kDistanceExtra[di]);
}

std::string deflate(std::string_view in) {
    constexpr int kWindow = 32768;
    constexpr int kMinMatch = 3;
    constexpr int kMaxMatch = 258;
    constexpr int kHashBits = 15;
    constexpr int kMaxChain = 128;
    if (in.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw Error("gzip: input larger than 2 GB");
    const int n = static_cast<int>(in.size());
    const auto* bytes = reinterpret_cast<const unsigned char*>(in.data());

    // Hash chains over the last 32 KB: head[h] is the latest position with hash h, prev[pos % window] the one
    // before it.
    std::vector<int> head(std::size_t{1} << kHashBits, -1);
    std::vector<int> prev(kWindow, -1);
    auto hash = [&](int i) {
        std::uint32_t v = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    auto insert = [&](int i) {
        if (i + kMinMatch > n) return;
        std::uint32_t h = hash(i);
        prev[static_cast<std::size_t>(i % kWindow)] = head[h];
        head[h] = i;
    };

    BitWriter w;
    w.put(1, 1);  // BFINAL: this is the only block
    w.put(1, 2);  // BTYPE 01: fixed Huffman codes
    int i = 0;
    while (i < n) {
        int best_length = 0;
        int best_distance = 0;
        if (i + kMinMatch <= n) {
            const int max_length = std::min(kMaxMatch, n - i);
            int candidate = head[hash(i)];
            for (int chain = kMaxChain; candidate >= 0 && i - candidate <= kWindow && chain > 0; --chain) {
                if (bytes[candidate + best_length] == bytes[i + best_length]) {
                    int length = 0;
                    while (length < max_length && bytes[candidate + length] == bytes[i + length]) ++length;
                    if (length > best_length) {
                        best_length = length;
                        best_distance = i - candidate;
                        if (length == max_length) break;
                    }
                }
                int next = prev[static_cast<std::size_t>(candidate % kWindow)];
                if (next >= candidate) break;  // the slot was reused by a newer position
                candidate = next;
            }
        }
        if (best_length >= kMinMatch) {
            put_match(w, best_length, best_distance);
            for (int k = 0; k < best_length; ++k) insert(i + k);
            i += best_length;
        } else {
            put_symbol(w, bytes[i]);
            insert(i);
            ++i;
        }
    }
    put_symbol(w, 256);  // end of block
    return w.finish();
}

void put_le32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

}  // namespace

std::string tar(std::vector<Entry> entries) {
    std::map<std::string, Entry> members;  // keyed by path, with directories ending in '/' so they sort first
    auto key = [](const std::string& path, bool directory) { return directory ? path + "/" : path; };
    for (auto& e : entries) {
        check_path(e.path);
        if (members.contains(key(e.path, !e.directory)))
            throw Error(std::format("tar: '{}' is both a file and a directory", e.path));
        if (!e.directory && members.contains(key(e.path, false))) throw Error(std::format("tar: '{}' is given twice", e.path));
        // Add the parents too, so the archive creates every directory with a known mode.
        for (std::size_t slash = e.path.find('/'); slash != std::string::npos; slash = e.path.find('/', slash + 1)) {
            std::string parent = e.path.substr(0, slash);
            if (members.contains(key(parent, false))) throw Error(std::format("tar: '{}' is both a file and a directory", parent));
            members.try_emplace(key(parent, true), Entry{.path = parent, .data = {}, .directory = true});
        }
        std::string k = key(e.path, e.directory);
        members.insert_or_assign(k, std::move(e));  // a directory given explicitly may already be there as a parent
    }
    std::string out;
    for (const auto& [k, e] : members) write_member(out, e.path, e);
    out.append(2 * kBlock, '\0');  // end of archive
    return out;
}

std::uint32_t crc32(std::string_view data) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (char ch : data) crc = table[(crc ^ static_cast<unsigned char>(ch)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::string gzip(std::string_view data) {
    // ID1 ID2, CM 8 (deflate), FLG 0, MTIME 0, XFL 0, OS 255 (unknown)
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    out += deflate(data);
    put_le32(out, crc32(data));
    put_le32(out, static_cast<std::uint32_t>(data.size() & 0xFFFFFFFFu));
    return out;
}

}  // namespace cpm::archive
