# Axon

> A cyberpunk-themed Arch package manager frontend using libalpm for NeurOS.

Axon provides a CLI and an ncurses package browser backed by shared transaction
and approval handling. It can install, remove, upgrade, inspect packages, and
perform maintenance. It is still in fucking **development**: pacman compatibility and
recovery handling are incomplete.

## Contents

- [Source navigation](#what-is-where)
- [Compile and run](#compile-and-run)
- [Terminal interface](#terminal-interface)
- [Install and package](#install-and-package)
- [Regression checks](#regression-checks)
- [Manual](#manual)
- [License](#license)

## What is where?

```text
Axon/
    .github/workflows/
        regression.yml            GCC/Clang regressions and sanitizer jobs
    src/
        alpm/
            alpm_core.c / .h       Context lifecycle and local package queries
            alpm_extra.c / .h      Repositories, configuration and transactions
            alpm_internal.h       Private libalpm context
        cli/
            main.c                Arguments, queries and command dispatch
            mutation.c / .h       Transaction review, prompts and progress bars
            maintenance.c / .h    Inspection, install reasons and cache commands
        config/
            pacman_config.c / .h  Configuration parsing and public reader API
            config_alpm.c         Apply configuration to a fresh backend
            config_internal.h     Private configuration and source locations
        core/
            operation.c / .h      Shared transaction lifecycle and policy API
            operation_internal.h  Private operation state
            plan.c                Transaction snapshots and size summaries
            callbacks.c           Events, questions, downloads and cancellation
            maintenance.c / .h    Orphans, database checks, reasons and cache policy
        security/
            privilege.c / .h      Process identity checks and root requirements
        ui/
            terminal.c / .h       Interactive package browser
            theme.h               ANSI palette and terminal color detection
    tests/
        alpm_extra_test.c          Isolated package and repository transactions
        config_test.c             Parser, backend setup and compatibility
        config_alloc_test.c       Parser allocation failures
        cli_test.c                Query, argument and maintenance CLI checks
        privilege_test.c          Authorization and simulated credential checks
        operation_test.c          Transaction plans, approval and failure handling
        maintenance_test.c        Cache policy, reasons and database diagnostics
        ui_test.c                 Terminal interaction through a pseudoterminal
        package_test.c            Staging, metadata and package reproducibility
        download_driver.c         Backend driver for download/privileged fixtures
        disk_full.c               Test-only write-failure injection
        download_test.py          HTTP mirrors, signatures and download failures
        privileged_test.py        Real privilege checks in namespaces and chroots
    man/
        axon.1                    Section 1 command manual
    Makefile                      Build, test, install and packaging targets
    README.md                     Project overview and usage
    .gitignore                    Generated-file exclusions
    LICENSE                       MIT license
```

The build generates `axon`, `libaxon-alpm.a`, objects (`.o`), header dependencies
(`.d`), and test binaries (`.out`/`.so`). `make clean` removes those files.
Staging and packaging artifacts live in `build/` and remain after `make clean`.

## Compile and run

Build on Linux with a C11 compiler (GCC or Clang), GNU Make, binutils,
`pkg-config`, **libalpm 16.x**, and **ncursesw 6 or newer**. Development has been
checked against libalpm 16.0.1. On Arch, libalpm is supplied by `pacman`.

```sh
make
./axon --help
./axon -Q                       # List installed packages
./axon -Qi pacman glibc          # Inspect installed packages
./axon -Ss '^pacman$'            # Search cached repositories
./axon --depends pacman          # Dependencies and reverse dependencies
./axon --orphans                 # Unneeded dependency packages
./axon --check-db                # Database/dependency checks; no repair
./axon --config /path/to/pacman.conf -Q
```

`--info` requires `--query` and package names. `--search` requires `--sync` and
regular expressions. Queries use cached databases; they do not refresh mirrors.
`--check-db` does not verify installed-file contents or checksums.

Package changes require root and explicit confirmation:

```sh
sudo ./axon -Syu                 # Refresh, then review a full system upgrade
sudo ./axon -S extra/vim         # Install an exact package from a repository
sudo ./axon -U ./package.pkg.tar.zst
sudo ./axon -R vim
sudo ./axon --asdeps PACKAGE
sudo ./axon --asexplicit PACKAGE
sudo ./axon --clean-cache        # Review removals under CleanMethod
```

`-Su` upgrades using cached databases; `-Sy` only refreshes them. Prefer `-Syu`
for system updates. Refresh/upgrade commands take no package targets, so
`-Sy PACKAGE` and `-Syu PACKAGE` are rejected. A second `-y` forces refresh.
Downgrades require `--allow-downgrade` and a separate confirmation. Protected
removals require approval for each exact package. There is no `--noconfirm`.
Group installation, AUR builds, and many pacman options are not implemented.

Axon reads `/etc/pacman.conf` by default and does not elevate privileges itself.
Queries need readable configuration and databases; mutations need an ordinary
root process. Setuid/setgid and inconsistent process identities are rejected.
`XferCommand` is parsed but prevents backend setup because it is unsupported.

Changes are not automatically rolled back after an error or interruption.
Read helper and hook diagnostics even when the transaction reports completion:
propagating every helper failure into the final status is still unfinished.

## Terminal interface

```sh
./axon --ui                      # Browse as your user
sudo ./axon --ui                 # Browse and perform confirmed mutations
```

The browser requires interactive input/output and at least a 40×8 terminal.

| Key | Action |
| --- | --- |
| Up/Down or `k`/`j` | Move through packages |
| Tab | Switch installed/repository lists |
| `/` | Filter names and descriptions |
| Space | Toggle selection |
| Enter | Show package details |
| `i` / `r` | Install from repositories / remove installed packages |
| `u` / `f` | Refresh and upgrade / install a local archive |
| `o` / `d` / `c` | Inspect orphans / check databases / clean cache |
| `e` / `b` | Mark installed packages explicit / dependency |
| `q` | Quit |

Package actions use selected visible rows, or the current row if none are
selected. The UI uses the same transaction review and approval path as the CLI.
Launching it unprivileged does not grant permission to mutate packages.

The palette uses blue `#5A96FF` for text, purple `#C170FF` for titles and
selections, and green `#00EB74` for progress and enabled indicators. CLI output
uses ANSI RGB colors; curses uses custom colors or terminal fallbacks. Set a
nonempty `NO_COLOR` to disable colors. Redirected CLI output stays plain.
Installation and download progress use terminal-width-aware bars;
`NoProgressBar` in pacman.conf suppresses them. The pacman `Color` directive
does not control Axon's automatic theme.

## Install and package

```sh
make stage                      # Stage under build/stage/usr/
make package                    # Create build/axon-VERSION-PKGREL-ARCH.pkg.tar.zst
make source-dist                # Create build/axon-VERSION.src.tar.zst
make release-check              # Check for test overrides and embedded RPATH
sudo make install               # Install executable, man page and license
sudo make uninstall             # Remove those installed files
```

Defaults are `/usr/bin/axon`, `/usr/share/man/man1/axon.1`, and
`/usr/share/licenses/axon/LICENSE`. Override `PREFIX`, `BINDIR`, `MANDIR`,
`LICENSEDIR`, or `DESTDIR` as needed. Package builds use `/usr` and accept
`VERSION`, `PKGREL`, `CARCH`, and `SOURCE_DATE_EPOCH` (default `0`). Packages
declare dependencies on pacman, ncurses, glibc, and the libalpm 16 ABI.

## Regression checks

Tests additionally need libarchive, a linker supporting GNU `--wrap`, Python 3,
GnuPG, util-linux (`unshare`), GNU tar and zstd. Sanitizers require Clang.
Configuration comparisons use `/usr/bin/pacman-conf` when available.

```sh
make test-c                     # C backend, configuration, CLI and UI suites
make test                       # C suites plus HTTP/signature/download fixtures
make test-privileged            # Production privilege checks in isolated roots
make test-package               # Install/uninstall, archives and reproducibility
make regression                 # All three test targets above
make sanitize                   # C suites with ASan, UBSan and leak checks
make clean                      # Clean before switching compiler/sanitizer flags
make
```

Tests use temporary databases, caches and package archives. Download fixtures
bind a loopback HTTP server. Some backend fixtures use test-only authorization
stubs; the privileged suite uses production checks inside user/mount/PID/network
namespaces and temporary chroots. These overrides are not linked into `axon`.
The privileged suite requires namespace creation permissions and fails if the
host disallows them. Leak checks require an environment permitting inspection
of the test processes.

[GitHub Actions](.github/workflows/regression.yml) runs GCC and Clang regression
jobs in Arch containers and a separate sanitizer job. Ordinary suites run as an
unprivileged user; the privileged fixture launcher runs as container root while
retaining namespace/chroot isolation. Logs and package artifacts are uploaded.

## Manual

```sh
man -l man/axon.1                # From the checkout
man axon                        # After installation
```

The [axon(1) source](man/axon.1) documents operations, UI keys, configuration,
privileges and exit statuses. A man-page viewer is not needed to build Axon.

## License

Axon is licensed under the [MIT license](LICENSE). Modification, redistribution,
and commercial use are permitted under its terms.
