#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlaslob::persistence::detail {

class NativeCommandLineArguments final {
 public:
  explicit NativeCommandLineArguments(std::vector<std::string> values);

  NativeCommandLineArguments(const NativeCommandLineArguments&) = delete;
  NativeCommandLineArguments& operator=(const NativeCommandLineArguments&) = delete;
  NativeCommandLineArguments(NativeCommandLineArguments&&) = delete;
  NativeCommandLineArguments& operator=(NativeCommandLineArguments&&) = delete;

  [[nodiscard]] std::span<const std::string_view> arguments() const noexcept;

 private:
  std::vector<std::string> values_;
  std::vector<std::string_view> arguments_;
};

// Returns every native process argument as owned UTF-8. On Windows this reads
// the wide process command line instead of the lossy narrow argv projection.
[[nodiscard]] NativeCommandLineArguments native_command_line_arguments(int argc,
                                                                       char* const argv[]);

// Prevents the Windows CRT from translating canonical LF bytes to CRLF.
[[nodiscard]] bool configure_binary_standard_streams() noexcept;

// Converts one UTF-8 CLI argument into the platform-native path representation.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value);

}  // namespace atlaslob::persistence::detail
