.DEFAULT_GOAL := all
CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
CPPFLAGS += -Isrc/alpm -Isrc/config -Isrc/security -Isrc/core $(shell $(PKG_CONFIG) --cflags libalpm)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
UI_CFLAGS = $(shell $(PKG_CONFIG) --cflags ncursesw)
UI_LIBS = $(shell $(PKG_CONFIG) --libs ncursesw)
src/ui/terminal.o: CPPFLAGS += $(UI_CFLAGS)
ALPM_LIBS = $(shell $(PKG_CONFIG) --libs libalpm libarchive)
TEST_FLAGS = $(shell $(PKG_CONFIG) --cflags libarchive)
TEST_LIBS = $(shell $(PKG_CONFIG) --libs libarchive)
OBJECTS = src/alpm/alpm_core.o src/alpm/alpm_extra.o \
          src/config/pacman_config.o src/config/config_alpm.o src/security/privilege.o \
          src/core/operation.o src/core/plan.o src/core/callbacks.o src/core/maintenance.o src/core/inspection.o src/ui/output.o

.PHONY: all test clean

all: axon

axon: src/cli/query.o src/cli/main.o src/cli/mutation.o src/cli/maintenance.o src/ui/terminal.o libaxon-alpm.a
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(ALPM_LIBS) $(UI_LIBS) $(LDLIBS) -o $@

libaxon-alpm.a: $(OBJECTS)
	$(AR) rcs $@ $^

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

tests/alpm_extra_test.out: tests/alpm_extra_test.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(TEST_LIBS) $(LDLIBS) -Wl,--wrap=axon_privilege_check -o $@

tests/config_test.out: tests/config_test.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(LDLIBS) -o $@

tests/config_alloc_test.out: tests/config_alloc_test.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(LDLIBS) \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=strdup,--wrap=free -o $@

tests/cli_test.out: tests/cli_test.c axon
	$(CC) $(TEST_FLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< $(TEST_LIBS) $(LDLIBS) -o $@

tests/privilege_test.out: tests/privilege_test.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(LDLIBS) \
		-Wl,--wrap=getresuid,--wrap=getresgid,--wrap=getauxval -o $@

tests/operation_test.out: tests/operation_test.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(TEST_LIBS) $(LDLIBS) -Wl,--wrap=axon_privilege_check,--wrap=axon_alpm_transaction_release,--wrap=axon_alpm_transaction_commit,--wrap=malloc,--wrap=calloc,--wrap=strdup,--wrap=alpm_list_append,--wrap=alpm_list_append_strdup -o $@

tests/download_driver.out: tests/download_driver.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(LDLIBS) -Wl,--wrap=axon_privilege_check -o $@

tests/privileged_driver.out: tests/download_driver.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -DAXON_TEST_PRODUCTION $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(LDLIBS) -o $@

.PHONY: test-privileged
tests/helper_failure_test.out: tests/helper_failure_test.c tests/root_fixture.c tests/root_fixture.h libaxon-alpm.a axon
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< tests/root_fixture.c libaxon-alpm.a $(ALPM_LIBS) $(TEST_LIBS) $(LDLIBS) -lutil -o $@

test-privileged: axon tests/privileged_test.out tests/helper_failure_test.out
	./tests/privileged_test.out
	unshare --user --map-root-user --mount --pid --fork --net ./tests/helper_failure_test.out

tests/disk_full.so: tests/disk_full.c
	$(CC) $(CFLAGS) -fPIC -shared $(LDFLAGS) $< -ldl -o $@

tests/maintenance_test.out: tests/maintenance_test.c src/cli/maintenance.o libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $^ $(ALPM_LIBS) $(TEST_LIBS) $(LDLIBS) -Wl,--wrap=axon_privilege_check,--wrap=axon_fprintf,--wrap=malloc -o $@

tests/ui_test.out: tests/ui_test.c src/ui/terminal.o src/cli/mutation.o src/cli/maintenance.o libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $^ $(ALPM_LIBS) $(UI_LIBS) $(LDLIBS) -lutil -Wl,--wrap=axon_privilege_check -o $@

tests/package_test.out: tests/package_test.c axon
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< $(ALPM_LIBS) $(TEST_LIBS) $(LDLIBS) -o $@

.PHONY: test-package regression
test-package: tests/package_test.out
	./tests/package_test.out

regression: test test-privileged test-package

test: tests/download_test.out tests/ui_test.out tests/maintenance_test.out tests/disk_full.so tests/download_driver.out tests/alpm_extra_test.out tests/config_test.out tests/config_alloc_test.out tests/cli_test.out tests/privilege_test.out tests/operation_test.out
	./tests/alpm_extra_test.out
	./tests/config_test.out
	./tests/config_alloc_test.out
	./tests/cli_test.out
	./tests/privilege_test.out
	./tests/operation_test.out
	./tests/maintenance_test.out
	./tests/inspection_test.out
	./tests/output_test.out
	./tests/progress_test.out
	./tests/ui_test.out
	./tests/download_test.out

clean:
	$(RM) tests/progress_test.out
	$(RM) tests/download_test.out tests/privileged_test.out tests/output_test.out
	$(RM) tests/inspection_test.out
	$(RM) src/cli/query.o src/cli/query.d
	$(RM) tests/helper_failure_test.out
	$(RM) src/cli/maintenance.o src/cli/maintenance.d src/ui/terminal.o src/ui/terminal.d
	$(RM) axon src/cli/main.o src/cli/main.d src/cli/mutation.o src/cli/mutation.d
	$(RM) tests/maintenance_test.out tests/ui_test.out tests/package_test.out
	$(RM) tests/cli_test.out
	$(RM) tests/privilege_test.out
	$(RM) tests/operation_test.out tests/download_driver.out tests/privileged_driver.out tests/disk_full.so
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) libaxon-alpm.a tests/alpm_extra_test.out tests/config_test.out tests/config_alloc_test.out

-include src/cli/query.d $(OBJECTS:.o=.d) src/cli/main.d src/cli/mutation.d src/cli/maintenance.d src/ui/terminal.d

PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
LICENSEDIR ?= $(PREFIX)/share/licenses/axon
DESTDIR ?=
VERSION ?= 0.1.0
PKGREL ?= 1
CARCH ?= $(shell uname -m)
SOURCE_DATE_EPOCH ?= 0
PACKAGE = build/axon-$(VERSION)-$(PKGREL)-$(CARCH).pkg.tar.zst

.PHONY: check-deps release-check install uninstall stage package source-dist

check-deps:
	@$(PKG_CONFIG) --exists 'libalpm >= 16.0' 'libalpm < 17.0' || { echo 'Axon requires libalpm 16.x (pacman development files).' >&2; exit 1; }
	@$(PKG_CONFIG) --exists 'ncursesw >= 6.0' || { echo 'Axon requires ncursesw 6.x or newer.' >&2; exit 1; }
	@$(PKG_CONFIG) --exists libarchive || { echo 'Axon requires libarchive development files.' >&2; exit 1; }

src/alpm/alpm_core.o src/ui/terminal.o: | check-deps

release-check: axon
	@if nm axon | grep -Eq '__wrap_|axon_test'; then echo 'Refusing a release executable containing test overrides.' >&2; exit 1; fi
	@if readelf -d axon | grep -Eq '\((RPATH|RUNPATH)\)'; then echo 'Refusing a release executable with an embedded runtime library path.' >&2; exit 1; fi

install: axon
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(MANDIR)/man1" "$(DESTDIR)$(LICENSEDIR)"
	install -m 0755 axon "$(DESTDIR)$(BINDIR)/axon"
	install -m 0644 man/axon.1 "$(DESTDIR)$(MANDIR)/man1/axon.1"
	install -m 0644 LICENSE "$(DESTDIR)$(LICENSEDIR)/LICENSE"

uninstall:
	$(RM) "$(DESTDIR)$(BINDIR)/axon" "$(DESTDIR)$(MANDIR)/man1/axon.1" "$(DESTDIR)$(LICENSEDIR)/LICENSE"

stage: axon
	$(MAKE) install DESTDIR="$(CURDIR)/build/stage"

package: release-check
	@mkdir -p build
	@set -eu; \
	axon_stage=$$(mktemp -d "$(CURDIR)/build/package.XXXXXX"); \
	trap 'rm -rf "$$axon_stage"' EXIT HUP INT TERM; \
	$(MAKE) install PREFIX=/usr BINDIR=/usr/bin MANDIR=/usr/share/man LICENSEDIR=/usr/share/licenses/axon DESTDIR="$$axon_stage"; \
	axon_size=$$(du -sb "$$axon_stage" | cut -f1); \
	{ printf '%s\n' 'pkgname = axon' 'pkgbase = axon' 'pkgver = $(VERSION)-$(PKGREL)' \
	'pkgdesc = NeurOS package manager with CLI and terminal interface' 'arch = $(CARCH)' \
	'license = MIT' 'depend = pacman' 'depend = ncurses' 'depend = glibc' 'depend = libarchive'; \
	printf 'depend = libalpm.so=16-%s\n' "$$(getconf LONG_BIT)"; \
	printf 'builddate = %s\nsize = %s\n' '$(SOURCE_DATE_EPOCH)' "$$axon_size"; \
	} > "$$axon_stage/.PKGINFO"; \
	tar --sort=name --format=posix --pax-option=delete=atime,delete=ctime \
	--mtime='@$(SOURCE_DATE_EPOCH)' --owner=0 --group=0 --numeric-owner \
	-C "$$axon_stage" --zstd -cf "$(CURDIR)/$(PACKAGE)" .PKGINFO usr

source-dist:
	@mkdir -p build
	tar --sort=name --mtime='@$(SOURCE_DATE_EPOCH)' --owner=0 --group=0 --numeric-owner \
	--exclude='*.o' --exclude='*.d' --exclude='*.out' --exclude='*.so' --exclude='__pycache__' --transform='s,^,axon-$(VERSION)/,' \
	--zstd -cf build/axon-$(VERSION).src.tar.zst Makefile LICENSE src man tests

.PHONY: test-c sanitize

test-c: tests/alpm_extra_test.out tests/config_test.out tests/config_alloc_test.out tests/cli_test.out tests/privilege_test.out tests/operation_test.out tests/maintenance_test.out tests/ui_test.out
	./tests/alpm_extra_test.out
	./tests/config_test.out
	./tests/config_alloc_test.out
	./tests/cli_test.out
	./tests/privilege_test.out
	./tests/operation_test.out
	./tests/maintenance_test.out
	./tests/inspection_test.out
	./tests/output_test.out
	./tests/progress_test.out
	./tests/ui_test.out

sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
	$(MAKE) test tests/privileged_test.out tests/helper_failure_test.out CC=clang CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined'

tests/inspection_test.out: tests/inspection_test.c libaxon-alpm.a axon
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(TEST_LIBS) $(LDLIBS) -o $@

test test-c: tests/inspection_test.out

tests/download_test.out: tests/download_test.c tests/root_fixture.c tests/root_fixture.h tests/download_driver.out tests/disk_full.so libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< tests/root_fixture.c libaxon-alpm.a $(ALPM_LIBS) $(LDLIBS) -o $@

tests/privileged_test.out: tests/privileged_test.c tests/root_fixture.c tests/root_fixture.h tests/privileged_driver.out tests/helper_failure_test.out libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< tests/root_fixture.c libaxon-alpm.a $(ALPM_LIBS) $(LDLIBS) -o $@

tests/output_test.out: tests/output_test.c src/ui/output.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $^ $(LDLIBS) -o $@

test test-c: tests/output_test.out

tests/progress_test.out: tests/progress_test.c src/cli/mutation.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(LDLIBS) -lutil -o $@

test test-c: tests/progress_test.out
