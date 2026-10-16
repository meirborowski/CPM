#include "core/sha256.hpp"
#include "support/check.hpp"

using cpm::Sha256;

TEST(nist_vectors) {
    CHECK_EQ(Sha256::hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(Sha256::hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(million_a_incremental) {
    Sha256 h;
    std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(chunk);
    CHECK_EQ(Sha256::hex(h.finish()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(block_boundaries) {
    // Lengths around the 55/56/64-byte padding boundaries, hashed whole and byte by byte.
    for (std::size_t len : {55u, 56u, 63u, 64u, 65u, 119u, 120u}) {
        std::string data(len, 'x');
        Sha256 h;
        for (char c : data) h.update(std::string_view(&c, 1));
        CHECK_EQ(Sha256::hex(h.finish()), Sha256::hex(data));
    }
}
