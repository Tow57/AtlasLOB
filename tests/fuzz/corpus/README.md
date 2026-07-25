# Command-log decoder seed corpus

The retained command-log decoder seeds are generated from the canonical
`ATLSLG01` header and command-record fixtures in
`tests/unit/persistence_decoder_fuzz_smoke_test.cpp`.

Configure with `ATLAS_BUILD_FUZZERS=ON`, build the
`atlas_fuzz_log_header_decoder` and `atlas_fuzz_log_record_decoder` targets,
then run `tests/fuzz/run_command_log_fuzz_smoke.py`. The script creates raw
seed files in the build tree from the reviewed golden encodings, runs both
libFuzzer targets with a fixed smoke budget, and leaves any newly discovered
inputs in that build-local corpus. Source-tree corpus mutation is never
performed by the test.
