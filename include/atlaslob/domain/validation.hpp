#pragma once

#include "atlaslob/domain/commands.hpp"

namespace atlaslob::domain {

struct ValidationResult final {
  RejectReason reason{RejectReason::none};

  [[nodiscard]] constexpr bool accepted() const noexcept { return reason == RejectReason::none; }
};

[[nodiscard]] ValidationResult validate(const NewOrder& order) noexcept;

}  // namespace atlaslob::domain
