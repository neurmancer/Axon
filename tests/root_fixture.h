#ifndef AXON_ROOT_FIXTURE
#define AXON_ROOT_FIXTURE
#include <archive.h>
extern char project[4096];
void copy_binary(const char *source, const char *destination);
void write_text(const char *path, const char *text);
void entry(struct archive *archive, const char *name, const char *text);
void fixture(int scenario);
#endif
