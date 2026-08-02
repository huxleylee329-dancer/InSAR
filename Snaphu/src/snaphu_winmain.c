#include <windows.h>
#include <stdlib.h>

int snaphu_main(int argc, char** argv);

int wmain(int argc, wchar_t** argv)
{
    int i, result;
    char** utf8argv = (char**)calloc((size_t)argc, sizeof(char*));
    if (!utf8argv) return 1;
    for (i = 0; i < argc; ++i) {
        int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, NULL, 0, NULL, NULL);
        if (length <= 0 || !(utf8argv[i] = (char*)calloc((size_t)length, 1)) || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, utf8argv[i], length, NULL, NULL) != length) { result = 1; goto done; }
    }
    result = snaphu_main(argc, utf8argv);
done:
    for (i = 0; i < argc; ++i) free(utf8argv[i]);
    free(utf8argv);
    return result;
}
