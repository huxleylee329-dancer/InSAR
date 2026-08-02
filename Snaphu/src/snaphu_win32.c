#include "snaphu_win32.h"

#ifdef _WIN32
static int to_wide(const char* input, wchar_t** output)
{
    int length;
    if (!input || strchr(input, '\0') != input + strlen(input)) return -1;
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, NULL, 0);
    if (length <= 0) return -1;
    *output = (wchar_t*)calloc((size_t)length, sizeof(wchar_t));
    if (!*output) return -1;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, *output, length) != length) { free(*output); *output = NULL; return -1; }
    return 0;
}
FILE* snaphu_fopen_utf8(const char* path, const char* mode)
{
    wchar_t *wpath = NULL, *wmode = NULL;
    char* binarymode = NULL;
    const char* openmode = mode;
    FILE* file = NULL;
    size_t length;

    if (!mode) return NULL;
    if (!strchr(mode, 'b')) {
        length = strlen(mode);
        binarymode = (char*)malloc(length + 2);
        if (!binarymode) goto done;
        memcpy(binarymode, mode, length);
        binarymode[length] = 'b';
        binarymode[length + 1] = '\0';
        openmode = binarymode;
    }
    if (to_wide(path, &wpath) || to_wide(openmode, &wmode)) goto done;
    file = _wfopen(wpath, wmode);

done:
    free(wpath);
    free(wmode);
    free(binarymode);
    return file;
}
int snaphu_remove_utf8(const char* path) { wchar_t* wide = NULL; int result = -1; if (!to_wide(path, &wide)) result = _wremove(wide); free(wide); return result; }
int snaphu_unlink_utf8(const char* path) { return snaphu_remove_utf8(path); }
int snaphu_mkdir_utf8(const char* path, int mode) { wchar_t* wide = NULL; int result = -1; (void)mode; if (!to_wide(path, &wide)) result = _wmkdir(wide); free(wide); return result; }
int snaphu_rmdir_utf8(const char* path) { wchar_t* wide = NULL; int result = -1; if (!to_wide(path, &wide)) result = _wrmdir(wide); free(wide); return result; }
int snaphu_stat_utf8(const char* path, struct stat* result) { wchar_t* wide = NULL; DWORD attributes; if (!result || to_wide(path, &wide)) return -1; attributes = GetFileAttributesW(wide); free(wide); if (attributes == INVALID_FILE_ATTRIBUTES) return -1; result->st_mode = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? 0040000 : 0100000; return 0; }
#endif
