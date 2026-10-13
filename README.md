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
            main.c                Arguments and command dispatch
            query.c / .h          Package queries, ownership and file checks
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
            inspection.c         File/metadata checks and database record validation
        security/
            privilege.c / .h      Process identity checks and root requirements
        ui/
            terminal.c / .h       Interactive package browser
            output.c / .h         Terminal escaping and Unicode column clipping
            theme.h               ANSI palette and terminal color detection
    tests/
        alpm_extra_test.c          Isolated package and repository transactions
        config_test.c             Parser, backend setup and compatibility
        config_alloc_test.c       Parser allocation failures
        cli_test.c                Query, argument and maintenance CLI checks
        privilege_test.c          Authorization and simulated credential checks
        operation_test.c          Transaction plans, approval and failure handling
        maintenance_test.c        Cache policy, reasons and database diagnostics
        inspection_test.c         Pacman comparisons and file/database inspection
        output_test.c             Terminal escaping and output failures
        progress_test.c           Progress rendering and output cancellation
        ui_test.c                 Terminal interaction through a pseudoterminal
        package_test.c            Staging, metadata and package reproducibility
        download_driver.c         Backend driver for download/privileged fixtures
        disk_full.c               Test-only write-failure injection
        download_test.c           HTTP mirrors, signatures and download failures
        privileged_test.c         Real privilege checks in namespaces and chroots
        helper_failure_test.c     Hooks, scriptlets, storage and transaction failures
        root_fixture.c / .h       Shared disposable-root fixture helpers
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
`pkg-config`, **libalpm 16.x**, **libarchive**, and **ncursesw 6 or newer**. Development has been
checked against libalpm 16.0.1. On Arch, libalpm is supplied by `pacman`.

```sh
make
./axon --help
./axon -Q                       # List installed packages
./axon -Qi pacman glibc          # Inspect installed packages
./axon -Qs '^pacman$'            # Search installed packages
./axon -Ss '^pacman$'            # Search cached repositories
./axon -Si extra/vim            # Inspect a repository package
./axon -Qk pacman                # Check installed-file presence
./axon -Qkk --checksums pacman   # Check metadata and recorded checksums
./axon --depends pacman          # Dependencies and reverse dependencies
./axon --orphans                 # Unneeded dependency packages
./axon --check-db                # Database/dependency checks; no repair
./axon --config /path/to/pacman.conf -Q
```

`--info` and `--search` work with either `--query` or `--sync`. Omitting package
names or search patterns selects all eligible packages. Search patterns are
regular expressions combined with AND. Queries use cached databases; they do
not refresh mirrors. File ownership (`-Qo`), file lists (`-Ql`), groups (`-Qg`,
`-Sg`, `-Sgg`), and foreign/native or install-reason filters are also supported.

`--check-db` checks database records and dependency consistency. Use `-Qk` for
installed-file presence or `-Qkk --checksums` for metadata and checksum checks.
Missing paths matched by `NoExtract` are accepted, including wildcard and
negated patterns. Existing files and inaccessible paths are still checked;
missing metadata or checksum coverage remains an error. Modified backup
configuration contents are reported separately from damaged files.

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
for system updates. A second `-y` forces refresh.
Downgrades require `--allow-downgrade` and a separate confirmation. Protected
removals require approval for each exact package. There is no `--noconfirm`.
`-S GROUP` expands all eligible group members into the approval plan; numeric
group subset selection is not implemented. Explicit targets matched by
`IgnorePkg` or `IgnoreGroup` require an additional affirmative answer.
Recursive (`-Rs`, `-Rss`), cascade (`-Rc`), no-backup (`-Rn`), and unneeded-only
(`-Ru`) removal modes are supported. AUR builds and many other pacman options
are not implemented.

Axon reads `/etc/pacman.conf` by default and does not elevate privileges itself.
Queries need readable configuration and databases; mutations need an ordinary
root process. Setuid/setgid and inconsistent process identities are rejected.
`XferCommand` is parsed but prevents backend setup because it is unsupported.

Changes are not automatically rolled back after an error or interruption.
Hook, scriptlet, and helper errors reported by libalpm fail the operation even
when libalpm reports commit success. Diagnostics report observed package
completions and warn when changes may have occurred. Approval-output failures
prevent the pending mutation, including reason changes and cache deletion.

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
Failed transactions or maintenance commands, and declined mutations, retain a
final exit status of 1 even after returning to browsing or a later successful
action.

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
declare dependencies on pacman, libarchive, ncurses, glibc, and the libalpm 16 ABI.

## Regression checks

Tests additionally need pacman and pacman-conf, a linker supporting GNU `--wrap`,
GnuPG, util-linux (`unshare`), GNU tar and zstd. The fixtures are written in C;
Python is not required. Sanitizers require Clang.

```sh
make test-c                     # Backend, configuration, CLI, inspection and UI suites
make test                       # Same suites plus C HTTP/signature/download fixtures
make test-privileged            # Production privilege checks in isolated roots
make test-package               # Install/uninstall, archives and reproducibility
make regression                 # All three test targets above
make sanitize                   # Run make test with ASan, UBSan and leak checks
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make test-privileged
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

`make sanitize` also builds the privileged fixtures; the separate command above
runs them with sanitizer checks enabled. These tests exercise disposable roots
and chroots. Installation, signed full upgrades, interruption, and reboot
recovery must also be validated in a disposable NeurOS VM before adopting Axon
as the distro's default frontend.

[GitHub Actions](.github/workflows/regression.yml) runs GCC and Clang regression
jobs in Arch containers and a separate sanitizer job. Ordinary suites run as an
unprivileged user; the privileged fixture launcher runs as container root while
retaining namespace/chroot isolation. Logs and package artifacts are uploaded.

## Manual

```sh
man -l man/axon.1                # From the checkout
man axon                        # After installation
```

The [axon(1) source](man/axon.1) documents operations, browser behavior, configuration,
privileges and exit statuses. A man-page viewer is not needed to build Axon.

## License

Axon is licensed under the [MIT license](LICENSE). Modification, redistribution,
and commercial use are permitted under its terms.
