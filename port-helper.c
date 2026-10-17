/*
 * port-helper.c - part of the portable AnyPS5 folder.
 *
 * port-game.bat and build-anyps5.bat compile this file with the GCC that is
 * already in tools\mingw64, into bin\port-helper.exe. It does the work of
 * port-game.bat:
 *
 *   - finds the game in what you dragged: the dump folder itself, a folder
 *     around it, the eboot.bin file, or a game folder plus its patch folder
 *   - copies the game into ported\<name>\app0 (your dump is never changed)
 *   - turns fake-signed SELF files (eboot.bin, sce_module\*.prx, ...) in that
 *     copy into the plain ELF files AnyPS5 needs
 *   - finds extra module folders such as Unity's Media\Plugins
 *   - runs AnyPS5's relinker, copies the libraries and writes Play.bat
 *
 * Encrypted (retail) SELF files cannot be turned into ELF files on a PC.
 *
 * Commands (port-game.bat uses "port", "version" and "dragged"):
 *   port-helper port <AnyPS5 folder> <dragged item>...
 *   port-helper plan <AnyPS5 folder> <dragged item>...  (show what port would do)
 *   port-helper info <file>...                           (what kind of file)
 *   port-helper convert <SELF file> <ELF file>
 *   port-helper fixtree <folder> [executable name]      (convert a copied game)
 *   port-helper version <number>                         (exit 0 if it matches)
 *   port-helper dragged              (exit 0 if port-game.bat was started by
 *                                     dragging onto it or double-clicking it)
 *
 * build-anyps5.bat also uses it in place of PowerShell while building AnyPS5;
 * see "Stand-in for PowerShell" below.
 */

#define HELPER_VERSION 2

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
#define SEP '\\'
#define RELINKER_NAME "relinker.exe"
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#define SEP '/'
#define RELINKER_NAME "relinker"
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

/* ------------------------------------------------------------------------ */
/* Small utilities                                                           */
/* ------------------------------------------------------------------------ */

static void out_of_memory(void)
{
    fputs("port-helper: out of memory\n", stderr);
    exit(3);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

static void *xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = xmalloc(n);
    memcpy(d, s, n);
    return d;
}

static char *xvfmt(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) n = 0;
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    return s;
}

static char *xfmt(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = xvfmt(fmt, ap);
    va_end(ap);
    return s;
}

/* Text for the console. Paths can contain any characters, so on Windows the
   console gets UTF-16 directly; redirected output is written as UTF-8. */
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = xvfmt(fmt, ap);
    va_end(ap);
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (h != NULL && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
        if (wn > 1) {
            wchar_t *w = xmalloc((size_t)wn * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wn);
            DWORD written;
            WriteConsoleW(h, w, (DWORD)(wn - 1), &written, NULL);
            free(w);
        }
        free(s);
        return;
    }
#endif
    fputs(s, stdout);
    fflush(stdout);
    free(s);
}

static int lower_ascii(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower_ascii((unsigned char)*a) != lower_ascii((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int iends(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && ieq(s + n - m, suffix);
}

static int icontains(const char *s, const char *sub)
{
    size_t m = strlen(sub);
    for (; *s; s++) {
        size_t i = 0;
        while (i < m && s[i] && lower_ascii((unsigned char)s[i]) == lower_ascii((unsigned char)sub[i])) i++;
        if (i == m) return 1;
    }
    return 0;
}

typedef struct {
    char **v;
    size_t n, cap;
} strlist;

static void sl_add(strlist *l, char *s) /* takes ownership of s */
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = xrealloc(l->v, l->cap * sizeof(char *));
    }
    l->v[l->n++] = s;
}

static void sl_free(strlist *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void sl_sort(strlist *l)
{
    if (l->n > 1) qsort(l->v, l->n, sizeof(char *), cmp_str);
}

static int is_sep(char c)
{
    return c == '\\' || c == '/';
}

static char *pjoin(const char *a, const char *b)
{
    size_t n = strlen(a);
    if (n && is_sep(a[n - 1])) return xfmt("%s%s", a, b);
    return xfmt("%s%c%s", a, SEP, b);
}

static const char *pbase(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (is_sep(*p)) b = p + 1;
    return b;
}

static char *pdir(const char *p) /* parent folder, keeps "C:\" and "/" */
{
    const char *b = pbase(p);
    size_t n = (size_t)(b - p);
    while (n > 1 && is_sep(p[n - 1]) && !(n == 3 && p[1] == ':')) n--;
    if (n == 0) return xstrdup(".");
    char *d = xmalloc(n + 1);
    memcpy(d, p, n);
    d[n] = 0;
    return d;
}

static int is_drive_root(const char *p)
{
    return p[0] && p[1] == ':' && is_sep(p[2]) && p[3] == 0;
}

/* Is path equal to dir or inside it? (ASCII case-insensitive, separators equal) */
static int path_within(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    while (n && is_sep(dir[n - 1])) n--;
    for (size_t i = 0; i < n; i++) {
        char a = path[i], b = dir[i];
        if (!a) return 0;
        if (is_sep(a) && is_sep(b)) continue;
        if (lower_ascii((unsigned char)a) != lower_ascii((unsigned char)b)) return 0;
    }
    return path[n] == 0 || is_sep(path[n]);
}

/* ------------------------------------------------------------------------ */
/* File system layer                                                         */
/* ------------------------------------------------------------------------ */

typedef struct {
    int exists, is_dir, is_link;
    uint64_t size;
} pstat_t;

#ifdef _WIN32

static wchar_t *w_of(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) n = 1;
    wchar_t *w = xmalloc((size_t)n * sizeof(wchar_t));
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

static char *u_of(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) n = 1;
    char *s = xmalloc((size_t)n);
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) s[0] = 0;
    return s;
}

/* Full path with the \\?\ prefix, so folders deeper than 260 characters work. */
static wchar_t *w_long(const char *s)
{
    wchar_t *w = w_of(s);
    for (wchar_t *q = w; *q; q++)
        if (*q == L'/') *q = L'\\';
    if (wcsncmp(w, L"\\\\?\\", 4) == 0) return w;
    size_t n = wcslen(w);
    wchar_t *r;
    if (n >= 3 && w[1] == L':' && w[2] == L'\\') {
        r = xmalloc((n + 5) * sizeof(wchar_t));
        wcscpy(r, L"\\\\?\\");
        wcscat(r, w);
    } else if (n >= 2 && w[0] == L'\\' && w[1] == L'\\') {
        r = xmalloc((n + 8) * sizeof(wchar_t));
        wcscpy(r, L"\\\\?\\UNC\\");
        wcscat(r, w + 2);
    } else {
        return w;
    }
    free(w);
    return r;
}

static int p_stat(const char *path, pstat_t *st)
{
    memset(st, 0, sizeof *st);
    wchar_t *w = w_long(path);
    WIN32_FILE_ATTRIBUTE_DATA d;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok) return 0;
    st->exists = 1;
    st->is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->is_link = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    st->size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return 0;
}

typedef struct {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    int pending;
} pdir_t;

static pdir_t *p_opendir(const char *path)
{
    char *pattern = pjoin(path, "*");
    wchar_t *w = w_long(pattern);
    free(pattern);
    pdir_t *d = xcalloc(1, sizeof *d);
    d->h = FindFirstFileExW(w, FindExInfoBasic, &d->fd, FindExSearchNameMatch, NULL, 2 /* FIND_FIRST_EX_LARGE_FETCH */);
    if (d->h == INVALID_HANDLE_VALUE) d->h = FindFirstFileW(w, &d->fd);
    free(w);
    if (d->h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES) return d; /* empty */
        free(d);
        return NULL;
    }
    d->pending = 1;
    return d;
}

static int p_readdir(pdir_t *d, char **name, pstat_t *st)
{
    for (;;) {
        if (d->h == INVALID_HANDLE_VALUE) return 0;
        if (!d->pending) {
            if (!FindNextFileW(d->h, &d->fd)) return 0;
        }
        d->pending = 0;
        if (wcscmp(d->fd.cFileName, L".") == 0 || wcscmp(d->fd.cFileName, L"..") == 0) continue;
        memset(st, 0, sizeof *st);
        st->exists = 1;
        st->is_dir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        st->is_link = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        st->size = ((uint64_t)d->fd.nFileSizeHigh << 32) | d->fd.nFileSizeLow;
        *name = u_of(d->fd.cFileName);
        return 1;
    }
}

static void p_closedir(pdir_t *d)
{
    if (!d) return;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
}

static FILE *p_fopen(const char *path, const char *mode)
{
    wchar_t *w = w_long(path);
    wchar_t *m = w_of(mode);
    FILE *f = _wfopen(w, m);
    free(w);
    free(m);
    return f;
}

static int p_mkdir1(const char *path)
{
    wchar_t *w = w_long(path);
    BOOL ok = CreateDirectoryW(w, NULL);
    DWORD e = ok ? 0 : GetLastError();
    free(w);
    if (ok || e == ERROR_ALREADY_EXISTS) return 0;
    pstat_t st;
    p_stat(path, &st);
    return st.exists && st.is_dir ? 0 : -1;
}

static int p_remove(const char *path)
{
    wchar_t *w = w_long(path);
    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
    BOOL ok = DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}

static int p_rmdir1(const char *path)
{
    wchar_t *w = w_long(path);
    BOOL ok = RemoveDirectoryW(w);
    free(w);
    return ok ? 0 : -1;
}

static int p_replace(const char *from, const char *to)
{
    wchar_t *a = w_long(from), *b = w_long(to);
    BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        SetFileAttributesW(b, FILE_ATTRIBUTE_NORMAL);
        ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    free(a);
    free(b);
    return ok ? 0 : -1;
}

static int p_copyfile(const char *from, const char *to)
{
    wchar_t *a = w_long(from), *b = w_long(to);
    BOOL ok = CopyFileW(a, b, FALSE);
    if (!ok) {
        SetFileAttributesW(b, FILE_ATTRIBUTE_NORMAL);
        ok = CopyFileW(a, b, FALSE);
    }
    free(a);
    free(b);
    return ok ? 0 : -1;
}

/* An environment variable as UTF-8, or NULL. */
static char *p_getenv(const char *name)
{
    wchar_t *wn = w_of(name);
    DWORD n = GetEnvironmentVariableW(wn, NULL, 0);
    char *r = NULL;
    if (n > 0) {
        wchar_t *buf = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
        DWORD got = GetEnvironmentVariableW(wn, buf, n + 1);
        if (got > 0 && got <= n) r = u_of(buf);
        free(buf);
    }
    free(wn);
    return r;
}

static char *p_fullpath(const char *path)
{
    wchar_t *w = w_of(path);
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (n == 0) {
        free(w);
        return xstrdup(path);
    }
    wchar_t *buf = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
    DWORD got = GetFullPathNameW(w, n + 1, buf, NULL);
    free(w);
    if (got == 0 || got > n) {
        free(buf);
        return xstrdup(path);
    }
    char *s = u_of(buf);
    free(buf);
    size_t len = strlen(s);
    while (len > 1 && is_sep(s[len - 1]) && !is_drive_root(s) && !(len == 2 && is_sep(s[0]))) s[--len] = 0;
    return s;
}

static void wappend(wchar_t **buf, size_t *len, size_t *cap, const wchar_t *s, size_t n)
{
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = *cap ? *cap * 2 : 256;
        *buf = xrealloc(*buf, *cap * sizeof(wchar_t));
    }
    memcpy(*buf + *len, s, n * sizeof(wchar_t));
    *len += n;
    (*buf)[*len] = 0;
}

/* Quote one argument the way the C runtime splits command lines. */
static void wappend_arg(wchar_t **buf, size_t *len, size_t *cap, const wchar_t *arg)
{
    if (*len) wappend(buf, len, cap, L" ", 1);
    if (arg[0] && !wcspbrk(arg, L" \t\n\v\"")) {
        wappend(buf, len, cap, arg, wcslen(arg));
        return;
    }
    wappend(buf, len, cap, L"\"", 1);
    for (const wchar_t *p = arg;; p++) {
        size_t slashes = 0;
        while (*p == L'\\') {
            p++;
            slashes++;
        }
        if (*p == 0) {
            for (size_t i = 0; i < slashes * 2; i++) wappend(buf, len, cap, L"\\", 1);
            break;
        }
        if (*p == L'"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) wappend(buf, len, cap, L"\\", 1);
        } else {
            for (size_t i = 0; i < slashes; i++) wappend(buf, len, cap, L"\\", 1);
        }
        wappend(buf, len, cap, p, 1);
    }
    wappend(buf, len, cap, L"\"", 1);
}

/* Runs a program in this console; returns its exit code, -1 if it did not start. */
static int p_run(char **argv)
{
    wchar_t *cmd = NULL;
    size_t len = 0, cap = 0;
    for (int i = 0; argv[i]; i++) {
        wchar_t *w = w_of(argv[i]);
        wappend_arg(&cmd, &len, &cap, w);
        free(w);
    }
    wchar_t *app = w_of(argv[0]);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    fflush(stdout);
    BOOL ok = CreateProcessW(app, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(app);
    free(cmd);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

#else /* POSIX: used for testing the same logic on Linux */

static int p_stat(const char *path, pstat_t *st)
{
    memset(st, 0, sizeof *st);
    struct stat s;
    if (lstat(path, &s) != 0) return 0;
    st->exists = 1;
    st->is_link = S_ISLNK(s.st_mode);
    st->is_dir = S_ISDIR(s.st_mode);
    st->size = (uint64_t)s.st_size;
    return 0;
}

typedef struct {
    DIR *d;
    char *path;
} pdir_t;

static pdir_t *p_opendir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return NULL;
    pdir_t *r = xcalloc(1, sizeof *r);
    r->d = d;
    r->path = xstrdup(path);
    return r;
}

static int p_readdir(pdir_t *d, char **name, pstat_t *st)
{
    struct dirent *e;
    while ((e = readdir(d->d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char *full = pjoin(d->path, e->d_name);
        p_stat(full, st);
        free(full);
        if (!st->exists) continue;
        *name = xstrdup(e->d_name);
        return 1;
    }
    return 0;
}

static void p_closedir(pdir_t *d)
{
    if (!d) return;
    closedir(d->d);
    free(d->path);
    free(d);
}

static FILE *p_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

static int p_mkdir1(const char *path)
{
    if (mkdir(path, 0777) == 0 || errno == EEXIST) return 0;
    return -1;
}

static int p_remove(const char *path)
{
    return unlink(path);
}

static int p_rmdir1(const char *path)
{
    return rmdir(path);
}

static int p_replace(const char *from, const char *to)
{
    return rename(from, to);
}

static int p_copyfile(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb");
    if (!a) return -1;
    FILE *b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return -1;
    }
    char *buf = xmalloc(1 << 20);
    size_t n;
    int bad = 0;
    while ((n = fread(buf, 1, 1 << 20, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) bad = 1;
    free(buf);
    fclose(a);
    if (fclose(b) != 0) bad = 1;
    return bad ? -1 : 0;
}

static char *p_getenv(const char *name)
{
    const char *v = getenv(name);
    return v ? xstrdup(v) : NULL;
}

static char *p_fullpath(const char *path)
{
    char *r = realpath(path, NULL);
    if (r) return r;
    if (path[0] == '/') return xstrdup(path);
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) return xstrdup(path);
    return pjoin(cwd, path);
}

static int p_run(char **argv)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
    return -1;
}

#endif

static int p_mkdirs(const char *path)
{
    size_t n = strlen(path);
    char *tmp = xstrdup(path);
    size_t start = 1;
    if (n >= 3 && tmp[1] == ':' && is_sep(tmp[2])) start = 3;
    else if (n >= 2 && is_sep(tmp[0]) && is_sep(tmp[1])) {
        /* \\server\share\ : skip the server and share names */
        size_t i = 2, parts = 0;
        while (i < n && parts < 2) {
            if (is_sep(tmp[i])) parts++;
            i++;
        }
        start = i;
    }
    for (size_t i = start; i <= n; i++) {
        if (i == n || is_sep(tmp[i])) {
            char c = tmp[i];
            tmp[i] = 0;
            if (i > 0 && !(i == 2 && tmp[1] == ':') && p_mkdir1(tmp) != 0) {
                free(tmp);
                return -1;
            }
            tmp[i] = c;
        }
    }
    free(tmp);
    return 0;
}

static int p_rmtree(const char *path)
{
    pstat_t st;
    p_stat(path, &st);
    if (!st.exists) return 0;
    if (!st.is_dir || st.is_link) return st.is_dir ? p_rmdir1(path) : p_remove(path);
    pdir_t *d = p_opendir(path);
    if (!d) return -1;
    char *name;
    pstat_t e;
    int bad = 0;
    while (p_readdir(d, &name, &e)) {
        char *full = pjoin(path, name);
        if (e.is_dir && !e.is_link) bad |= p_rmtree(full);
        else if (e.is_dir) bad |= p_rmdir1(full);
        else bad |= p_remove(full);
        free(full);
        free(name);
    }
    p_closedir(d);
    if (p_rmdir1(path) != 0) bad = -1;
    return bad ? -1 : 0;
}

static int file_exists(const char *path)
{
    pstat_t st;
    p_stat(path, &st);
    return st.exists && !st.is_dir;
}

static int dir_exists(const char *path)
{
    pstat_t st;
    p_stat(path, &st);
    return st.exists && st.is_dir;
}

/* Reads up to `limit` bytes (the whole file if limit is 0). */
static uint8_t *read_file(const char *path, uint64_t limit, uint64_t *got, uint64_t *total)
{
    pstat_t st;
    p_stat(path, &st);
    if (!st.exists || st.is_dir) return NULL;
    uint64_t want = st.size;
    if (limit && want > limit) want = limit;
    if (want > (uint64_t)0x7FFFFFFF) return NULL; /* larger than 2 GiB: not an executable */
    FILE *f = p_fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = xmalloc((size_t)want + 1);
    size_t n = want ? fread(b, 1, (size_t)want, f) : 0;
    fclose(f);
    if (n != want) {
        free(b);
        return NULL;
    }
    *got = want;
    *total = st.size;
    return b;
}

static int write_file(const char *path, const uint8_t *data, uint64_t n)
{
    FILE *f = p_fopen(path, "wb");
    if (!f) return -1;
    int bad = n && fwrite(data, 1, (size_t)n, f) != n;
    if (fclose(f) != 0) bad = 1;
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------------------ */
/* SELF -> ELF                                                               */
/* ------------------------------------------------------------------------ */

/*
 * A PS4/PS5 SELF file is a container around an ELF file:
 *   0x00  header (magic 4F 15 3D 1D or 54 14 F5 EE; segment count at 0x18,
 *         the size of the signed part at 0x10)
 *   0x20  one 32-byte entry per stored segment: flags, offset, size, size
 *   then  the ELF header and program headers of the wrapped ELF
 *   then  extra information, metadata, a signature and the segment data
 * Entries with the "has blocks" flag hold the bytes of one program header
 * (its index is in flags bits 20+); the others hold block digests.
 * A fake-signed SELF stores the bytes unencrypted, so the ELF can be rebuilt
 * by putting every stored segment back at its file offset. The PT_SCE_VERSION
 * data is the tail of the file. Section headers are not stored.
 * Matches make_fself.py (ps5-payload-dev SDK, PS4 magic), the PS5-magic
 * fake-signed files of real game dumps, and the loaders of shadPS4 and the
 * PS5 SELF decrypter.
 */

enum { K_NONE, K_SMALL, K_OTHER, K_ELF, K_FSELF, K_ENCRYPTED, K_COMPRESSED, K_BADSELF };

static const char *kind_text(int k)
{
    switch (k) {
    case K_ELF: return "an ELF file";
    case K_FSELF: return "a fake-signed SELF file";
    case K_ENCRYPTED: return "an encrypted SELF file";
    case K_COMPRESSED: return "a compressed SELF file";
    case K_BADSELF: return "a damaged SELF file";
    case K_OTHER: return "not a program file";
    case K_SMALL: return "too small to be a program file";
    default: return "missing or unreadable";
    }
}

#define SELF_FLAG_ENCRYPTED 0x2ull
#define SELF_FLAG_COMPRESSED 0x8ull
#define SELF_FLAG_HAS_BLOCKS 0x800ull
#define SELF_FLAG_HAS_DIGESTS 0x10000ull
#define PT_SCE_VERSION 0x6FFFFF01u
#define SCAN_PREFIX ((uint64_t)8 << 20)

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int is_self_magic(const uint8_t *b)
{
    uint32_t m = rd32(b);
    return m == 0x1D3D154Fu || m == 0xEEF51454u;
}

static int is_elf_magic(const uint8_t *b)
{
    return b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
}

typedef struct {
    uint64_t off, size;
    uint32_t ph;
} seg_map;

typedef struct {
    uint64_t elf_off, hdr_len, out_size, signed_size;
    uint16_t phnum, e_type;
    const uint8_t *ph;
    seg_map *maps;
    size_t nmaps;
    uint8_t *mapped; /* per program header */
    int ps5;
} self_t;

static void self_free(self_t *s)
{
    free(s->maps);
    free(s->mapped);
    s->maps = NULL;
    s->mapped = NULL;
}

static uint32_t ph_type(const self_t *s, uint32_t i) { return rd32(s->ph + (size_t)i * 56); }
static uint64_t ph_offset(const self_t *s, uint32_t i) { return rd64(s->ph + (size_t)i * 56 + 8); }
static uint64_t ph_filesz(const self_t *s, uint32_t i) { return rd64(s->ph + (size_t)i * 56 + 32); }

/* `b` holds the first `have` bytes of a SELF file of `total` bytes. */
static int self_parse(const uint8_t *b, uint64_t have, uint64_t total, self_t *s, char *why, size_t whyn)
{
    memset(s, 0, sizeof *s);
#define SELF_BAD(...)                          \
    do {                                       \
        snprintf(why, whyn, __VA_ARGS__);      \
        self_free(s);                          \
        return K_BADSELF;                      \
    } while (0)
    if (have < 0x20) SELF_BAD("its header is incomplete");
    s->ps5 = rd32(b) == 0xEEF51454u;
    s->signed_size = rd64(b + 0x10);
    uint32_t count = rd16(b + 0x18);
    if (count == 0) SELF_BAD("it lists no segments");
    uint64_t elf_off = 0x20 + (uint64_t)count * 0x20;
    if (elf_off + 0x40 > have) SELF_BAD("it ends before the ELF header");
    const uint8_t *e = b + elf_off;
    if (!is_elf_magic(e)) SELF_BAD("it does not contain an ELF header");
    if (e[4] != 2 || e[5] != 1) SELF_BAD("it is not a 64-bit little-endian ELF");
    if (rd16(e + 0x12) != 0x3E) SELF_BAD("it is not an x86-64 program");
    uint64_t phoff = rd64(e + 0x20);
    uint32_t ehsize = rd16(e + 0x34), phentsize = rd16(e + 0x36), phnum = rd16(e + 0x38);
    if (phentsize != 56 || phnum == 0) SELF_BAD("its program header table is invalid");
    if (phoff < 0x40 || phoff > 0x10000) SELF_BAD("its program header offset is invalid");
    uint64_t hdr_len = phoff + (uint64_t)phnum * 56;
    if (ehsize > hdr_len) hdr_len = ehsize;
    if (elf_off + hdr_len > have) SELF_BAD("it ends inside the program headers");
    s->elf_off = elf_off;
    s->hdr_len = hdr_len;
    s->phnum = (uint16_t)phnum;
    s->e_type = rd16(e + 0x10);
    s->ph = e + phoff;

    /* Entries with "has blocks" carry segment bytes. Without any such entry,
       fall back to every entry that is not a digest table. */
    int blocked = 0;
    for (uint32_t i = 0; i < count; i++)
        if (rd64(b + 0x20 + (size_t)i * 0x20) & SELF_FLAG_HAS_BLOCKS) blocked = 1;
    size_t ndata = 0;
    int encrypted = 0, compressed = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t flags = rd64(b + 0x20 + (size_t)i * 0x20);
        int data = blocked ? (flags & SELF_FLAG_HAS_BLOCKS) != 0 : (flags & SELF_FLAG_HAS_DIGESTS) == 0;
        if (!data) continue;
        ndata++;
        if (flags & SELF_FLAG_ENCRYPTED) encrypted = 1;
        if (flags & SELF_FLAG_COMPRESSED) compressed = 1;
    }
    if (ndata == 0) SELF_BAD("it holds no segment data");
    if (encrypted) {
        snprintf(why, whyn, "its segments are encrypted");
        return K_ENCRYPTED;
    }
    if (compressed) {
        snprintf(why, whyn, "its segments are compressed");
        return K_COMPRESSED;
    }

    s->maps = xcalloc(ndata, sizeof(seg_map));
    s->mapped = xcalloc(phnum, 1);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *ent = b + 0x20 + (size_t)i * 0x20;
        uint64_t flags = rd64(ent), off = rd64(ent + 8), size = rd64(ent + 16);
        int data = blocked ? (flags & SELF_FLAG_HAS_BLOCKS) != 0 : (flags & SELF_FLAG_HAS_DIGESTS) == 0;
        if (!data || size == 0) continue;
        if (off > total || size > total - off) SELF_BAD("segment %u points outside the file (the file may be incomplete)", i);
        uint32_t idx = (uint32_t)((flags >> 20) & 0xFFFF);
        long j = -1;
        if (idx < phnum && ph_filesz(s, idx) == size) {
            if (s->mapped[idx]) SELF_BAD("segment %u is stored twice", i);
            j = idx;
        } else {
            for (uint32_t k = 0; k < phnum; k++) {
                if (s->mapped[k] || ph_filesz(s, k) != size) continue;
                if (j >= 0) SELF_BAD("segment %u cannot be matched to the program headers", i);
                j = (long)k;
            }
            if (j < 0) SELF_BAD("segment %u does not match the program headers", i);
        }
        s->mapped[j] = 1;
        s->maps[s->nmaps].off = off;
        s->maps[s->nmaps].size = size;
        s->maps[s->nmaps].ph = (uint32_t)j;
        s->nmaps++;
    }

    /* Every program header range must exist in the rebuilt file. */
    uint64_t out = hdr_len;
    for (uint32_t k = 0; k < phnum; k++) {
        uint64_t o = ph_offset(s, k), z = ph_filesz(s, k);
        if (z == 0) continue;
        if (o > UINT64_MAX - z) SELF_BAD("program header %u is invalid", k);
        if (o + z > out) out = o + z;
    }
    if (out > total * 2 + ((uint64_t)16 << 20) || out > (uint64_t)0x7FFFFFFF) SELF_BAD("its ELF layout is not plausible");
    s->out_size = out;
    return K_FSELF;
#undef SELF_BAD
}

/* Rebuilds the ELF from a whole SELF file `b` of `n` bytes, parsed into `s`. */
static uint8_t *self_build(const uint8_t *b, uint64_t n, const self_t *s, int *version_missing)
{
    uint8_t *o = xcalloc((size_t)s->out_size, 1);
    memcpy(o, b + s->elf_off, (size_t)s->hdr_len);
    for (size_t i = 0; i < s->nmaps; i++)
        memcpy(o + ph_offset(s, s->maps[i].ph), b + s->maps[i].off, (size_t)s->maps[i].size);
    /* The PT_SCE_VERSION bytes are not a stored segment: they are the tail of
       the file. make_fself.py writes them at the signed size (16-byte aligned);
       other signers write them right after the last segment. Both end at the
       end of the file. AnyPS5 does not use them, but keep them faithful. */
    uint64_t data_end = 0;
    for (size_t i = 0; i < s->nmaps; i++)
        if (s->maps[i].off + s->maps[i].size > data_end) data_end = s->maps[i].off + s->maps[i].size;
    *version_missing = 0;
    for (uint32_t k = 0; k < s->phnum; k++) {
        if (ph_type(s, k) != PT_SCE_VERSION || ph_filesz(s, k) == 0 || s->mapped[k]) continue;
        uint64_t len = ph_filesz(s, k);
        if (len <= n && n - len >= data_end) memcpy(o + ph_offset(s, k), b + (n - len), (size_t)len);
        else if (s->signed_size <= n && len <= n - s->signed_size) memcpy(o + ph_offset(s, k), b + s->signed_size, (size_t)len);
        else *version_missing = 1;
    }
    wr64(o + 0x28, 0); /* e_shoff: a SELF keeps no section headers */
    wr16(o + 0x3A, 0);
    wr16(o + 0x3C, 0);
    wr16(o + 0x3E, 0);
    return o;
}

typedef struct {
    int kind;
    uint16_t e_type, machine;
    int ps5;
    char why[200];
} finfo_t;

static void classify_file(const char *path, finfo_t *fi)
{
    memset(fi, 0, sizeof *fi);
    uint64_t got = 0, total = 0;
    /* 64 bytes tell ELF, SELF and anything else apart; only a SELF needs more. */
    uint8_t *b = read_file(path, 64, &got, &total);
    if (!b) {
        fi->kind = K_NONE;
        snprintf(fi->why, sizeof fi->why, "it cannot be read");
        return;
    }
    if (got < 4) fi->kind = K_SMALL;
    else if (is_elf_magic(b)) {
        fi->kind = K_ELF;
        if (got >= 0x14) {
            fi->e_type = rd16(b + 0x10);
            fi->machine = rd16(b + 0x12);
        }
    } else if (is_self_magic(b)) {
        free(b);
        b = read_file(path, SCAN_PREFIX, &got, &total);
        if (!b) {
            fi->kind = K_NONE;
            snprintf(fi->why, sizeof fi->why, "it cannot be read");
            return;
        }
        self_t s;
        fi->kind = self_parse(b, got, total, &s, fi->why, sizeof fi->why);
        fi->e_type = s.e_type;
        fi->machine = 0x3E;
        fi->ps5 = s.ps5;
        self_free(&s);
    } else {
        fi->kind = K_OTHER;
    }
    free(b);
}

static int is_module_type(uint16_t t) { return t == 0xFE18 || t == 3; }

/* Converts a fake-signed SELF file to an ELF file. Returns 0 on success. */
static int convert_self(const char *src, const char *dst, char *why, size_t whyn)
{
    uint64_t got = 0, total = 0;
    uint8_t *b = read_file(src, 0, &got, &total);
    if (!b) {
        snprintf(why, whyn, "it cannot be read");
        return -1;
    }
    if (got < 4 || !is_self_magic(b)) {
        free(b);
        snprintf(why, whyn, "it is not a SELF file");
        return -1;
    }
    self_t s;
    int kind = self_parse(b, got, total, &s, why, whyn);
    if (kind != K_FSELF) {
        free(b);
        return -1;
    }
    int version_missing = 0;
    uint8_t *o = self_build(b, got, &s, &version_missing);
    uint64_t on = s.out_size;
    self_free(&s);
    free(b);
    int rc = write_file(dst, o, on);
    free(o);
    if (rc) snprintf(why, whyn, "the converted file could not be written");
    return rc;
}

static int convert_in_place(const char *path, char *why, size_t whyn)
{
    char *tmp = xfmt("%s.port-tmp", path);
    int rc = convert_self(path, tmp, why, whyn);
    if (rc == 0 && p_replace(tmp, path) != 0) {
        snprintf(why, whyn, "the converted file could not replace the original");
        rc = -1;
    }
    if (rc != 0) p_remove(tmp);
    free(tmp);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* Converting a copied game folder                                           */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *exe;      /* the executable's file name in the top folder */
    int converted, elf_files;
    strlist problems;     /* "relative\path: reason" */
    strlist module_dirs;  /* folders for --module-dir */
    strlist notes;
} tree_t;

static int is_standard_module_dir(const char *name)
{
    return ieq(name, "sce_module") || ieq(name, "sce_modules") || ieq(name, "prx");
}

static void walk_tree(tree_t *t, const char *dir, const char *rel, int depth)
{
    if (depth > 64) return;
    pdir_t *d = p_opendir(dir);
    if (!d) {
        sl_add(&t->problems, xfmt("%s: this folder could not be read", rel[0] ? rel : "."));
        return;
    }
    int has_module = 0, has_program = 0;
    strlist subdirs = {0};
    char *name;
    pstat_t st;
    while (p_readdir(d, &name, &st)) {
        if (st.is_link) { /* never follow links out of the copy */
            free(name);
            continue;
        }
        if (st.is_dir) {
            if ((depth == 0 && ieq(name, "sce_sys")) || ieq(name, "fakelib")) free(name);
            else sl_add(&subdirs, name);
            continue;
        }
        char *full = pjoin(dir, name);
        char *relname = rel[0] ? pjoin(rel, name) : xstrdup(name);
        if (iends(name, ".port-tmp")) {
            p_remove(full); /* left over from an interrupted run */
        } else if (!iends(name, ".guest.prx") && st.size >= 0x40) {
            finfo_t fi;
            classify_file(full, &fi);
            if (fi.kind == K_FSELF) {
                char why[200];
                if (convert_in_place(full, why, sizeof why) == 0) {
                    t->converted++;
                    classify_file(full, &fi);
                } else {
                    sl_add(&t->problems, xfmt("%s: %s", relname, why));
                }
            } else if (fi.kind == K_ENCRYPTED || fi.kind == K_COMPRESSED || fi.kind == K_BADSELF) {
                sl_add(&t->problems, xfmt("%s: %s (%s)", relname, kind_text(fi.kind), fi.why));
            }
            if (fi.kind == K_ELF && fi.machine == 0x3E) {
                t->elf_files++;
                if (is_module_type(fi.e_type)) has_module = 1;
                else if (!(depth == 0 && ieq(name, t->exe))) has_program = 1;
            }
        }
        free(relname);
        free(full);
        free(name);
    }
    p_closedir(d);

    int standard = depth == 1 && is_standard_module_dir(pbase(rel));
    if (has_module && !standard) {
        if (has_program) sl_add(&t->notes, xfmt("Not used as a module folder (it also contains programs): %s", rel[0] ? rel : "."));
        else sl_add(&t->module_dirs, xstrdup(rel[0] ? rel : "."));
    }
    sl_sort(&subdirs);
    for (size_t i = 0; i < subdirs.n; i++) {
        char *sub = pjoin(dir, subdirs.v[i]);
        char *subrel = rel[0] ? pjoin(rel, subdirs.v[i]) : xstrdup(subdirs.v[i]);
        walk_tree(t, sub, subrel, depth + 1);
        free(sub);
        free(subrel);
    }
    sl_free(&subdirs);
}

/* ------------------------------------------------------------------------ */
/* Finding the game in what was dragged                                      */
/* ------------------------------------------------------------------------ */

static const char *const EXE_NAMES[] = {"eboot.bin", "eboot.elf", "eboot.self", "eboot.bin.elf", NULL};

/* Returns the name of the game executable in `dir`, or NULL. */
static char *pick_exe(const char *dir)
{
    const char *first = NULL;
    for (int i = 0; EXE_NAMES[i]; i++) {
        char *p = pjoin(dir, EXE_NAMES[i]);
        int there = file_exists(p);
        if (there) {
            finfo_t fi;
            classify_file(p, &fi);
            if (fi.kind == K_ELF || fi.kind == K_FSELF) {
                free(p);
                return xstrdup(EXE_NAMES[i]);
            }
            if (!first) first = EXE_NAMES[i];
        }
        free(p);
    }
    return first ? xstrdup(first) : NULL;
}

static int skip_search_dir(const char *name)
{
    return ieq(name, "sce_sys") || ieq(name, "sce_module") || ieq(name, "sce_modules") || ieq(name, "fakelib") ||
           ieq(name, "$RECYCLE.BIN") || ieq(name, "System Volume Information");
}

typedef struct {
    char *root, *exe, *title, *version;
    int patch;
} part_t;

typedef struct {
    part_t *v;
    size_t n, cap;
} partlist;

static void pl_add(partlist *l, char *root, char *exe)
{
    for (size_t i = 0; i < l->n; i++)
        if (path_within(root, l->v[i].root) && path_within(l->v[i].root, root)) { /* same folder */
            free(root);
            free(exe);
            return;
        }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = xrealloc(l->v, l->cap * sizeof(part_t));
    }
    memset(&l->v[l->n], 0, sizeof(part_t));
    l->v[l->n].root = root;
    l->v[l->n].exe = exe;
    l->n++;
}

/* Searches `dir` for game folders (the ones with an executable). A folder
   with "patch" in its name but without an executable may hold the changed
   data files of an update; those are collected in `patches`. */
static void find_games(const char *dir, int depth, partlist *found, partlist *patches, const char *anyps5, int *count)
{
    if (*count >= 64) return;
    char *exe = pick_exe(dir);
    if (exe) {
        pl_add(found, xstrdup(dir), exe);
        (*count)++;
        return;
    }
    if (icontains(pbase(dir), "patch")) pl_add(patches, xstrdup(dir), NULL);
    if (depth >= 6) return;
    pdir_t *d = p_opendir(dir);
    if (!d) return;
    strlist subs = {0};
    char *name;
    pstat_t st;
    while (p_readdir(d, &name, &st)) {
        if (st.is_dir && !st.is_link && !skip_search_dir(name)) sl_add(&subs, name);
        else free(name);
    }
    p_closedir(d);
    sl_sort(&subs);
    for (size_t i = 0; i < subs.n; i++) {
        char *sub = pjoin(dir, subs.v[i]);
        if (!path_within(sub, anyps5)) find_games(sub, depth + 1, found, patches, anyps5, count);
        free(sub);
    }
    sl_free(&subs);
}

static int is_title_id(const char *s)
{
    for (int i = 0; i < 9; i++) {
        char c = s[i];
        if (i < 4 ? !(c >= 'A' && c <= 'Z') : !(c >= '0' && c <= '9')) return 0;
    }
    return 1;
}

/* The text value of "key" in the folder's sce_sys\param.json, or NULL. */
static char *param_string(const char *root, const char *key)
{
    char *pj = pjoin(root, "sce_sys");
    char *pj2 = pjoin(pj, "param.json");
    free(pj);
    uint64_t got = 0, total = 0;
    uint8_t *b = read_file(pj2, (uint64_t)1 << 20, &got, &total);
    free(pj2);
    if (!b) return NULL;
    b[got] = 0;
    char *v = NULL;
    char *pattern = xfmt("\"%s\"", key);
    char *k = strstr((char *)b, pattern);
    if (k) {
        k += strlen(pattern);
        while (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n' || *k == ':') k++;
        char *e = *k == '"' ? strchr(k + 1, '"') : NULL;
        if (e && e - k - 1 > 0 && e - k - 1 < 64) {
            size_t n = (size_t)(e - k - 1);
            v = xmalloc(n + 1);
            memcpy(v, k + 1, n);
            v[n] = 0;
        }
    }
    free(pattern);
    free(b);
    return v;
}

/* The title ID in the folder's sce_sys\param.json, or NULL. */
static char *param_title(const char *root)
{
    char *t = param_string(root, "titleId");
    if (t && (strlen(t) != 9 || !is_title_id(t))) {
        free(t);
        t = NULL;
    }
    return t;
}

/* The last title ID in a text such as "...\PPSA12544-app0", or NULL. */
static char *text_title(const char *s)
{
    const char *last = NULL;
    size_t n = strlen(s);
    for (size_t i = 0; i + 9 <= n; i++) {
        const char *p = s + i;
        if (is_title_id(p) && (i == 0 || !((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z')))) last = p;
    }
    if (!last) return NULL;
    char *t = xmalloc(10);
    memcpy(t, last, 9);
    t[9] = 0;
    return t;
}

/* The game's title ID: from sce_sys\param.json, or from the path. */
static char *title_of(const char *root)
{
    char *t = param_title(root);
    return t ? t : text_title(root);
}

/* Same, but only from the folder itself: its param.json or its own name. */
static char *own_title(const char *root)
{
    char *t = param_title(root);
    return t ? t : text_title(pbase(root));
}

static char *safe_name(const char *s)
{
    size_t n = strlen(s);
    char *r = xmalloc(n + 8);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                 c == '_' || c == '.' || c == '(' || c == ')' || c == '[' || c == ']' || c == '+' || c == ',';
        if (c >= 0x80) {
            /* one '_' for a whole UTF-8 character */
            while (i + 1 < n && ((unsigned char)s[i + 1] & 0xC0) == 0x80) i++;
        }
        r[j++] = ok ? (char)c : '_';
    }
    r[j] = 0;
    if (j > 64) r[j = 64] = 0; /* keeps the paths of the ported game short */
    while (j && (r[j - 1] == ' ' || r[j - 1] == '.')) r[--j] = 0;
    size_t lead = 0;
    while (r[lead] == ' ') lead++;
    memmove(r, r + lead, strlen(r + lead) + 1);
    if (!r[0]) {
        free(r);
        return xstrdup("game");
    }
    /* Windows device names (also with an extension, like "aux.x") */
    static const char *const reserved[] = {"CON", "PRN", "AUX", "NUL", "COM0", "COM1", "COM2", "COM3", "COM4", "COM5",
                                           "COM6", "COM7", "COM8", "COM9", "LPT0", "LPT1", "LPT2", "LPT3", "LPT4",
                                           "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", NULL};
    size_t stem = strcspn(r, ".");
    while (stem && r[stem - 1] == ' ') stem--;
    for (int i = 0; reserved[i]; i++) {
        size_t k = 0;
        while (k < stem && reserved[i][k] && lower_ascii((unsigned char)r[k]) == lower_ascii((unsigned char)reserved[i][k])) k++;
        if (k == stem && reserved[i][k] == 0) {
            char *x = xfmt("_%s", r);
            free(r);
            return x;
        }
    }
    return r;
}

typedef struct {
    partlist parts; /* base first, then patches */
    char *name;
    char *exe;
} plan_t;

static void pl_free(partlist *l)
{
    for (size_t i = 0; i < l->n; i++) {
        free(l->v[i].root);
        free(l->v[i].exe);
        free(l->v[i].title);
        free(l->v[i].version);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}

static void plan_free(plan_t *p)
{
    pl_free(&p->parts);
    free(p->name);
    free(p->exe);
    memset(p, 0, sizeof *p);
}

static int cmp_part_root(const void *a, const void *b)
{
    return strcmp(((const part_t *)a)->root, ((const part_t *)b)->root);
}

/* Patches are applied oldest first: by contentVersion ("01.001.000"), then by folder. */
static int cmp_patch(const void *a, const void *b)
{
    const part_t *x = a, *y = b;
    int c = strcmp(x->version ? x->version : "", y->version ? y->version : "");
    return c ? c : strcmp(x->root, y->root);
}

static int plan_failed(plan_t *pl, partlist *patches, strlist *dirs)
{
    plan_free(pl);
    pl_free(patches);
    sl_free(dirs);
    return -1;
}

/* Works out what to port from the dragged items: the game folder (base) and
   any patch folders, in the order they are copied. */
static int make_plan(const char *anyps5, int nitems, char **items, plan_t *pl)
{
    memset(pl, 0, sizeof *pl);
    partlist patches = {0};
    strlist dirs = {0}; /* the dragged folders */
    char *ported = pjoin(anyps5, "ported");
    int bad = 0;
    for (int i = 0; i < nitems && !bad; i++) {
        char *item = p_fullpath(items[i]);
        pstat_t st;
        p_stat(item, &st);
        if (!st.exists) {
            say("Not found: \"%s\"\n", item);
            bad = 1;
        } else if (path_within(item, ported)) {
            say("\"%s\" is inside AnyPS5's ported folder.\nDrag the original game dump instead.\n", item);
            bad = 1;
        } else if (!st.is_dir) {
            pl_add(&pl->parts, pdir(item), xstrdup(pbase(item)));
        } else {
            int count = 0;
            find_games(item, 0, &pl->parts, &patches, anyps5, &count);
            sl_add(&dirs, item);
            item = NULL;
        }
        free(item);
    }
    free(ported);
    if (bad) return plan_failed(pl, &patches, &dirs);

    if (pl->parts.n == 0) {
        int patch_only = 0;
        for (size_t i = 0; i < patches.n; i++) {
            char *t = own_title(patches.v[i].root);
            if (t) patch_only = 1;
            free(t);
        }
        if (patch_only)
            say("Only a patch folder without the game's eboot.bin was found.\n"
                "Drag the game's folder together with its patch folder.\n");
        else
            say("No game found in \"%s\".\nThe folder must contain the game's eboot.bin (it may be in a subfolder).\n",
                dirs.n ? dirs.v[0] : "?");
        return plan_failed(pl, &patches, &dirs);
    }

    for (size_t i = 0; i < pl->parts.n; i++) {
        pl->parts.v[i].title = title_of(pl->parts.v[i].root);
        pl->parts.v[i].version = param_string(pl->parts.v[i].root, "contentVersion");
        pl->parts.v[i].patch = icontains(pbase(pl->parts.v[i].root), "patch");
    }
    if (pl->parts.n > 1) {
        const char *title = NULL;
        for (size_t i = 0; i < pl->parts.n; i++) {
            const char *t = pl->parts.v[i].title;
            if (!t) continue;
            if (title && strcmp(title, t) != 0) {
                say("These folders contain different games:\n");
                for (size_t k = 0; k < pl->parts.n; k++)
                    say("  %s  (%s)\n", pl->parts.v[k].root, pl->parts.v[k].title ? pl->parts.v[k].title : "unknown");
                say("Drag one game (or one game and its patch) at a time.\n");
                return plan_failed(pl, &patches, &dirs);
            }
            title = t;
        }
        size_t bases = 0;
        for (size_t i = 0; i < pl->parts.n; i++)
            if (!pl->parts.v[i].patch) bases++;
        if (bases > 1) {
            say("Found more than one copy of the game:\n");
            for (size_t k = 0; k < pl->parts.n; k++)
                if (!pl->parts.v[k].patch) say("  %s\n", pl->parts.v[k].root);
            say("Drag only the one you want (and optionally its patch folder).\n");
            return plan_failed(pl, &patches, &dirs);
        }
        qsort(pl->parts.v, pl->parts.n, sizeof(part_t), cmp_part_root);
        for (size_t i = 0; i < pl->parts.n; i++)
            if (!pl->parts.v[i].patch) {
                part_t b = pl->parts.v[i];
                memmove(pl->parts.v + 1, pl->parts.v, i * sizeof(part_t));
                pl->parts.v[0] = b;
                break;
            }
    }

    /* Patch folders without an executable (data-only updates) of this game. */
    {
        const char *game_title = pl->parts.v[0].title;
        for (size_t i = 0; i < patches.n; i++) {
            const char *root = patches.v[i].root;
            int skip = 0;
            for (size_t k = 0; k < pl->parts.n && !skip; k++)
                if (path_within(pl->parts.v[k].root, root) || path_within(root, pl->parts.v[k].root)) skip = 1;
            char *t = skip ? NULL : own_title(root);
            if (t && game_title && strcmp(t, game_title) == 0) {
                size_t before = pl->parts.n;
                pl_add(&pl->parts, xstrdup(root), NULL);
                if (pl->parts.n > before) {
                    pl->parts.v[before].title = t;
                    pl->parts.v[before].version = param_string(root, "contentVersion");
                    pl->parts.v[before].patch = 1;
                    t = NULL;
                }
            }
            free(t);
        }
        if (pl->parts.n > 2) qsort(pl->parts.v + 1, pl->parts.n - 1, sizeof(part_t), cmp_patch);
    }

    /* Every dragged folder must have contributed something. */
    for (size_t i = 0; i < dirs.n; i++) {
        int used = 0;
        for (size_t k = 0; k < pl->parts.n && !used; k++)
            if (path_within(pl->parts.v[k].root, dirs.v[i])) used = 1;
        if (!used) {
            say("Neither the game nor a patch of it was found in \"%s\".\n", dirs.v[i]);
            return plan_failed(pl, &patches, &dirs);
        }
    }

    const char *name_src;
    char *tmpname = NULL;
    if (nitems == 1) {
        char *item = p_fullpath(items[0]);
        pstat_t st;
        p_stat(item, &st);
        if (st.is_dir) {
            if (is_drive_root(item)) tmpname = xfmt("game-%c", item[0]);
            else tmpname = xstrdup(pbase(item));
        } else {
            char *parent = pdir(item);
            tmpname = is_drive_root(parent) ? xfmt("game-%c", parent[0]) : xstrdup(pbase(parent));
            free(parent);
        }
        free(item);
        name_src = tmpname;
    } else {
        name_src = pbase(pl->parts.v[0].root);
        if (is_drive_root(pl->parts.v[0].root)) {
            tmpname = xfmt("game-%c", pl->parts.v[0].root[0]);
            name_src = tmpname;
        }
    }
    pl->name = safe_name(name_src);
    free(tmpname);
    pl->exe = xstrdup(pl->parts.v[0].exe);
    pl_free(&patches);
    sl_free(&dirs);
    return 0;
}

static void print_plan(const plan_t *pl)
{
    for (size_t i = 0; i < pl->parts.n; i++) {
        say("%s%s", i == 0 ? "Game:        " : "Patch:       ", pl->parts.v[i].root);
        if (pl->parts.v[i].version) say("  (version %s)", pl->parts.v[i].version);
        say("\n");
    }
    if (pl->parts.v[0].title) say("Title ID:    %s\n", pl->parts.v[0].title);
    say("Executable:  %s\n", pl->exe);
}

/* ------------------------------------------------------------------------ */
/* port                                                                      */
/* ------------------------------------------------------------------------ */

static int cpu_is_intel(void)
{
    const char *id = getenv("PROCESSOR_IDENTIFIER");
    if (id && *id) return strstr(id, "GenuineIntel") != NULL;
#if defined(__x86_64__) || defined(__i386__)
    unsigned a, b, c, d;
    if (__get_cpuid(0, &a, &b, &c, &d)) return b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e;
#endif
    return 0;
}

static int copy_part(const char *src, const char *dst, const char *anyps5)
{
#ifdef _WIN32
    wchar_t sysdir[MAX_PATH + 1];
    UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);
    if (n == 0 || n > MAX_PATH) return -1;
    char *sys = u_of(sysdir);
    char *robocopy = pjoin(sys, "robocopy.exe");
    free(sys);
    char *from = is_drive_root(src) ? xfmt("%s.", src) : xstrdup(src);
    char *argv[] = {robocopy, from, (char *)dst, "/E", "/XJ", "/R:2", "/W:2", "/MT:8", "/NFL", "/NDL", "/NP", "/NJH",
                    "/A-:R", "/XD", (char *)anyps5, "System Volume Information", "$RECYCLE.BIN", "fakelib", NULL};
    int rc = p_run(argv);
    free(robocopy);
    free(from);
    if (rc < 0) {
        say("Could not start robocopy.\n");
        return -1;
    }
    return rc >= 8 ? -1 : 0;
#else
    /* Same result for tests on Linux: copy everything except links and fakelib. */
    pdir_t *d = p_opendir(src);
    if (!d) return -1;
    if (p_mkdirs(dst) != 0) {
        p_closedir(d);
        return -1;
    }
    char *name;
    pstat_t st;
    int bad = 0;
    while (p_readdir(d, &name, &st)) {
        char *a = pjoin(src, name), *b = pjoin(dst, name);
        if (st.is_link) {
        } else if (st.is_dir) {
            if (!ieq(name, "fakelib") && !path_within(a, anyps5)) bad |= copy_part(a, b, anyps5);
        } else {
            bad |= p_copyfile(a, b);
        }
        free(a);
        free(b);
        free(name);
    }
    p_closedir(d);
    return bad ? -1 : 0;
#endif
}

static int write_play_bat(const char *out, const char *name)
{
    char *path = pjoin(out, "Play.bat");
    char *text = xfmt("@echo off\r\n"
                      "rem Starts the game from this folder, which it uses as its drive\r\n"
                      "rem (saves, temporary files). Everything stays in this folder.\r\n"
                      "cd /d \"%%~dp0\"\r\n"
                      "if not exist \"%%~dp0tmp\\\" mkdir \"%%~dp0tmp\"\r\n"
                      "set \"TEMP=%%~dp0tmp\"\r\n"
                      "set \"TMP=%%~dp0tmp\"\r\n"
                      "\"%s.exe\" %%*\r\n"
                      "if not \"%%errorlevel%%\"==\"0\" pause\r\n",
                      name);
    int rc = write_file(path, (const uint8_t *)text, strlen(text));
    free(text);
    free(path);
    return rc;
}

static int do_port(const char *anyps5_in, int nitems, char **items)
{
    char *anyps5 = p_fullpath(anyps5_in);
    char *bin = pjoin(anyps5, "bin");
    char *relinker = pjoin(bin, RELINKER_NAME);
    char *libs = pjoin(bin, "libs");
    plan_t pl;
    char *out = NULL, *app0 = NULL, *exe_out = NULL, *app0_exe = NULL;
    tree_t tree;
    memset(&tree, 0, sizeof tree);
    int result = 1;

    if (!file_exists(relinker) || !dir_exists(libs)) {
        say("AnyPS5 is not built yet - run build-anyps5.bat first.\n");
        goto done_noplan;
    }
    if (make_plan(anyps5, nitems, items, &pl) != 0) goto done_noplan;
    print_plan(&pl);
    char *ported = pjoin(anyps5, "ported");
    out = pjoin(ported, pl.name);
    free(ported);
    app0 = pjoin(out, "app0");
    exe_out = xfmt("%s%c%s.exe", out, SEP, pl.name);
    say("Output:      %s\n", out);
    if (pl.parts.v[0].title && strncmp(pl.parts.v[0].title, "CUSA", 4) == 0)
        say("\nNOTE: %s is a PS4 title ID. AnyPS5 is made for PS5 games, so this will probably not work.\n", pl.parts.v[0].title);
    if (pl.parts.n > 1 && pl.parts.v[0].patch)
        say("\nNOTE: all folders look like patches; using the first one as the base game.\n");

    /* Check the executable before copying anything (the last part wins). */
    {
        char *src = NULL;
        for (size_t i = pl.parts.n; i-- > 0;) {
            char *p = pjoin(pl.parts.v[i].root, pl.exe);
            if (file_exists(p)) {
                src = p;
                break;
            }
            free(p);
        }
        if (!src) {
            say("\n%s was not found.\n", pl.exe);
            goto done;
        }
        finfo_t fi;
        classify_file(src, &fi);
        if (fi.kind != K_ELF && fi.kind != K_FSELF) {
            say("\n*** %s is %s", pl.exe, kind_text(fi.kind));
            if (fi.why[0]) say(": %s", fi.why);
            say(". ***\n");
            if (fi.kind == K_ENCRYPTED)
                say("An encrypted (retail) executable cannot be converted on a PC - it has to be\n"
                    "decrypted on the console when the game is dumped. Dumps made to run on a\n"
                    "jailbroken PS5 are usually decrypted and fake-signed, which works here.\n");
            else if (fi.kind == K_COMPRESSED)
                say("Compressed SELF files are not supported.\n");
            else if (fi.kind == K_BADSELF)
                say("The file may be damaged or incomplete - try copying the dump again.\n");
            free(src);
            goto done;
        }
        if (is_module_type(fi.e_type)) {
            say("\n*** %s is a library (module), not the game's executable. ***\n"
                "Drag the game's folder or its eboot.bin instead.\n", pl.exe);
            free(src);
            goto done;
        }
        say("%s is %s%s\n", pl.exe, kind_text(fi.kind), fi.kind == K_FSELF ? " - it will be converted to ELF." : ".");
        free(src);
    }

    if (p_mkdirs(app0) != 0) {
        say("Could not create %s\n", app0);
        goto done;
    }
    for (size_t i = 0; i < pl.parts.n; i++) {
        say("\n=== Copying %s into app0%s ===\n", i == 0 ? "the game files" : "the patch files",
            i == 0 ? " - big games take a while" : "");
        if (copy_part(pl.parts.v[i].root, app0, anyps5) != 0) {
            say("\n*** Copying failed - see the messages above. ***\n"
                "Check that there is enough free space and that the dump can be read.\n");
            goto done;
        }
    }

    /* The relinker needs one of its module folders, even when it is empty. */
    {
        char *a = pjoin(app0, "sce_module"), *b = pjoin(app0, "sce_modules"), *c = pjoin(app0, "prx");
        if (!dir_exists(a) && !dir_exists(b) && !dir_exists(c)) p_mkdirs(a);
        int both = dir_exists(a) && dir_exists(b);
        free(a);
        free(b);
        free(c);
        if (both) {
            say("\n*** The game has both a sce_module and a sce_modules folder; AnyPS5 accepts only one. ***\n");
            goto done;
        }
    }

    say("\n=== Converting fake-signed SELF files to ELF ===\n");
    tree.exe = pl.exe;
    walk_tree(&tree, app0, "", 0);
    say("Converted %d file(s).\n", tree.converted);
    sl_sort(&tree.module_dirs);
    for (size_t i = 0; i < tree.notes.n; i++) say("%s\n", tree.notes.v[i]);
    if (tree.problems.n) {
        say("These files could not be converted and will not work:\n");
        for (size_t i = 0; i < tree.problems.n && i < 20; i++) say("  %s\n", tree.problems.v[i]);
        if (tree.problems.n > 20) say("  ... and %d more\n", (int)(tree.problems.n - 20));
    }
    app0_exe = pjoin(app0, pl.exe);
    {
        finfo_t fi;
        classify_file(app0_exe, &fi);
        if (fi.kind != K_ELF) {
            say("\n*** %s could not be converted. ***\n", pl.exe);
            goto done;
        }
    }
    if (tree.module_dirs.n) {
        say("Extra module folders:");
        for (size_t i = 0; i < tree.module_dirs.n; i++) say(" %s", tree.module_dirs.v[i]);
        say("\n");
    }

    {
        int intel = cpu_is_intel();
        int have_dirs = tree.module_dirs.n > 0;
        int tries[4][2];
        int ntries = 0;
        /* Order: keep the module folders as long as possible. */
        int order[4][2] = {{1, 1}, {0, 1}, {1, 0}, {0, 0}};
        for (int i = 0; i < 4; i++) {
            if (order[i][0] && !intel) continue;
            if (order[i][1] && !have_dirs) continue;
            tries[ntries][0] = order[i][0];
            tries[ntries][1] = order[i][1];
            ntries++;
        }
        int done_ok = -1;
        for (int t = 0; t < ntries && done_ok < 0; t++) {
            size_t argc = 0;
            char **argv = xcalloc(8 + tree.module_dirs.n * 2, sizeof(char *));
            argv[argc++] = relinker;
            argv[argc++] = "--windows";
            if (tries[t][0]) argv[argc++] = "--to-intel";
            if (tries[t][1])
                for (size_t i = 0; i < tree.module_dirs.n; i++) {
                    argv[argc++] = "--module-dir";
                    argv[argc++] = tree.module_dirs.v[i];
                }
            argv[argc++] = app0_exe;
            argv[argc++] = exe_out;
            argv[argc] = NULL;
            if (t == 0) say("\n=== Converting the game with AnyPS5%s ===\n", tries[t][0] ? " (with --to-intel for Intel CPUs)" : "");
            else {
                say("\nThat failed - trying again ");
                if (!tries[t][0] && intel && !tries[t][1] && have_dirs) say("without --to-intel and without the extra module folders.\n");
                else if (!tries[t][0] && intel) say("without --to-intel.\n");
                else say("without the extra module folders.\n");
            }
            int rc = p_run(argv);
            free(argv);
            if (rc == 0) done_ok = t;
            else if (rc < 0) {
                say("Could not start %s\n", relinker);
                break;
            }
        }
        if (done_ok < 0) {
            say("\n*** The conversion failed - see the messages above. ***\n"
                "If AnyPS5 reports a missing feature, this game is not supported yet.\n"
                "Updating AnyPS5 (run build-anyps5.bat again) may help.\n");
            goto done;
        }
        if (intel && !tries[done_ok][0])
            say("\nNOTE: converted without --to-intel. AnyPS5 then handles AMD-only\n"
                "instructions while the game runs, which is slower.\n");
        if (have_dirs && !tries[done_ok][1])
            say("\nNOTE: the extra module folders could not be converted and were left out.\n"
                "The game may fail when it loads them.\n");
    }

    {
        char *out_libs = pjoin(out, "libs");
        p_rmtree(out_libs);
        if (p_mkdirs(out_libs) != 0) {
            say("Could not create %s\n", out_libs);
            free(out_libs);
            goto done;
        }
        pdir_t *d = p_opendir(libs);
        char *name;
        pstat_t st;
        int bad = d == NULL;
        while (d && p_readdir(d, &name, &st)) {
            if (!st.is_dir) {
                char *a = pjoin(libs, name), *b = pjoin(out_libs, name);
                if (p_copyfile(a, b) != 0) bad = 1;
                free(a);
                free(b);
            }
            free(name);
        }
        p_closedir(d);
        free(out_libs);
        if (bad) {
            say("\n*** Copying the AnyPS5 libraries failed. ***\n");
            goto done;
        }
    }
    if (write_play_bat(out, pl.name) != 0) {
        say("\n*** Play.bat could not be written. ***\n");
        goto done;
    }
    say("\nDone. To play, double-click:\n  %s%cPlay.bat\n", out, SEP);
    say("Saves, shaders and temporary files stay inside that folder.\n");
    result = 0;

done:
    plan_free(&pl);
done_noplan:
    sl_free(&tree.problems);
    sl_free(&tree.module_dirs);
    sl_free(&tree.notes);
    free(out);
    free(app0);
    free(exe_out);
    free(app0_exe);
    free(anyps5);
    free(bin);
    free(relinker);
    free(libs);
    return result;
}

/* ------------------------------------------------------------------------ */
/* What was dragged onto port-game.bat                                       */
/* ------------------------------------------------------------------------ */

/*
 * Windows puts quotes only around dragged paths that contain a space. cmd.exe
 * then cuts a path like C:\Games\Tom&Jerry at the & and drops ^ characters
 * before port-game.bat sees its arguments (%*). port-game.bat therefore also
 * passes cmd.exe's own command line (ANYPS5_CMDLINE, from cmdcmdline) and
 * its own path (ANYPS5_BAT). When it was started by dragging, that line is
 *     C:\Windows\system32\cmd.exe /c ""C:\...\port-game.bat" item item"
 * and the items are read from it exactly as Windows wrote them.
 * Returns 1 if the line started port-game.bat (items may then be empty).
 */
static int items_from_cmdline(const char *cmdline, const char *bat, strlist *items)
{
    if (!cmdline || !bat || !*bat) return 0;
    size_t bn = strlen(bat), cn = strlen(cmdline);
    const char *hit = NULL;
    for (size_t i = 0; !hit && i + bn <= cn; i++) {
        size_t k = 0;
        while (k < bn && lower_ascii((unsigned char)cmdline[i + k]) == lower_ascii((unsigned char)bat[k])) k++;
        if (k == bn) hit = cmdline + i;
    }
    if (!hit) return 0;
    int quoted = hit > cmdline && hit[-1] == '"' && hit[bn] == '"';
    int outer = quoted && hit - 1 > cmdline && hit[-2] == '"'; /* cmd /c "..." around everything */
    const char *p = hit + bn + (quoted ? 1 : 0);
    if (*p && *p != ' ' && *p != '\t' && !(outer && *p == '"')) return 0;
    const char *end = cmdline + cn;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    if (outer && end > p && end[-1] == '"') end--;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) break;
        char *tok = xmalloc((size_t)(end - p) + 1);
        size_t n = 0;
        int in_quotes = 0;
        for (; p < end && (in_quotes || (*p != ' ' && *p != '\t')); p++) {
            if (*p == '"') in_quotes = !in_quotes;
            else tok[n++] = *p;
        }
        tok[n] = 0;
        if (n) sl_add(items, tok);
        else free(tok);
    }
    return 1;
}

static int all_exist(int n, char **v)
{
    for (int i = 0; i < n; i++) {
        pstat_t st;
        p_stat(v[i], &st);
        if (!st.exists) return 0;
    }
    return n > 0;
}

/* The dragged items: from cmd.exe's command line when port-game.bat was
   started by dragging, otherwise the arguments. */
static void resolve_items(int argc, char **argv, strlist *store, int *nitems, char ***items)
{
    *nitems = argc;
    *items = argv;
    char *cmdline = p_getenv("ANYPS5_CMDLINE"), *bat = p_getenv("ANYPS5_BAT");
    if (items_from_cmdline(cmdline, bat, store) && store->n > 0) {
        int n = (int)store->n;
        if (all_exist(n, store->v) || !all_exist(argc, argv)) {
            *nitems = n;
            *items = store->v;
        }
    }
    free(cmdline);
    free(bat);
}

/* ------------------------------------------------------------------------ */
/* Stand-in for PowerShell while building AnyPS5                             */
/* ------------------------------------------------------------------------ */

/*
 * libusb (part of AnyPS5) writes its version into version_describe.h during
 * the build, with
 *     powershell -NoProfile -ExecutionPolicy Bypass -File .../gen-describe.ps1 SRCDIR OUT
 * gen-describe.ps1 only starts gen-describe.sh, which puts the text of
 * SRCDIR/.tarball-version into OUT as LIBUSB_DESCRIBE. PowerShell writes
 * cache files into the user's AppData, so build-anyps5.bat passes this
 * program instead (LIBUSB_POWERSHELL_EXECUTABLE), and it writes the same
 * header. Any other PowerShell script is refused with a clear message.
 */
static int powershell_standin(int argc, char **argv)
{
    const char *script = NULL;
    int first = argc;
    for (int i = 1; i + 1 < argc; i++)
        if (ieq(argv[i], "-File")) {
            script = argv[i + 1];
            first = i + 2;
            break;
        }
    if (!script || !ieq(pbase(script), "gen-describe.ps1")) {
        fprintf(stderr,
                "port-helper: this build step wants PowerShell to run %s.\n"
                "The portable AnyPS5 folder does not use PowerShell, and port-helper only\n"
                "stands in for it for libusb's gen-describe.ps1.\n",
                script ? script : "a command");
        return 1;
    }
    if (argc - first != 2) {
        fprintf(stderr, "port-helper: gen-describe.ps1 expects a source folder and an output file\n");
        return 1;
    }
    const char *srcdir = argv[first], *out = argv[first + 1];

    /* Like gen-describe.sh: the .tarball-version text without CR, LF and spaces. */
    char *tv = pjoin(srcdir, ".tarball-version");
    uint64_t got = 0, total = 0;
    uint8_t *b = read_file(tv, 4096, &got, &total);
    free(tv);
    char describe[4097];
    size_t n = 0;
    for (uint64_t i = 0; b && i < got; i++)
        if (b[i] != '\r' && b[i] != '\n' && b[i] != ' ') describe[n++] = (char)b[i];
    describe[n] = 0;
    free(b);
    if (n == 0) {
        fprintf(stderr, "port-helper: gen-describe: no .tarball-version in %s\n", srcdir);
        snprintf(describe, sizeof describe, "Unknown source");
    }

    char escaped[2 * sizeof describe];
    size_t e = 0;
    for (size_t i = 0; describe[i]; i++) {
        if (describe[i] == '\\' || describe[i] == '"') escaped[e++] = '\\';
        escaped[e++] = describe[i];
    }
    escaped[e] = 0;
    char *content = xfmt("/* Auto-generated by build-aux/gen-describe.sh. Do not edit. */\n"
                         "#ifndef LIBUSB_DESCRIBE\n"
                         "#define LIBUSB_DESCRIBE \"%s\"\n"
                         "#endif\n",
                         escaped);
    size_t clen = strlen(content);

    /* Only rewrite the header when it changes, so nothing is rebuilt needlessly. */
    uint8_t *old = read_file(out, 8192, &got, &total);
    int same = old && got == clen && total == clen && memcmp(old, content, clen) == 0;
    free(old);
    int rc = 0;
    if (!same) {
        char *dir = pdir(out);
        char *tmp = xfmt("%s.tmp", out);
        if (p_mkdirs(dir) != 0 || write_file(tmp, (const uint8_t *)content, clen) != 0 || p_replace(tmp, out) != 0) {
            fprintf(stderr, "port-helper: could not write %s\n", out);
            p_remove(tmp);
            rc = 1;
        }
        free(dir);
        free(tmp);
    }
    if (rc == 0) say("gen-describe (port-helper in place of PowerShell): LIBUSB_DESCRIBE \"%s\"\n", escaped);
    free(content);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                  */
/* ------------------------------------------------------------------------ */

static void usage(void)
{
    say("port-helper %d - part of the portable AnyPS5 folder, used by port-game.bat.\n"
        "Drag a game's dump folder onto port-game.bat instead of running this directly.\n"
        "\n"
        "  port-helper port <AnyPS5 folder> <dump folder or eboot.bin> [patch folder]\n"
        "  port-helper plan <AnyPS5 folder> <dump folder or eboot.bin> [patch folder]\n"
        "  port-helper info <file>...\n"
        "  port-helper convert <SELF file> <ELF file>\n"
        "  port-helper fixtree <folder> [executable name]\n"
        "  port-helper version <number>\n",
        HELPER_VERSION);
}

static int real_main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }
    const char *cmd = argv[1];
    if (cmd[0] == '-') return powershell_standin(argc, argv); /* called as PowerShell */
    if (ieq(cmd, "version")) return argc >= 3 && atoi(argv[2]) == HELPER_VERSION ? 0 : 1;
    if (ieq(cmd, "dragged")) { /* exit 0 if port-game.bat was started by dragging (or double-clicking) */
        strlist items = {0};
        char *cmdline = p_getenv("ANYPS5_CMDLINE"), *bat = p_getenv("ANYPS5_BAT");
        int started = items_from_cmdline(cmdline, bat, &items);
        free(cmdline);
        free(bat);
        sl_free(&items);
        return started ? 0 : 1;
    }
    if (ieq(cmd, "info") && argc >= 3) {
        for (int i = 2; i < argc; i++) {
            finfo_t fi;
            classify_file(argv[i], &fi);
            say("%s: %s", argv[i], kind_text(fi.kind));
            if (fi.kind == K_ELF || fi.kind == K_FSELF)
                say(", %s", fi.machine != 0x3E ? "not x86-64" : is_module_type(fi.e_type) ? "module" : "program");
            if (fi.kind == K_FSELF || fi.kind == K_ENCRYPTED || fi.kind == K_COMPRESSED || fi.kind == K_BADSELF)
                say(", %s magic", fi.ps5 ? "PS5" : "PS4");
            if (fi.why[0]) say(" (%s)", fi.why);
            say("\n");
        }
        return 0;
    }
    if (ieq(cmd, "convert") && argc == 4) {
        char why[200];
        finfo_t fi;
        classify_file(argv[2], &fi);
        if (fi.kind != K_FSELF) {
            say("%s: %s%s%s%s - nothing converted.\n", argv[2], kind_text(fi.kind), fi.why[0] ? " (" : "", fi.why,
                fi.why[0] ? ")" : "");
            return fi.kind == K_ENCRYPTED ? 2 : fi.kind == K_COMPRESSED ? 3 : fi.kind == K_BADSELF ? 4 : 5;
        }
        if (convert_self(argv[2], argv[3], why, sizeof why) != 0) {
            say("%s: %s\n", argv[2], why);
            return 1;
        }
        say("%s: converted to ELF.\n", argv[2]);
        return 0;
    }
    if (ieq(cmd, "fixtree") && (argc == 3 || argc == 4)) {
        tree_t t;
        memset(&t, 0, sizeof t);
        t.exe = argc == 4 ? argv[3] : "eboot.bin";
        char *root = p_fullpath(argv[2]);
        walk_tree(&t, root, "", 0);
        free(root);
        sl_sort(&t.module_dirs);
        say("converted=%d\n", t.converted);
        for (size_t i = 0; i < t.module_dirs.n; i++) say("module-dir=%s\n", t.module_dirs.v[i]);
        for (size_t i = 0; i < t.notes.n; i++) say("note=%s\n", t.notes.v[i]);
        for (size_t i = 0; i < t.problems.n; i++) say("problem=%s\n", t.problems.v[i]);
        int bad = t.problems.n > 0;
        sl_free(&t.module_dirs);
        sl_free(&t.notes);
        sl_free(&t.problems);
        return bad ? 2 : 0;
    }
    if ((ieq(cmd, "plan") || ieq(cmd, "port")) && argc >= 3) {
        strlist store = {0};
        int nitems;
        char **items;
        resolve_items(argc - 3, argv + 3, &store, &nitems, &items);
        int rc = 1;
        if (nitems == 0) {
            say("Drag a game's dump folder (or its eboot.bin) onto port-game.bat.\n");
        } else if (ieq(cmd, "port")) {
            rc = do_port(argv[2], nitems, items);
        } else {
            plan_t pl;
            char *anyps5 = p_fullpath(argv[2]);
            if (make_plan(anyps5, nitems, items, &pl) == 0) {
                say("NAME=%s\nEXE=%s\n", pl.name, pl.exe);
                for (size_t i = 0; i < pl.parts.n; i++)
                    say("PART=%s|%s|%s\n", pl.parts.v[i].root, pl.parts.v[i].title ? pl.parts.v[i].title : "-",
                        i == 0 ? "base" : "patch");
                plan_free(&pl);
                rc = 0;
            }
            free(anyps5);
        }
        sl_free(&store);
        return rc;
    }
    usage();
    return 1;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wargv)
{
    char **argv = xcalloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) argv[i] = u_of(wargv[i]);
    int rc = real_main(argc, argv);
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return rc;
}
#else
int main(int argc, char **argv)
{
    return real_main(argc, argv);
}
#endif
