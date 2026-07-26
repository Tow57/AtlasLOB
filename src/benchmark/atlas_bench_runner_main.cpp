#include "benchmark_runner.hpp"

int main(int argc, char** argv) {
  return atlaslob::benchmark::run_native_benchmark_cli(argc, argv,
                                                       atlaslob::benchmark::RunnerFlavor::timed);
}
