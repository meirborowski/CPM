#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace cpm {

// Incremental SHA-256 (FIPS 180-4).
class Sha256 {
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256();
    void update(std::string_view data);
    Digest finish();  // the object must not be used afterwards

    static Digest hash(std::string_view data);
    static std::string hex(const Digest& digest);  // 64 lowercase hex characters
    static std::string hex(std::string_view data) { return hex(hash(data)); }

private:
    void block(const std::uint8_t* p);

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;  // bytes
};

}  // namespace cpm
