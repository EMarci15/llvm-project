#include "stdlib.h"
#include "string.h"

#include "proc_maps_parse.h"

namespace scudo {

inline static size_t index_of(char *str, const char c) {
    char *s = str;
    while (*s) {
        if (*s == c) return s-str;
        s++;
    }
    return -1;
}

inline static char *get_chunk_until(char* &str, const char delimiter) {
    size_t e = index_of(str, delimiter);
    DCHECK_LT(0,e);
    str[e] = '\0';
    char *result = str;
    str = &str[e+1];
    return result;
}

ProcRegion::ProcRegion(char *buff) {
    start_ptr = (uptr)strtoul(buff, &buff, 16); buff++;
    end_ptr = (uptr)strtoul(buff, &buff, 16); buff++;

    readable = (*buff++ == 'r');
    writable = (*buff++ == 'w');
    executable = (*buff++ == 'x');
    CoW = (*buff++ == 'p');
    buff++;

    offset = (uptr)strtoul(buff, &buff, 16); buff++;

    /*dev = */ get_chunk_until(buff, ' ');

    inode = (uptr)strtoul(buff, &buff, 10);

    while(*buff == ' ') buff++; // Ignore spaces
    has_path = (*buff == '[') || (*buff == '/');
    from_jemalloc = (strstr(buff, "jemalloc") != NULL);
    stack = (strcmp(buff, "[stack]\n") == 0);
    heap = (strcmp(buff, "[heap]\n") == 0);
}

};

