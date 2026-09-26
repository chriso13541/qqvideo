# tinyfiledialogs -- vendored dependency notice

**Version vendored: v3.18.1 (May 2, 2024).**
**License: zlib (SPDX-License-Identifier: Zlib).**
**Source: https://github.com/wizzymore/tinyfiledialogs (mirrors upstream
https://sourceforge.net/projects/tinyfiledialogs/ -- the canonical home,
not reachable from this build environment's network allowlist).**

## Why this exact version, not just "latest available mirror"

Older tinyfiledialogs releases shell out to native dialog helpers
(`zenity`/`kdialog` on Linux) by building a command string and passing
it to `system()`. Two real CVEs exist because of this:

- **CVE-2020-36767** (versions before 3.8.0) -- shell metacharacters in
  titles/messages/filenames could break out of the intended command.
- **CVE-2023-47104** (versions before 3.15.0, CVSS 9.8 critical) -- an
  incomplete fix for the above; backticks and `$`-prefixed shell
  expansions were still exploitable even after 3.8.0's quote-only fix.

The first mirror found while integrating this (`native-toolkit/
tinyfiledialogs`, and its `libtinyfiledialogs` sibling) is a frozen 2017
snapshot of v2.9.3 -- vulnerable to BOTH CVEs. **Do not use that mirror
or any source claiming a version before 3.15.0.**

v3.18.1 (vendored here) was verified directly, not just trusted by
version number: `tinyfiledialogs.c` contains an explicit check (around
the function preceding `tinyfd_getGlobalChar`) that rejects any title/
message/path string containing a single quote, double quote, backtick,
or a `$` immediately followed by `(`, `_`, or an alphabetic character --
exactly the character classes both CVEs were about.

## If you ever update this vendored copy

Re-run the same check: search the new `tinyfiledialogs.c` for handling
of `` ` `` and `$` characters before replacing these files, and confirm
the version is >= 3.15.0. Don't just grab "whatever mirror comes up
first" -- that's exactly how the vulnerable 2017 snapshot almost ended
up in this tree.
