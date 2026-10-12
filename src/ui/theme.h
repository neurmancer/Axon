#ifndef AXON_THEME_H
#define AXON_THEME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AXON_BLUE "\033[38;2;90;150;255m"
#define AXON_PURPLE "\033[38;2;193;112;255m"
#define AXON_GREEN "\033[38;2;0;235;116m"
#define AXON_RESET "\033[0m"

static inline int axon_terminal_output(FILE *stream)
{
    const char *term = getenv("TERM");
    return(isatty(fileno(stream)) && term != NULL && strcmp(term, "dumb") != 0);
}

static inline const char *axon_color(FILE *stream, const char *color)
{
    const char *disabled = getenv("NO_COLOR");
    return(axon_terminal_output(stream) && !(disabled && *disabled) ? color : "");
}

#endif
