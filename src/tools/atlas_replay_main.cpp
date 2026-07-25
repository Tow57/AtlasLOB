#include <iostream>

#include "persistence_cli.hpp"
#include "platform_cli.hpp"

int main(int argc, char* argv[]) {
  try {
    if (!atlaslob::persistence::detail::configure_binary_standard_streams()) {
      return atlaslob::persistence::detail::cli_io_failure_exit_code;
    }
    const auto arguments = atlaslob::persistence::detail::native_command_line_arguments(argc, argv);
    return atlaslob::persistence::detail::run_atlas_replay(arguments.arguments(), std::cout,
                                                           std::cerr);
  } catch (...) {
    return atlaslob::persistence::detail::cli_io_failure_exit_code;
  }
}
