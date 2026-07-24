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

## Python development and evidence tools

The Python correctness package has no runtime dependency outside the Python standard library. Its
isolated build uses setuptools 83.0.0 under the MIT license. Its optional, exactly pinned
development/test group installs:

| Project | Version | Source | License |
| --- | --- | --- | --- |
| pytest | 9.1.1 | <https://github.com/pytest-dev/pytest> | MIT |
| Hypothesis | 6.160.0 | <https://github.com/HypothesisWorks/hypothesis> | MPL-2.0 |
| Ruff | 0.15.22 | <https://github.com/astral-sh/ruff> | MIT |
| mypy | 2.3.0 | <https://github.com/python/mypy> | MIT |

These tools are used for test generation, unit execution, formatting/linting, and static typing.
They are not imported by the installed `atlaslob` runtime package.
