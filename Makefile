CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
CPPFLAGS += -Isrc/alpm -Isrc/config $(shell $(PKG_CONFIG) --cflags libalpm)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
ALPM_LIBS = $(shell $(PKG_CONFIG) --libs libalpm)
TEST_FLAGS = $(shell $(PKG_CONFIG) --cflags libarchive)
TEST_LIBS = $(shell $(PKG_CONFIG) --libs libarchive)
OBJECTS = src/alpm/alpm_core.o src/alpm/alpm_extra.o \
          src/config/pacman_config.o src/config/config_alpm.o

.PHONY: all test clean

all: libaxon-alpm.a

libaxon-alpm.a: $(OBJECTS)
	$(AR) rcs $@ $^

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

tests/alpm_extra_test.out: tests/alpm_extra_test.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(TEST_LIBS) $(LDLIBS) -o $@

tests/config_test.out: tests/config_test.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(LDLIBS) -o $@

tests/config_alloc_test.out: tests/config_alloc_test.c libaxon-alpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< libaxon-alpm.a $(ALPM_LIBS) $(LDLIBS) \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=strdup,--wrap=free -o $@

test: tests/alpm_extra_test.out tests/config_test.out tests/config_alloc_test.out
	./tests/alpm_extra_test.out
	./tests/config_test.out
	./tests/config_alloc_test.out

clean:
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) libaxon-alpm.a tests/alpm_extra_test.out tests/config_test.out tests/config_alloc_test.out

-include $(OBJECTS:.o=.d)
