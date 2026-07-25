#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "platform_cli.hpp"

namespace atlaslob::persistence::tests {
namespace {

[[nodiscard]] std::string utf8_bytes(std::u8string_view value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

TEST(PersistencePlatformCli, Utf8PathsRoundTripWithoutLosingUnicode) {
  constexpr std::u8string_view name{u8"路径-🧪-日志.log"};

  const auto path = detail::path_from_utf8(utf8_bytes(name));

  EXPECT_EQ(utf8_bytes(path.filename().u8string()), utf8_bytes(name));
}

}  // namespace
}  // namespace atlaslob::persistence::tests
