# Fix: `lane offsets` histogram message violates the `log_yaml()` two-line protocol

## Context

PR #232 (`feat/sve-port-avx53`) CI job **Build binary for CPU** fails, but the
failure is **not** caused by any PR #232 change:

- Failing bats case: `selftest_datacompare` → `not ok 95`, running
  `sdcshield ... -e selftest_datacomparefail_uint8_t`.
- Crash: `../framework/logging.cpp:1571: void format_and_print_raw_yaml(...):
  Assertion 'nl >= 0' failed.` → `Aborted (core dumped)` (exit 134).

The offending code lives in `framework/sandstone_run.cpp`
(`SandstoneMemcmpOrFail::report`), added by commit `e899b283`
(2026-09-17, SDC excitation enhancement suite) and already present on
`origin/main` (merged via PR #233). The PR only *exposes* it because
`.github/workflows/pr.yaml` triggers `build-cpu` **only on `pull_request`**
(never on a push to `main`) and builds with `-Dbuildtype=debugoptimized`,
where `assert()` is active (`b_ndebug=if-release`).

## Root cause

`log_yaml(char levelchar, const char *yaml)` prepends a `'\n'` to the caller
string (`log_thread_yaml` → `log_message_for_thread(..., '\n', yaml)`). The
formatter `format_and_print_raw_yaml` then expects a **two-line** message:

1. line 1 (empty, from the prepended `'\n'`) → heading, defaults to `details:`;
2. line 2 → the optional `text:` description, which must be terminated by a
   `'\n'`; remaining lines form the `details:` body.

The histogram code passed a **single line**
(`"lane offsets: N mismatching bytes in [a..b] (...)"`), so after the leading
empty line there is no second `'\n'` → `message.find('\n') == -1` → the
`assert(nl >= 0)` fires in debug builds. Release builds (`NDEBUG`) skip the
assert and silently produce malformed output, which is why the default local
`builddir` build does not reproduce it.

## Fix

Make the histogram emit a well-formed two-line message: the first line is the
`text:` description (`lane offsets`), the second is the histogram detail.

- File: `framework/sandstone_run.cpp` (our own code — upstream
  `logging.cpp` is left untouched, per the additive-port rule).
- `std::string hist = "lane offsets:";` → `"lane offsets\n"`.
- Drop the leading space in the appended stat line.

Resulting YAML:

```yaml
- level: warning
  text: 'lane offsets'
  details:
   1 mismatching bytes in [0..0] (single-lane pattern — one bad physical unit fingerprint)
```

## Verification (100% real)

- debugoptimized build (`builddir-dbg`, same flags as CI) — before: `exit=134`
  + `Assertion 'nl >= 0' failed`; after: all 18 `selftest_datacomparefail_*`
  cases exit **1** (expected fail), no `Aborted`, well-formed `text:`/`details:`.
- release build (`builddir`): `ninja` clean, zero warnings.
- regression: `./builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`.
- x86 non-regression by inspection: platform-independent string formatting, no
  arch guards touched.
