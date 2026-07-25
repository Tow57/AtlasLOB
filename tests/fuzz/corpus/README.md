# Persistence decoder seed corpus

The retained decoder seeds are generated from the canonical `ATLSLG01`
header and command-record fixtures in
`tests/unit/persistence_decoder_fuzz_smoke_test.cpp` plus the reviewed
`ATLSSN01` golden fixture in
`tests/unit/persistence_snapshot_codec_test.cpp`.

Configure with `ATLAS_BUILD_FUZZERS=ON`, build the
`atlas_fuzz_log_header_decoder`, `atlas_fuzz_log_record_decoder`, and
`atlas_fuzz_snapshot_decoder` targets, then run
`tests/fuzz/run_command_log_fuzz_smoke.py`. The script creates raw seed files
in the build tree from the reviewed golden encodings, runs all three
libFuzzer targets with a fixed smoke budget, and leaves any newly discovered
inputs in that build-local corpus. The snapshot smoke campaign caps generated
inputs at 1 MiB; checked unit tests separately prove the 256 MiB decoder
limit and length-bomb handling. Source-tree corpus mutation is never
performed by the test.
