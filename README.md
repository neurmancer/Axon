# Axon

> An Arch package manager frontend using libalpm for the NeurOS distro project.
>
> _Lowkey...nothing works just yet broo...just wait a little more_
> and this is just a side-quest so...no comments in the source at least not the funny ones
> Once everything will work I'll add some cyberpunk flavor lol...


## Contents

- [Source navigation](#what-is-where)
- [Compile and run](#compile-and-run)
- [Manual](#manual)
- [License](#license)

## What is where?

```text
Axon/
    src/
        alpm/
            alpm_core.c / .h       Context lifecycle and local package shit queries
            alpm_extra.c / .h      Repositories, configuration and transactions
            alpm_internal.h       [classified] context 
        cli/
            main.c                Argument handling query output and errors
        
        config/
            pacman_config.c / .h   Configuration parsing and public reader API
            config_alpm.c         parsed configuration to a fresh backend
            config_internal.h     Private configuration and source locations
        security/
            privilege.c / .h      Process identity checks and root requirements (agian I am not a CyberSec guy...)
    tests/
        alpm_extra_test.c         Isolated package and repository transactions
        cli_test.c                CLI subprocess tests with temporary databases
        config_test.c             Parser, configuration setup, and compatibility
        config_alloc_test.c       Parser allocation-failure handling
        privilege_test.c          Authorization checks and privilege changes
    
    man/
        axon.1                    Section 1 command manual
    
    Makefile                      
    README.md                                           
    .gitignore                    
    LICENSE                       MIT license
```

The build generates `axon`, `libaxon-alpm.a`, object files (`.o`), header
dependency files (`.d`), and test executables (`.out`). These are gitignored;
`make clean` removes them.

## Compile and run

Build on Linux with a C11 compiler, GNU Make, an archiver (`ar`), `pkg-config`,
and libalpm headers and libraries. Development has been checked against
libalpm 16.0.1. Tests also require libarchive headers and libraries and a linker
supporting GNU `--wrap`. The reference configuration comparisons use
`/usr/bin/pacman-conf` when available; otherwise that comparison is skipped.

From the project root:

```sh
make
./axon --help
```

Use the default `/etc/pacman.conf` to query your system:

```sh
./axon -Q                       # List installed packages
./axon -Qi pacman glibc          # Inspect installed packages by exact name
./axon --query --info pacman     # Equivalent long options
./axon -Ss '^pacman$'            # Search cached repository data
```

`--info` requires `--query` and at least one package name. `--search` requires
`--sync` and at least one regular expression. Search does not refresh repository
databases; missing caches are reported as errors.

To use another configuration:

```sh
./axon --config /path/to/pacman.conf -Q
```

Queries do not require root when the configuration and databases are readable.
The executable does not elevate privileges automatically. The configuration
reader rejects malformed or unsupported settings; `XferCommand` currently
prevents backend setup even for queries.

Run the checks or remove generated files:

```sh
make test
make clean
```

Tests use temporary fixtures and do not require root. Package mutation tests
use a test-only authorization stub; privilege tests separately exercise the
real checks and simulated credential changes. Neither bypass is linked into
`axon`.

## Manual

Read the command manual directly from the checkout:

```sh
man -l man/axon.1
```

The [axon(1) source](man/axon.1) documents options, examples, configuration
limits, privileges, and exit statuses. Reading it requires a man-page viewer;
building Axon does not.

## License

Axon is licensed under the [MIT license](LICENSE). Modification, redistribution,
and commercial use are permitted under its terms.
