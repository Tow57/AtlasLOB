#include <cstddef>
#include <cstdint>
#include <span>

#include "command_log_codec.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> input{data, size};
  if (input.size() >= atlaslob::persistence::detail::command_log_record_length_prefix_bytes) {
    static_cast<void>(atlaslob::persistence::detail::inspect_log_record_length(
        input.first(atlaslob::persistence::detail::command_log_record_length_prefix_bytes)));
  } else {
    static_cast<void>(atlaslob::persistence::detail::inspect_log_record_length(input));
  }
  static_cast<void>(atlaslob::persistence::detail::decode_command_record(input));
  return 0;
}
