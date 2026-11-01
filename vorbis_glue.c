/* stb_vorbis (public domain) lives in its own file because it and minimp3 define helpers with the same names. */
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"
int crate_ogg_decode(const unsigned char *mem, int len, int *channels, int *rate, short **out) {
    return stb_vorbis_decode_memory(mem, len, channels, rate, out);
}
