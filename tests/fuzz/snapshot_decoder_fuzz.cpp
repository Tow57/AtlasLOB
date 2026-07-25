#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "snapshot_codec.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> input{data, size};
  if (input.size() >= atlaslob::persistence::detail::snapshot_fixed_prefix_bytes) {
    static_cast<void>(atlaslob::persistence::detail::inspect_snapshot_length(
        input.first(atlaslob::persistence::detail::snapshot_fixed_prefix_bytes)));
  } else {
    static_cast<void>(atlaslob::persistence::detail::inspect_snapshot_length(input));
  }
  const auto decoded = atlaslob::persistence::detail::decode_snapshot(input);
  if (decoded) {
    const auto encoded = atlaslob::persistence::detail::encode_snapshot(*decoded.value);
    if (!encoded || encoded.value->size() != input.size() ||
        !std::equal(encoded.value->begin(), encoded.value->end(), input.begin())) {
      std::abort();
    }
  }
  return 0;
}
