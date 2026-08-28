/* host_rdargs.c - AmigaOS ReadArgs over argv for the host build.
 *
 * Implements the subset of the ReadArgs template language AmiPart's CLI
 * uses: NAME, NAME/S (switch), NAME/K (keyword with value), NAME/M
 * (multiple strings), NAME/A (required).  /N and /F are not needed by
 * the template and are treated as /K.
 *
 * Matching follows AmigaDOS rules closely enough for cmdline.txt to
 * stay true on the host: keywords are case-insensitive, values come as
 * KEY=value or KEY value, and unkeyed tokens fill unfilled plain slots
 * first, then the /M entry.  A lone "?" prints the template and reads
 * one line of arguments from stdin, like AmigaDOS does.
 *
 * String pointers land in pointer-width SIPTR slots (see src/clib.h);
 * they point into argv (or into a heap copy of the "?" line, freed by
 * FreeArgs together with the /M vector).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "amiga_compat.h"

extern int    host_argc;
extern char **host_argv;

void host_set_ioerr(LONG err);   /* provided by amiga_shim.c */

#define T_MAX   96          /* template entries */
#define TOK_MAX 256         /* command-line tokens */

struct TEntry {
    char name[24];
    int  is_s, is_k, is_m, is_a;
    int  filled;
};

struct HostRDA {
    char  *linebuf;         /* heap copy of "?" input line, or NULL */
    char **mvec;            /* NULL-terminated /M vector, or NULL   */
};

static int ci_keyeq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return b[n] == '\0';
}

static int parse_template(const char *tmpl, struct TEntry *e)
{
    int n = 0;
    const char *p = tmpl;
    while (*p && n < T_MAX) {
        struct TEntry *t = &e[n];
        size_t len = 0;
        memset(t, 0, sizeof(*t));
        while (*p && *p != ',' && *p != '/' && *p != '=') {
            if (len < sizeof(t->name) - 1) t->name[len++] = *p;
            p++;
        }
        t->name[len] = '\0';
        if (*p == '=') {            /* alias: keep first name, skip alias */
            while (*p && *p != ',' && *p != '/') p++;
        }
        while (*p == '/') {
            char f = toupper((unsigned char)p[1]);
            if      (f == 'S') t->is_s = 1;
            else if (f == 'K') t->is_k = 1;
            else if (f == 'M') t->is_m = 1;
            else if (f == 'A') t->is_a = 1;
            else if (f == 'N' || f == 'F') t->is_k = 1;  /* good enough */
            p += 2;
        }
        if (*p == ',') p++;
        n++;
    }
    return n;
}

/* Split a line into tokens in place ("quoted strings" supported). */
static int tokenize(char *line, char **tok, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        if (*p == '"') {
            tok[n++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            tok[n++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        }
        if (*p) *p++ = '\0';
    }
    return n;
}

struct RDArgs *ReadArgs(CONST_STRPTR tmpl, SIPTR *array, struct RDArgs *rda)
{
    struct TEntry  ent[T_MAX];
    char          *tok[TOK_MAX];
    char          *mcollect[TOK_MAX];
    int            nent, ntok = 0, nm = 0;
    int            i, ti, mslot = -1;
    struct HostRDA *h;

    (void)rda;

    h = (struct HostRDA *)calloc(1, sizeof(*h));
    if (!h) { host_set_ioerr(103 /* ERROR_NO_FREE_STORE */); return NULL; }

    nent = parse_template((const char *)tmpl, ent);
    for (i = 0; i < nent; i++)
        if (ent[i].is_m) mslot = i;

    /* "?": print the template, read one line of args from stdin. */
    if (host_argc == 2 && strcmp(host_argv[1], "?") == 0) {
        char line[4096];
        printf("%s: ", (const char *)tmpl);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) line[0] = '\0';
        h->linebuf = strdup(line);
        if (!h->linebuf) { free(h); host_set_ioerr(103); return NULL; }
        ntok = tokenize(h->linebuf, tok, TOK_MAX);
    } else {
        for (i = 1; i < host_argc && ntok < TOK_MAX; i++)
            tok[ntok++] = host_argv[i];
    }

    for (ti = 0; ti < ntok; ti++) {
        char  *t  = tok[ti];
        char  *eq = strchr(t, '=');
        size_t kl = eq ? (size_t)(eq - t) : strlen(t);
        int    hit = -1;

        for (i = 0; i < nent; i++)
            if (!ent[i].filled && ci_keyeq(t, ent[i].name, kl)) { hit = i; break; }
        /* an already-filled keyword still claims its token (last wins) */
        if (hit < 0)
            for (i = 0; i < nent; i++)
                if (ci_keyeq(t, ent[i].name, kl)) { hit = i; break; }

        if (hit >= 0 && ent[hit].is_s && !eq) {
            array[hit] = (SIPTR)DOSTRUE;
            ent[hit].filled = 1;
            continue;
        }
        if (hit >= 0 && !ent[hit].is_s) {
            char *val;
            if (eq) val = eq + 1;
            else if (ti + 1 < ntok) val = tok[++ti];
            else { host_set_ioerr(116 /* ERROR_KEY_NEEDS_ARG */); goto fail; }
            if (ent[hit].is_m) {
                if (nm < TOK_MAX) mcollect[nm++] = val;
            } else {
                array[hit] = (SIPTR)val;
            }
            ent[hit].filled = 1;
            continue;
        }

        /* Unkeyed token: first unfilled plain (no-flag) slot, else /M. */
        for (i = 0; i < nent; i++)
            if (!ent[i].filled && !ent[i].is_s && !ent[i].is_k && !ent[i].is_m) {
                array[i] = (SIPTR)t;
                ent[i].filled = 1;
                hit = i;
                break;
            }
        if (hit < 0 && mslot >= 0) {
            if (nm < TOK_MAX) mcollect[nm++] = t;
            ent[mslot].filled = 1;
            hit = mslot;
        }
        if (hit < 0) { host_set_ioerr(118 /* ERROR_TOO_MANY_ARGS */); goto fail; }
    }

    if (nm > 0 && mslot >= 0) {
        h->mvec = (char **)calloc((size_t)nm + 1, sizeof(char *));
        if (!h->mvec) { host_set_ioerr(103); goto fail; }
        for (i = 0; i < nm; i++) h->mvec[i] = mcollect[i];
        array[mslot] = (SIPTR)h->mvec;
    }

    for (i = 0; i < nent; i++)
        if (ent[i].is_a && !ent[i].filled) {
            host_set_ioerr(114 /* ERROR_REQUIRED_ARG_MISSING */);
            goto fail;
        }

    return (struct RDArgs *)h;

fail:
    free(h->linebuf);
    free(h->mvec);
    free(h);
    return NULL;
}

void FreeArgs(struct RDArgs *rda)
{
    struct HostRDA *h = (struct HostRDA *)rda;
    if (!h) return;
    free(h->linebuf);
    free(h->mvec);
    free(h);
}
