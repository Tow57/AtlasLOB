#include "allocation_tracker.hpp"
#include "benchmark_runner.hpp"

int main(int argc, char** argv) {
  return atlaslob::benchmark::run_native_benchmark_cli(
      argc, argv, atlaslob::benchmark::RunnerFlavor::allocation,
      atlaslob::benchmark::AllocationHooks{
          .begin = &atlaslob::benchmark::begin_allocation_tracking,
          .end = &atlaslob::benchmark::end_allocation_tracking,
      });
}
