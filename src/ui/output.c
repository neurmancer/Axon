#define _GNU_SOURCE
#include "output.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static int output_failed;

int axon_output_failed(void)
{
    return(output_failed);
}

char *axon_display_text(const char *text, int columns)
{
    if(columns < 0) {
        columns = 0;
    }
    if(text == NULL) {
        text = "(unavailable)";
    }
    size_t length = strlen(text);
    if(length > (SIZE_MAX - 1) / 4) {
        return(NULL);
    }
    char *output = malloc(length * 4 + 1);
    if(output == NULL) {
        return(NULL);
    }
    size_t used = 0;
    int cells = 0;
    mbstate_t state = {0};
    while(*text) {
        wchar_t value;
        size_t size = mbrtowc(&value, text, length, &state);
        int invalid = size == (size_t)-1 || size == (size_t)-2;
        int control = !invalid && (iswcntrl(value) || (value >= 0x202a && value <= 0x202e)
            || (value >= 0x2066 && value <= 0x2069));
        if(invalid || control) {
            if(invalid) {
                size = 1;
                memset(&state, 0, sizeof(state));
            }
            if(size > (size_t)(columns - cells) / 4) {
                break;
            }
            for(size_t index = 0; index < size; index++) {
                snprintf(output + used, 5, "\\x%02x", (unsigned char)text[index]);
                used += 4;
                cells += 4;
            }
        }
        else {
            int width = wcwidth(value);
            if(width < 0) {
                width = 1;
            }
            if(width > columns - cells) {
                break;
            }
            memcpy(output + used, text, size);
            used += size;
            cells += width;
        }
        text += size;
        length -= size;
    }
    output[used] = '\0';
    return(output);
}

int axon_vfprintf(FILE *stream, const char *format, va_list arguments)
{
    va_list args;
    va_copy(args, arguments);
    int total = 0, failed = 0;
    for(const char *cursor = format; *cursor && !failed;) {
        if(*cursor != '%') {
            failed = fputc(*cursor++, stream) == EOF;
            total++;
            continue;
        }
        const char *start = cursor++;
        if(*cursor == '%') {
            failed = fputc(*cursor++, stream) == EOF;
            total++;
            continue;
        }
        while(*cursor && strchr("-+ #0", *cursor)) {
            cursor++;
        }
        int width = 0, precision = 0, has_width = 0, has_precision = 0;
        if(*cursor == '*') {
            has_width = 1;
            width = va_arg(args, int);
            cursor++;
        }
        while(*cursor >= '0' && *cursor <= '9') {
            cursor++;
        }
        if(*cursor == '.') {
            cursor++;
            if(*cursor == '*') {
                has_precision = 1;
                precision = va_arg(args, int);
                cursor++;
            }
            while(*cursor >= '0' && *cursor <= '9') {
                cursor++;
            }
        }
        char length = '\0';
        if(*cursor && strchr("zjtl", *cursor)) {
            length = *cursor++;
            if(length == 'l' && *cursor == 'l') {
                length = 'q';
                cursor++;
            }
        }
        char conversion = *cursor;
        if(!conversion || (size_t)(cursor - start) >= 63) {
            failed = 1;
            break;
        }
        cursor++;
        char spec[64];
        memcpy(spec, start, (size_t)(cursor - start));
        spec[cursor - start] = '\0';
        int count = -1;
#define WRITE(value) (has_width ? (has_precision ? fprintf(stream, spec, width, precision, (value)) : fprintf(stream, spec, width, (value))) : (has_precision ? fprintf(stream, spec, precision, (value)) : fprintf(stream, spec, (value))))
        if(conversion == 's' && length == '\0') {
            char *text = axon_display_text(va_arg(args, const char *), INT_MAX);
            if(text != NULL) {
                count = WRITE(text);
                free(text);
            }
        }
        else if(conversion == 'd' || conversion == 'i') {
            if(length == 'j') { count = WRITE(va_arg(args, intmax_t)); }
            else if(length == 'z' || length == 't') { count = WRITE(va_arg(args, ptrdiff_t)); }
            else if(length == 'l') { count = WRITE(va_arg(args, long)); }
            else if(length == 'q') { count = WRITE(va_arg(args, long long)); }
            else { count = WRITE(va_arg(args, int)); }
        }
        else if(strchr("uoxX", conversion) != NULL) {
            if(length == 'j') { count = WRITE(va_arg(args, uintmax_t)); }
            else if(length == 'z') { count = WRITE(va_arg(args, size_t)); }
            else if(length == 'l') { count = WRITE(va_arg(args, unsigned long)); }
            else if(length == 'q') { count = WRITE(va_arg(args, unsigned long long)); }
            else { count = WRITE(va_arg(args, unsigned int)); }
        }
        else if(conversion == 'c' && !length) {
            count = WRITE(va_arg(args, int));
        }
        else if(strchr("fFeEgGaA", conversion) != NULL && !length) {
            count = WRITE(va_arg(args, double));
        }
        else if(conversion == 'p' && !length) {
            count = WRITE(va_arg(args, void *));
        }
#undef WRITE
        if(count < 0 || count > INT_MAX - total) {
            failed = 1;
        }
        else {
            total += count;
        }
    }
    va_end(args);
    if(failed) {
        output_failed = 1;
        errno = EIO;
        return(-1);
    }
    return(total);
}

int axon_fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = axon_vfprintf(stream, format, args);
    va_end(args);
    return(result);
}
