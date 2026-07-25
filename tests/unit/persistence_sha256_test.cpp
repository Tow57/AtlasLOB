#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "sha256.hpp"

namespace atlaslob::persistence::tests {
namespace {

std::span<const std::uint8_t> bytes(std::string_view value) {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

TEST(PersistenceSha256, MatchesPublishedEmptyAndAbcVectors) {
  EXPECT_EQ(utility::sha256({}).hex(),
            "e3b0c44298fc1c149afbf4c8996fb924"
            "27ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(utility::sha256(bytes("abc")).hex(),
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad");
}

TEST(PersistenceSha256, StreamingAndOneShotInputsAreEquivalent) {
  utility::Sha256 streaming;
  streaming.update(bytes("Atlas"));
  streaming.update(bytes("LOB"));

  EXPECT_EQ(streaming.finish(), utility::sha256(bytes("AtlasLOB")));
}

}  // namespace
}  // namespace atlaslob::persistence::tests
