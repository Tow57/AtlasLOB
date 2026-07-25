#include "platform_cli.hpp"

#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
// clang-format off: MinGW's shellapi.h requires Windows types declared by windows.h.
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

namespace atlaslob::persistence::detail {
namespace {

#if defined(_WIN32)

[[noreturn]] void throw_last_windows_error() {
  throw std::system_error{static_cast<int>(::GetLastError()), std::system_category()};
}

[[nodiscard]] int checked_windows_length(std::size_t size) {
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error{"command-line value exceeds the Windows representation"};
  }
  return static_cast<int>(size);
}

[[nodiscard]] std::string utf8_from_wide(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  const int input_size = checked_windows_length(value.size());
  const int output_size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                                input_size, nullptr, 0, nullptr, nullptr);
  if (output_size == 0) {
    throw_last_windows_error();
  }

  std::string result(static_cast<std::size_t>(output_size), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, result.data(),
                            output_size, nullptr, nullptr) != output_size) {
    throw_last_windows_error();
  }
  return result;
}

[[nodiscard]] std::wstring wide_from_utf8(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int input_size = checked_windows_length(value.size());
  const int output_size =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
  if (output_size == 0) {
    throw_last_windows_error();
  }

  std::wstring result(static_cast<std::size_t>(output_size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, result.data(),
                            output_size) != output_size) {
    throw_last_windows_error();
  }
  return result;
}

struct LocalFreeDeleter final {
  void operator()(wchar_t** value) const noexcept {
    if (value != nullptr) {
      static_cast<void>(::LocalFree(value));
    }
  }
};

#endif

}  // namespace

NativeCommandLineArguments::NativeCommandLineArguments(std::vector<std::string> values)
    : values_{std::move(values)} {
  arguments_.reserve(values_.size());
  for (const auto& value : values_) {
    arguments_.push_back(value);
  }
}

std::span<const std::string_view> NativeCommandLineArguments::arguments() const noexcept {
  return arguments_;
}

NativeCommandLineArguments native_command_line_arguments(int argc, char* const argv[]) {
  std::vector<std::string> values;
#if defined(_WIN32)
  static_cast<void>(argc);
  static_cast<void>(argv);

  int native_count{};
  std::unique_ptr<wchar_t*, LocalFreeDeleter> native_arguments{
      ::CommandLineToArgvW(::GetCommandLineW(), &native_count)};
  if (native_arguments == nullptr) {
    throw_last_windows_error();
  }
  if (native_count <= 0) {
    throw std::runtime_error{"Windows returned no command-line arguments"};
  }
  values.reserve(static_cast<std::size_t>(native_count));
  for (int index = 0; index < native_count; ++index) {
    if (native_arguments.get()[index] == nullptr) {
      throw std::runtime_error{"Windows returned a null command-line argument"};
    }
    values.push_back(utf8_from_wide(native_arguments.get()[index]));
  }
#else
  if (argc < 0 || (argc != 0 && argv == nullptr)) {
    throw std::invalid_argument{"invalid native command line"};
  }
  values.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    if (argv[index] == nullptr) {
      throw std::invalid_argument{"null native command-line argument"};
    }
    values.emplace_back(argv[index]);
  }
#endif
  return NativeCommandLineArguments{std::move(values)};
}

bool configure_binary_standard_streams() noexcept {
#if defined(_WIN32)
  if (::_setmode(::_fileno(stdout), _O_BINARY) == -1) {
    return false;
  }
  if (::_setmode(::_fileno(stderr), _O_BINARY) == -1) {
    return false;
  }
#endif
  return true;
}

std::filesystem::path path_from_utf8(std::string_view value) {
#if defined(_WIN32)
  return std::filesystem::path{wide_from_utf8(value)};
#else
  return std::filesystem::path{value};
#endif
}

}  // namespace atlaslob::persistence::detail
