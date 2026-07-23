# Third-Party Notices

AtlasLOB's production library currently depends only on the C++ standard library. When tests are
enabled, CMake fetches the following pinned test-only dependency.

## GoogleTest

- Project: GoogleTest
- Version: 1.17.0
- Commit: `52eb8108c5bdec04579160ae17225d66034bd723`
- Source: <https://github.com/google/googletest>
- License: BSD 3-Clause

GoogleTest is used only by AtlasLOB's test targets and is not linked into the production libraries
or command-line executable.
