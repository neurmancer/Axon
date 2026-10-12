#define _GNU_SOURCE
#include "../src/ui/output.h"
#include "../src/ui/theme.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    assert(setlocale(LC_CTYPE, "C.UTF-8") != NULL);
    char *text = axon_display_text("a\033]52;c;payload\a\n\r\t\177", INT_MAX);
    assert(text && strcmp(text, "a\\x1b]52;c;payload\\x07\\x0a\\x0d\\x09\\x7f") == 0);
    free(text);
    text = axon_display_text("界éx", 3);
    assert(text && strcmp(text, "界é") == 0);
    free(text);
    text = axon_display_text("界éx", 1);
    assert(text && !*text);
    free(text);
    text = axon_display_text("a\342\200\256b\377", INT_MAX);
    assert(text && strcmp(text, "a\\xe2\\x80\\xaeb\\xff") == 0);
    free(text);
    char *output = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&output, &length);
    assert(stream);
    assert(axon_fprintf(stream, "%s %zu %jd %ju %ld %lld %u %.2f %%\n", "x\033[2J", (size_t)4,
                       (intmax_t)-5, (uintmax_t)6, (long)7, (long long)8, 9U, 1.25) > 0);
    assert(fclose(stream) == 0);
    assert(strcmp(output, "x\\x1b[2J 4 -5 6 7 8 9 1.25 %\n") == 0);
    free(output);
    assert(!axon_output_failed());
    assert(strcmp(axon_color(stdout, AXON_BLUE), "") == 0);
    assert(setenv("NO_COLOR", "1", 1) == 0 && strcmp(axon_color(stderr, AXON_GREEN), "") == 0);
    stream = fopen("/dev/full", "w");
    assert(stream && setvbuf(stream, NULL, _IONBF, 0) == 0);
    assert(axon_fprintf(stream, "%s", "error") < 0 && axon_output_failed());
    fclose(stream);
    puts("Terminal escaping, UTF-8 clipping, redirected color, and output failures passed.");
    return(0);
}
