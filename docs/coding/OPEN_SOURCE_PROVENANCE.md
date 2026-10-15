# Open Source Provenance Statement (开源溯源说明)

> This document supports openEuler community compliance review for the SDCShield project.

## Project Identity

- **Project name**: SDCShield
- **License**: Apache License 2.0 (see [LICENSE](../LICENSE))
- **NOTICE**: see [NOTICE](../NOTICE)

## Derivation Chain

SDCShield is a derived work with the following lineage:

1. **Original upstream**: [OpenDCDiag](https://github.com/intel/OpenDCDiag) by Intel Corporation,
   containing the `sandstone` CPU-testing framework. Licensed under Apache License 2.0.
2. **ARM64 port**: [opendcdiag-arm](https://github.com/wangxumarshall/sdcshield) (renamed to sdcshield) — a fork of
   Intel's OpenDCDiag ported to ARM64 (Kunpeng 920 / generic ARMv8.1+), with the x86-64
   reference architecture preserved.
3. **SDCShield**: the present repository — a rebrand of the ARM64 port for contribution to the
   openEuler community. The rename covers user-facing identity only (project name, binary name,
   documentation, scripts, CI). The internal `sandstone` framework symbols, header filenames,
   and architecture remain unchanged Intel-origin code.

## Apache-2.0 Obligations Fulfilled

| Obligation | How fulfilled |
|---|---|
| §1 Retain copyright notices | All original source files keep their `Copyright 202x Intel Corporation.` / `SPDX-License-Identifier: Apache-2.0` headers byte-identical. No Intel copyright line has been deleted or overwritten. |
| §2 Retain LICENSE | Root `LICENSE` is the full Apache-2.0 text, unchanged. |
| §4 NOTICE | Upstream had no NOTICE file; this project adds one stating the derivation and upstream URLs. |
| 3rd-party code | `LICENSE.3rdparty` retains all third-party entries (forkfd, FreeBSD) verbatim. |
| Modification marking | This rebrand is documented here and in git history. Git history is preserved (no squash/rebase of upstream commits). |

## Trademark Position

- The `OpenDCDiag` name appears in this repository **only** in descriptive derivation
  statements ("SDCShield is derived from OpenDCDiag"), which is nominative fair use.
- The `sandstone` codename appears **only** inside source code (symbol names, header
  filenames) as licensed Intel-origin code; it is never used as an external project,
  product, or community name.
- The external project name is exclusively **SDCShield**.

## DCO

Every commit made as part of the SDCShield rebrand carries a `Signed-off-by:` trailer
(DCO). Original upstream commits retain their original authorship and sign-offs.
