#ifndef AXON_OUTPUT
#define AXON_OUTPUT
#include <stdio.h>
#include <stdarg.h>

char *axon_display_text(const char *text, int columns);
int axon_fprintf(FILE *stream, const char *format, ...) __attribute__((format(printf, 2, 3)));
int axon_vfprintf(FILE *stream, const char *format, va_list args);
int axon_output_failed(void);
#endif
