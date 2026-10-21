/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * update.c - is there a newer release, and fetch and install it. Shared by
 * Laine's Amiga projects: everything product-specific (name, repository,
 * archive and installer names, version, where the settings live) comes from
 * upd_cfg.h and the product's upd_load_prefs(). Started as PrismRTG's
 * PrismUpdate.
 *
 *   <tool> [GET] [FETCH] [SOURCE=GITHUB|MIRROR|BOTH] [MIRROR=https://...] [QUIET]
 *
 * Needs a running TCP/IP stack (bsdsocket.library) and AmiSSL 5
 * (amisslmaster.library), or anything that answers to the same API, such
 * as an SSL offload. Everything is fetched over HTTPS. The client keeps to
 * the basic SSL calls (context, SNI, connect, read, write) so an offload
 * that implements those works too.
 *
 * Sources: GitHub's release API (a beta looks at the newest release
 * including pre-releases, a final release only at the newest final one), or
 * a mirror. MIRROR= is either a directory with latest.txt, whose first line
 * is "<version> <archive>" (the archive's name in the same directory, or a
 * full https URL), or a Forgejo/Gitea repository's address
 * (https://host/owner/repo), whose release API answers like GitHub's.
 *
 * GET downloads the archive to T:, unpacks it with LhA and runs its
 * installer with the Installer: the usual questions follow. FETCH only
 * downloads and unpacks it.
 *
 * Exit codes: 0 up to date, 5 a newer release exists (printed), 10 could
 * not check (no stack, no AmiSSL, no connection), 20 a fetch or install
 * failed. The last line printed is the result, which a Prefs program shows.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <amissl/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "upd_cfg.h"

static const char version[] __attribute__((used)) = "$VER: " UPD_TOOL " " UPD_VERSION " (" UPD_RELDATE ")";

#define TEMPLATE "GET/S,FETCH/S,SOURCE/K,MIRROR/K,QUIET/S,STK/S"
enum { A_GET, A_FETCH, A_SOURCE, A_MIRROR, A_QUIET, A_STK, A_COUNT };

#define API_HOST  "api.github.com"
#define AGENT     UPD_TOOL "/" UPD_VERSION
#define ARCHIVE   "T:" UPD_PRODUCT "-update.lha"
#define UNPACKED  "T:" UPD_PRODUCT "-update"
#define SCRIPT    "T:" UPD_PRODUCT "-update.script"
#define MAX_JSON  (512 * 1024)
#define MAX_LHA   (4L * 1024 * 1024)
#define NEED_STACK 65536

struct Library *SocketBase, *AmiSSLMasterBase, *AmiSSLBase, *AmiSSLExtBase;
static BOOL quiet;
static const char *why = "";          /* the reason a check failed, for the last line */
static char whybuf[96];

static void say(const char *fmt, ...)
{
    va_list ap;
    if (quiet)
        return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* ---- versions --------------------------------------------------------- */

/* "1.1b5", "v1.1-beta5", "1.0.2", "v1.1" -> major, minor, patch, beta (0 =
 * a final release, which ranks above every beta of the same number) */
struct Ver { int major, minor, patch, beta; };

static BOOL ver_parse(const char *s, struct Ver *v)
{
    const char *p = s;
    memset(v, 0, sizeof(*v));
    if (*p == 'v' || *p == 'V')
        p++;
    if (*p < '0' || *p > '9')
        return FALSE;
    v->major = strtol(p, (char **)&p, 10);
    if (*p == '.') v->minor = strtol(p + 1, (char **)&p, 10);
    if (*p == '.') v->patch = strtol(p + 1, (char **)&p, 10);
    while (*p == '-' || *p == '_' || *p == ' ')
        p++;
    if (*p == 'b' || *p == 'B') {
        p++;
        if (!strncmp(p, "eta", 3)) p += 3;
        while (*p == '-' || *p == ' ') p++;
        v->beta = strtol(p, NULL, 10);
        if (v->beta <= 0) v->beta = 1;
    }
    return TRUE;
}

static int ver_cmp(const struct Ver *a, const struct Ver *b)
{
    int ra = a->beta ? a->beta : 100000, rb = b->beta ? b->beta : 100000;
    if (a->major != b->major) return a->major - b->major;
    if (a->minor != b->minor) return a->minor - b->minor;
    if (a->patch != b->patch) return a->patch - b->patch;
    return ra - rb;
}

static void ver_str(const struct Ver *v, char *out)
{
    if (v->patch) sprintf(out, "%d.%d.%d", v->major, v->minor, v->patch);
    else          sprintf(out, "%d.%d", v->major, v->minor);
    if (v->beta)  sprintf(out + strlen(out), "b%d", v->beta);
}

/* ---- HTTPS ------------------------------------------------------------- */

/* GitHub's release files redirect to a signed address of well over a
 * thousand characters: room for it */
#define URL_MAX 2048
struct Url { char host[128]; char path[URL_MAX]; int port; };

static BOOL url_parse(const char *u, struct Url *o)
{
    const char *p, *s;
    int n;
    if (strncmp(u, "https://", 8))
        return FALSE;
    p = u + 8;
    s = strchr(p, '/');
    n = s ? s - p : (int)strlen(p);
    if (n <= 0 || n >= (int)sizeof(o->host))
        return FALSE;
    memcpy(o->host, p, n);
    o->host[n] = 0;
    o->port = 443;
    if (strchr(o->host, ':')) {
        char *c = strchr(o->host, ':');
        *c = 0;
        o->port = atoi(c + 1);
    }
    if (!s || strlen(s) >= sizeof(o->path))
        strcpy(o->path, "/");
    else
        strcpy(o->path, s);
    return TRUE;
}

struct Fetch {
    LONG   status;               /* HTTP status                           */
    char   location[URL_MAX];    /* a redirect's target                   */
    UBYTE *body;                 /* when fetched to memory                */
    ULONG  len;
    BPTR   file;                 /* when fetched to a file                */
    ULONG  limit;
};

static SSL_CTX *ctx;
static BOOL verify;                      /* the peer's certificate is checked */

/* Ctrl-C in a Shell, or the Prefs window closing: stop waiting */
static BOOL broken(void)
{
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

static LONG tcp_connect(const char *host, int port)
{
    struct hostent *he;
    struct sockaddr_in sa;
    LONG s;
    ULONG tv[2] = { 30, 0 };                 /* a timeval: 30 s, 0 us */

    if (!(he = gethostbyname((STRPTR)host))) {
        say(UPD_TOOL ": could not look up %s (DNS)\n", host);
        sprintf(whybuf, ": %.60s not found (DNS)", host);
        why = whybuf;
        return -1;
    }
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        say(UPD_TOOL ": no socket (errno %ld)\n", (long)Errno());
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        say(UPD_TOOL ": could not connect to %s port %d (errno %ld)\n", host, port, (long)Errno());
        sprintf(whybuf, ": no connection to %.60s", host);
        why = whybuf;
        CloseSocket(s);
        return -1;
    }
    return s;
}

static BOOL ssl_write_all(SSL *ssl, const char *p, int n)
{
    while (n > 0) {
        int r = SSL_write(ssl, p, n);
        if (r <= 0 || broken())
            return FALSE;
        p += r;
        n -= r;
    }
    return TRUE;
}

/* a line of the header block: into buf, or FALSE at end of input */
static BOOL ssl_line(SSL *ssl, char *buf, int n)
{
    int i = 0;
    for (;;) {
        char c;
        if (SSL_read(ssl, &c, 1) != 1 || broken())
            return FALSE;
        if (c == '\n')
            break;
        if (c != '\r' && i < n - 1)
            buf[i++] = c;
    }
    buf[i] = 0;
    return TRUE;
}

static BOOL fetch_put(struct Fetch *f, const UBYTE *p, ULONG n)
{
    if (f->len + n > f->limit)
        return FALSE;
    if (f->file) {
        if (Write(f->file, (APTR)p, n) != (LONG)n)
            return FALSE;
    } else
        memcpy(f->body + f->len, p, n);
    f->len += n;
    return TRUE;
}

/* GET one URL; body to f->body (allocated here, f->limit bytes at most)
 * or to f->file. A 3xx fills f->location. Returns FALSE on a transport
 * failure; the HTTP status is in f->status. */
static BOOL fetch_once(const struct Url *u, struct Fetch *f)
{
    SSL *ssl;
    LONG s;
    static char line[URL_MAX], req[URL_MAX + 256];
    BOOL ok = FALSE, chunked = FALSE;
    LONG clen = -1;

    f->status = 0;
    f->location[0] = 0;
    if ((s = tcp_connect(u->host, u->port)) < 0)
        return FALSE;
    if (!(ssl = SSL_new(ctx))) {
        CloseSocket(s);
        return FALSE;
    }
    SSL_set_tlsext_host_name(ssl, u->host);
    SSL_set_fd(ssl, s);
    if (SSL_connect(ssl) != 1) {
        if (verify) {
            /* the handshake with the certificate check on failed: most
             * likely no certificates to check against (AmiSSL:Certs is
             * not there). Once more without, and the report says so. */
            say(UPD_TOOL ": note: the certificate of %s could not be checked "
                "(no AmiSSL:Certs?); the connection is not verified\n", u->host);
            verify = FALSE;
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
            SSL_free(ssl);
            CloseSocket(s);
            return fetch_once(u, f);
        }
        say(UPD_TOOL ": the TLS handshake with %s failed\n", u->host);
        sprintf(whybuf, ": the TLS handshake with %.60s failed", u->host);
        why = whybuf;
        goto out;
    }
    sprintf(req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " AGENT "\r\n"
                 "Accept: */*\r\nConnection: close\r\n\r\n", u->path, u->host);
    if (!ssl_write_all(ssl, req, strlen(req)))
        goto out;
    if (!ssl_line(ssl, line, sizeof(line)) || strncmp(line, "HTTP/", 5) || strlen(line) < 12)
        goto out;
    f->status = atoi(line + 9);
    while (ssl_line(ssl, line, sizeof(line)) && line[0]) {
        if (!strnicmp(line, "Content-Length:", 15))
            clen = atol(line + 15);
        else if (!strnicmp(line, "Transfer-Encoding:", 18) && strstr(line + 18, "hunked"))
            chunked = TRUE;
        else if (!strnicmp(line, "Location:", 9)) {
            const char *p = line + 9;
            while (*p == ' ') p++;
            strncpy(f->location, p, sizeof(f->location) - 1);
        }
    }
    if (f->status / 100 == 3) {
        ok = TRUE;
        goto out;
    }
    if (!f->file && !(f->body = AllocVec(f->limit + 1, MEMF_ANY)))
        goto out;
    f->len = 0;
    if (chunked) {
        for (;;) {
            LONG n;
            if (!ssl_line(ssl, line, sizeof(line)))
                goto out;
            n = strtol(line, NULL, 16);
            if (n <= 0)
                break;
            while (n > 0) {
                UBYTE buf[2048];
                int r = SSL_read(ssl, buf, n < (LONG)sizeof(buf) ? n : (LONG)sizeof(buf));
                if (r <= 0 || broken() || !fetch_put(f, buf, r))
                    goto out;
                n -= r;
            }
            ssl_line(ssl, line, sizeof(line));      /* the chunk's CRLF */
        }
        ok = TRUE;
    } else {
        for (;;) {
            UBYTE buf[2048];
            int r = SSL_read(ssl, buf, sizeof(buf));
            if (r <= 0)
                break;
            if (broken() || !fetch_put(f, buf, r))
                goto out;
            if (clen >= 0 && (LONG)f->len >= clen)
                break;
        }
        ok = clen < 0 || (LONG)f->len == clen;
    }
    if (f->body)
        f->body[f->len] = 0;
out:
    SSL_shutdown(ssl);
    SSL_free(ssl);
    CloseSocket(s);
    return ok;
}

/* GET with up to four redirects (GitHub's release files are one) */
static BOOL fetch(const char *url, struct Fetch *f)
{
    static struct Url u;
    static char cur[URL_MAX];
    int hops;
    strncpy(cur, url, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = 0;
    for (hops = 0; hops < 5; hops++) {
        if (!url_parse(cur, &u)) {
            say(UPD_TOOL ": not an https URL: %s\n", cur);
            return FALSE;
        }
        if (!fetch_once(&u, f)) {
            say(UPD_TOOL ": could not fetch from %s (status %ld)\n", u.host, f->status);
            return FALSE;
        }
        if (f->status / 100 != 3)
            break;
        if (!f->location[0])
            return FALSE;
        if (f->location[0] == '/')
            sprintf(cur, "https://%s%s", u.host, f->location);
        else {
            strncpy(cur, f->location, sizeof(cur) - 1);
            cur[sizeof(cur) - 1] = 0;
        }
    }
    if (f->status != 200) {
        say(UPD_TOOL ": %s answered %ld\n", u.host, f->status);
        /* GitHub answers 403 or 429 once an address has made its 60 API
         * calls in the hour that it allows without an account */
        if ((f->status == 403 || f->status == 429) && !strcmp(u.host, "api.github.com"))
            why = ": GitHub's hourly limit is used up, try again later";
        return FALSE;
    }
    return TRUE;
}

/* ---- the sources ------------------------------------------------------- */

struct Latest { char tag[48]; char url[URL_MAX]; const char *from; };

/* the value of the first "key":"..." at or after p */
/* "key": "value" - with or without white space around the colon: GitHub's
 * API began answering pretty-printed ("tag_name": "v1.0.2") in October 2026,
 * and looking for "tag_name":" found nothing, so every check through GitHub
 * failed ("could not check for an update"). Forgejo still answers compact. */
static const char *json_str(const char *p, const char *key, char *out, int n)
{
    char pat[48];
    const char *q, *e;
    sprintf(pat, "\"%s\"", key);
    while ((q = strstr(p, pat)) != NULL) {
        p = q + strlen(pat);                 /* the next search starts past it */
        q = p;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            q++;
        if (*q++ != ':')
            continue;                        /* a value that is this word, not a key */
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            q++;
        if (*q++ != '"')
            continue;                        /* not a string */
        if (!(e = strchr(q, '"')) || e - q >= n)
            return NULL;
        memcpy(out, q, e - q);
        out[e - q] = 0;
        return e;
    }
    return NULL;
}

/* a release API that answers like GitHub's: base = "https://api.github.com/repos/
 * owner/repo" or a Forgejo/Gitea "https://host/api/v1/repos/owner/repo" */
static BOOL from_api(const char *base, const char *from, const struct Ver *mine, struct Latest *l)
{
    struct Fetch f;
    const char *p, *end;
    static char url[URL_MAX];
    static char asset[URL_MAX];
    BOOL ok = FALSE;

    memset(&f, 0, sizeof(f));
    f.limit = MAX_JSON;
    /* a beta sees pre-releases (the highest version of the newest few);
     * a final release only the newest final one */
    sprintf(url, "%s/releases%s", base, mine->beta ? "?limit=5&per_page=5" : "/latest");
    if (!fetch(url, &f))
        goto out;
    p = (const char *)f.body;
    if (!(p = json_str(p, "tag_name", l->tag, sizeof(l->tag))))
        goto out;
    if (mine->beta) {
        /* the list does not start with the newest: GitHub puts the release
         * marked Latest first (1.1 above 1.2 beta 1), so the highest
         * version of the list is taken */
        struct Ver best, v;
        const char *q = p;
        static char tag[48];
        ver_parse(l->tag, &best);
        while ((q = json_str(q, "tag_name", tag, sizeof(tag))) != NULL)
            if (ver_parse(tag, &v) && ver_cmp(&v, &best) > 0) {
                best = v;
                strcpy(l->tag, tag);
                p = q;
            }
    }
    /* the release archive among the assets: <product>-<ver>.lha; stop at
     * the next release's tag */
    end = strstr(p, "\"tag_name\":");
    l->url[0] = 0;
    while ((p = json_str(p, "browser_download_url", asset, sizeof(asset))) && (!end || p < end)) {
        const char *nm = strrchr(asset, '/');
        if (nm && !strncmp(nm + 1, UPD_ASSET, sizeof(UPD_ASSET) - 1) && strstr(nm, ".lha")) {
            strcpy(l->url, asset);
            break;
        }
    }
    l->from = from;
    ok = TRUE;
out:
    if (f.body) FreeVec(f.body);
    return ok;
}

static BOOL from_github(const struct Ver *mine, struct Latest *l)
{
    return from_api("https://" API_HOST "/repos/" UPD_REPO, "GitHub", mine, l);
}

/* https://host/owner/repo (a Forgejo or Gitea repository): its release API */
static BOOL forge_base(const char *mirror, char *base, int n)
{
    static struct Url u;
    const char *p;
    int parts = 0;
    if (!url_parse(mirror, &u) || strstr(u.path, ".txt"))
        return FALSE;
    for (p = u.path; *p; p++)
        if (*p == '/' && p[1] && p[1] != '/')
            parts++;
    if (parts != 2 || strlen(mirror) + 32 > (size_t)n)
        return FALSE;
    sprintf(base, "https://%s/api/v1/repos%s", u.host, u.path);
    if (base[strlen(base) - 1] == '/')
        base[strlen(base) - 1] = 0;
    return TRUE;
}

static BOOL from_mirror(const char *mirror, const struct Ver *mine, struct Latest *l)
{
    struct Fetch f;
    static char url[URL_MAX];
    char *p, *e;
    BOOL ok = FALSE;

    /* an owner/repository path may be a Forgejo/Gitea repository; if its
     * release API doesn't answer, it is a plain directory after all */
    if (mirror[0] && forge_base(mirror, url, sizeof(url)) && from_api(url, "the mirror", mine, l))
        return TRUE;
    if (!mirror[0]) {
        say(UPD_TOOL ": no mirror is set (MIRROR= in the prefs)\n");
        return FALSE;
    }
    if (strlen(mirror) > URL_MAX - 64) {
        say(UPD_TOOL ": the mirror address is too long\n");
        return FALSE;
    }
    memset(&f, 0, sizeof(f));
    f.limit = 4096;
    sprintf(url, "%s%slatest.txt", mirror, mirror[strlen(mirror) - 1] == '/' ? "" : "/");
    if (!fetch(url, &f))
        goto out;
    /* "<version> <archive>", the archive a name in the mirror's directory
     * or a full https URL; a newline is not required after it */
    p = (char *)f.body;
    while (*p == ' ' || *p == '\n' || *p == '\r') p++;
    if (!(e = strpbrk(p, " \r\n")))
        goto out;
    if (e - p >= (int)sizeof(l->tag))
        goto out;
    memcpy(l->tag, p, e - p);
    l->tag[e - p] = 0;
    while (*e == ' ') e++;
    p = e;
    if ((e = strpbrk(p, " \r\n")))
        *e = 0;
    if (!*p || strlen(p) + strlen(mirror) + 2 > sizeof(l->url)) {
        say(UPD_TOOL ": the mirror's latest.txt names no archive, or too long a one\n");
        goto out;
    }
    if (!strncmp(p, "https://", 8))
        strcpy(l->url, p);
    else
        sprintf(l->url, "%s%s%s", mirror, mirror[strlen(mirror) - 1] == '/' ? "" : "/", p);
    l->from = "the mirror";
    ok = TRUE;
out:
    if (f.body) FreeVec(f.body);
    return ok;
}

/* ---- install ----------------------------------------------------------- */

static BOOL run(const char *cmd)
{
    LONG rc = SystemTags((STRPTR)cmd, SYS_Input, (ULONG)Input(), SYS_Output, (ULONG)Output(), TAG_DONE);
    return rc == 0;
}

static BOOL exists(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) UnLock(l);
    return l != 0;
}

/* the drawer the archive unpacked the installer into, or FALSE */
static BOOL find_installer(char *dir, int n)
{
    BPTR l;
    struct FileInfoBlock *fib;
    BOOL ok = FALSE;
    sprintf(dir, UNPACKED "/" UPD_INSTALLER);
    if (exists(dir)) {
        strcpy(dir, UNPACKED);
        return TRUE;
    }
    if (!(fib = AllocDosObject(DOS_FIB, NULL)))
        return FALSE;
    if ((l = Lock(UNPACKED, ACCESS_READ))) {
        if (Examine(l, fib))
            while (!ok && ExNext(l, fib)) {
                if (fib->fib_DirEntryType > 0) {
                    char t[300];
                    sprintf(t, UNPACKED "/%s/" UPD_INSTALLER, fib->fib_FileName);
                    if (exists(t)) {
                        sprintf(dir, UNPACKED "/%s", fib->fib_FileName);
                        ok = TRUE;
                    }
                }
            }
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    return ok;
}

static BOOL get_and_install(const struct Latest *l, BOOL install)
{
    struct Fetch f;
    char dir[300], script[700];
    const char *installer = exists("SYS:System/Installer") ? "SYS:System/Installer" :
                            exists("SYS:Utilities/Installer") ? "SYS:Utilities/Installer" : "Installer";
    BPTR out;

    if (!l->url[0]) {
        say(UPD_TOOL ": the release has no " UPD_PRODUCT " archive to download\n");
        return FALSE;
    }
    say(UPD_TOOL ": fetching %s\n", l->url);
    memset(&f, 0, sizeof(f));
    f.limit = MAX_LHA;
    if (!(f.file = Open(ARCHIVE, MODE_NEWFILE))) {
        say(UPD_TOOL ": can't write " ARCHIVE "\n");
        return FALSE;
    }
    if (!fetch(l->url, &f)) {
        Close(f.file);
        DeleteFile(ARCHIVE);
        return FALSE;
    }
    Close(f.file);
    say(UPD_TOOL ": %lu bytes, unpacking\n", f.len);
    run("Delete >NIL: " UNPACKED " ALL QUIET");
    if (!run("C:LhA >NIL: x -q " ARCHIVE " " UNPACKED "/")) {
        say(UPD_TOOL ": LhA could not unpack the archive (is C:LhA there?)\n");
        return FALSE;
    }
    if (!find_installer(dir, sizeof(dir))) {
        say(UPD_TOOL ": no " UPD_INSTALLER " in the archive\n");
        return FALSE;
    }
    if (!install) {
        say(UPD_TOOL ": unpacked into %s; its " UPD_INSTALLER " installs it\n", dir);
        return TRUE;
    }
    /* the Installer with the archive's drawer as the current one: its
     * (source) paths are relative to it */
    if (!(out = Open(SCRIPT, MODE_NEWFILE)))
        return FALSE;
    /* MINUSER AVERAGE as the icon has it: Novice would answer the
     * installer's questions by itself */
    sprintf(script, "CD \"%s\"\n%s " UPD_INSTALLER " APPNAME " UPD_PRODUCT " MINUSER AVERAGE\n", dir, installer);
    Write(out, script, strlen(script));
    Close(out);
    say(UPD_TOOL ": starting the installer (%s)\n", installer);
    return run("Execute " SCRIPT);
}

/* ---- main --------------------------------------------------------------- */

/* AmiSSL wants more stack than a Shell gives (4 KB by default): the program
 * runs on its own 64 KB. It used to start itself again through SystemTags()
 * with NP_StackSize - but that sets the new Shell's stack, and the command it
 * runs gets the Shell's DEFAULT stack, 4 KB again; told not to check twice
 * (STK), the TLS handshake then overran it and corrupted memory: gurus of
 * every kind, mostly on the second or third run (real AmiSSL 5.27, 68030,
 * 2026-10-21; the AmiSSL-Tunnel shim needs little stack and never showed it).
 * StackSwap() here instead; across it only globals are used. STK is still
 * accepted, and ignored. */
static struct StackSwapStruct swap;
static APTR swapMem;
static int swapRc;

static int upd_main(void);

int main(void)
{
    struct Task *me = FindTask(NULL);
    if ((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower >= NEED_STACK - 4096)
        return upd_main();
    if (!(swapMem = AllocVec(NEED_STACK, MEMF_ANY))) {
        printf(UPD_TOOL ": not enough memory for its stack\n");
        return RETURN_FAIL;
    }
    swap.stk_Lower = swapMem;
    swap.stk_Upper = (ULONG)swapMem + NEED_STACK;
    swap.stk_Pointer = (APTR)swap.stk_Upper;
    StackSwap(&swap);
    swapRc = upd_main();
    StackSwap(&swap);
    FreeVec(swapMem);
    return swapRc;
}

static int upd_main(void)
{
    LONG args[A_COUNT] = { 0 };
    struct RDArgs *rda;
    static char mirror_buf[UPD_MIRROR_MAX + 1];
    struct Ver mine, theirs;
    struct Latest l;
    char have[24], want[24];
    UBYTE source;
    const char *mirror;
    LONG rc = RETURN_ERROR;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), UPD_TOOL);
        return RETURN_FAIL;
    }
    quiet = args[A_QUIET] != 0;

    source = upd_load_prefs(mirror_buf, sizeof(mirror_buf));
    if (args[A_SOURCE]) {
        const char *s = (const char *)args[A_SOURCE];
        source = !stricmp(s, "GITHUB") ? UPD_GITHUB : !stricmp(s, "MIRROR") ? UPD_MIRROR :
                 !stricmp(s, "BOTH") ? UPD_BOTH : UPD_OFF;
    }
    mirror = args[A_MIRROR] ? (const char *)args[A_MIRROR] : mirror_buf;
    if (source == UPD_OFF) {
        say(UPD_TOOL ": update checks are off (UPDATES= in " UPD_PREFS_NAME ", or SOURCE=)\n");
        goto done;
    }
    if (source == UPD_MIRROR && !mirror[0]) {
        say(UPD_TOOL ": no mirror is set (MIRROR= in the prefs)\n");
        goto done;
    }
    ver_parse(UPD_VERSION, &mine);

    if (!(SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4))) {
        say(UPD_TOOL ": no TCP/IP stack is running (bsdsocket.library)\n");
        goto done;
    }
    if (!(AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library", AMISSLMASTER_MIN_VERSION))) {
        say(UPD_TOOL ": needs AmiSSL 5 (amisslmaster.library) for HTTPS\n");
        goto done;
    }
    /* the OpenSSL 3 baseline, not the headers' newest: any AmiSSL 5 (or
     * an offload) answers to it, and the calls here are the basic ones */
    if (OpenAmiSSLTags(AMISSL_V3xx,
                       AmiSSL_UsesOpenSSLStructs, FALSE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                       AmiSSL_SocketBase, (ULONG)SocketBase,
                       AmiSSL_ErrNoPtr, (ULONG)&errno,
                       TAG_DONE) != 0) {
        say(UPD_TOOL ": AmiSSL would not start\n");
        AmiSSLBase = NULL;
        goto done;
    }
    if (!(ctx = SSL_CTX_new(TLS_client_method()))) {
        say(UPD_TOOL ": no SSL context\n");
        goto done;
    }
    /* the server's certificate against the ones AmiSSL ships (AmiSSL:Certs);
     * an offload checks on its own side. The host name is not matched
     * against the certificate (an offload has no call for it). */
    if (SSL_CTX_set_default_verify_paths(ctx) == 1) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        verify = TRUE;
    }

    memset(&l, 0, sizeof(l));
    if ((source == UPD_GITHUB || source == UPD_BOTH) && from_github(&mine, &l))
        ;
    else if ((source == UPD_MIRROR || source == UPD_BOTH) && from_mirror(mirror, &mine, &l))
        ;
    else {
        say(UPD_TOOL ": could not check for an update%s\n", why);
        goto done;
    }
    if (!ver_parse(l.tag, &theirs)) {
        say(UPD_TOOL ": %s names a release oddly: %s\n", l.from, l.tag);
        goto done;
    }
    ver_str(&mine, have);
    ver_str(&theirs, want);
    if (ver_cmp(&theirs, &mine) <= 0) {
        say(UPD_TOOL ": up to date: %s installed, %s has %s\n", have, l.from, want);
        rc = RETURN_OK;
        goto done;
    }
    say(UPD_TOOL ": %s is available on %s (%s installed)\n", want, l.from, have);
    rc = RETURN_WARN;
    if (args[A_GET] || args[A_FETCH])
        rc = get_and_install(&l, args[A_GET] != 0) ? RETURN_OK : RETURN_ERROR;

done:
    if (ctx) SSL_CTX_free(ctx);
    if (AmiSSLBase) CloseAmiSSL();
    if (AmiSSLMasterBase) CloseLibrary(AmiSSLMasterBase);
    if (SocketBase) CloseLibrary(SocketBase);
    FreeArgs(rda);
    return rc;
}
