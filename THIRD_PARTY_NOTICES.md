# Third-Party Notices

AtlasLOB's C++ production libraries depend only on the C++ standard library. The optional native
Python extension uses the pinned build dependencies listed below. When tests are enabled, CMake
fetches the following pinned test-only dependency.

## GoogleTest

- Project: GoogleTest
- Version: 1.17.0
- Commit: `52eb8108c5bdec04579160ae17225d66034bd723`
- Source: <https://github.com/google/googletest>
- License: BSD 3-Clause

GoogleTest is used only by AtlasLOB's test targets and is not linked into the production libraries
or command-line executable.

## Google Benchmark

- Project: Google Benchmark
- Version: 1.9.5
- Commit: `192ef10025eb2c4cdd392bc502f0c852196baa48`
- Source: <https://github.com/google/benchmark>
- License: Apache-2.0

Google Benchmark is fetched only when `ATLAS_BUILD_BENCHMARKS=ON`. It is linked exclusively into
opt-in measurement executables and is not part of AtlasLOB's production libraries, command-line
tools, Python extension, or wheel artifacts.

## Native Python build and wheel tools

The installed `atlaslob` package has no third-party Python runtime dependency. Building its private
CPython extension and official wheel artifacts uses these exactly pinned tools:

| Project | Version | Source | License |
| --- | --- | --- | --- |
| scikit-build-core | 1.0.3 | <https://github.com/scikit-build/scikit-build-core> | Apache-2.0 |
| pybind11 | 3.0.4 | <https://github.com/pybind/pybind11> | BSD-3-Clause |
| cibuildwheel | 4.1.0 | <https://github.com/pypa/cibuildwheel> | BSD-2-Clause |
| build | 1.3.0 | <https://github.com/pypa/build> | MIT |
| auditwheel | 6.4.2 | <https://github.com/pypa/auditwheel> | MIT |

scikit-build-core drives the PEP 517 CMake build, pybind11 supplies the CPython binding layer, and
cibuildwheel builds the retained Linux wheel artifacts. `build` creates the retained source
distribution, and auditwheel inspects repaired wheel dependencies. They are build or development
tools and are not imported by the installed runtime package.

## Python development and evidence tools

The Python correctness package has no runtime dependency outside the Python standard library. The
Phase 4 `ReferenceRouter`, Generator V2, canonical evidence, and shrinking modules preserve that
boundary and add no third-party runtime dependency. Its optional development/test group declares
these exactly pinned top-level tools:

| Project | Version | Source | License |
| --- | --- | --- | --- |
| pytest | 9.1.1 | <https://github.com/pytest-dev/pytest> | MIT |
| Hypothesis | 6.160.0 | <https://github.com/HypothesisWorks/hypothesis> | MPL-2.0 |
| Ruff | 0.15.22 | <https://github.com/astral-sh/ruff> | MIT |
| mypy | 2.3.0 | <https://github.com/python/mypy> | MIT |

pytest, Ruff, and mypy provide unit execution, formatting/linting, and static typing. Hypothesis
drives the bounded seeded byte-stream and valid-command mutation fuzz tests. These tools are not
imported by the installed `atlaslob` runtime package.

Transitive development dependencies are resolved by pip and are not lockfile-pinned in this
release. Required top-level evidence tools are exactly pinned; a complete transitive environment
lock remains future reproducibility hardening.
