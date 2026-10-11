/*
Copyright (C) 2003-2006 Andrey Nazarov

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "ui.h"
#include "common/files.h"
#include "common/fuzzy.h"
#if USE_ZLIB
#include <zlib.h>
#endif

/*
=======================================================================

AQ2WORLD DEMOS

The servers' demos, as the aq2world S3 bucket lists them, a source and
a month at a time (the keys carry the date, and a listing is capped at
a thousand keys, so the month is the unit): the vrol pickups, the clan
war pickups, and de.aq2.rocks. Left and Right move a month, Tab the
source, letters filter the list as the demo browser does, and Enter
downloads the one under the cursor into demos/ - gunzipped, so it plays
and reads like any demo there - or plays it when it is there already;
Shift+Enter starts its map in the jump mod with the pathing menu open on
it, after the download when it is not there yet. Only what is picked is
downloaded. A listing is kept for the session and in s3cache/ on disk:
the month being played is asked again after ten minutes, an older one
after a day.

The players of a demo go in a box under its row, by team: read out of
the file for one that is in demos/, and for the others out of the first
kilobytes of the gzipped file on the bucket - the demo's first block
carries every player's skin - fetched when the cursor has rested on the
row a moment, and kept with the listing.

=======================================================================
*/

#define S3_URL          "https://aq2world.s3.amazonaws.com"
#define S3_MAX_KEYS     4096        // of a month
#define S3_MAX_PAGES    8           // listings of a thousand a month may take
#define S3_CACHE_DIR    "s3cache"
#define S3_TTL_NOW      (10 * 60)   // seconds a listing of this month is good for
#define S3_TTL_PAST     (24 * 60 * 60)
#define S3_PAGES_KEPT   8           // months kept in memory
#define S3_KEY_MAX      192         // a key with its directory: longer than MAX_QPATH
#define S3_PLAYERS_MAX  256         // the players' note
#define S3_PEEK_BYTES   (48 << 10)  // of the gzipped file: the first block is in there
#define S3_PEEK_OUT     (256 << 10) // what they unpack to, at most
#define S3_PEEK_REST_MS 400         // the cursor rests this long before a peek
#define S3_NONE         "-"         // the players of a demo that has none to read
#define S3_EXTRASIZE    q_offsetof(s3entry_t, text)

// a directory of the bucket: keys are <root>/<stem>_<YYYY-MM-DD>_... for
// the stemmed ones; de.aq2.rocks drops <YYYY-MM-DD>_<HHMMSS>_... and,
// from August 2026, <YYYYMMDD>-<HHMMSS>-TP-MM-<t1>_<t2>-<port>-<map>
// straight into its directory
typedef struct {
    const char  *name;
    const char  *root;
    const char  *stem;
} s3source_t;

static const s3source_t s3_sources[] = {
    { "pickup",      "demos/aqtion/vrol/pickup",    "pickup" },
    { "cw-pickup",   "demos/aqtion/vrol/cw-pickup", "cw" },
    { "aq2world_eu", "demos/aqtion/aq2world_eu",    "" },
};
#define S3_SOURCES      q_countof(s3_sources)
#define S3_COMPACT_FROM "2026-08"   // the stemless directory's compact keys, from this month
#define S3_DASHED_UNTIL "2026-09"   // its dashed ones, before this

typedef struct {
    char    key[S3_KEY_MAX];
    int64_t size;
    time_t  stamp;                  // LastModified
    char    players[S3_PLAYERS_MAX]; // by team, a line each; "" not looked at yet, S3_NONE none
} s3raw_t;

// a month's listing, as fetched
typedef struct {
    int     source;
    char    month[8];               // YYYY-MM
    time_t  fetched;
    int     count;
    s3raw_t *raw;
    unsigned used;                  // for the eviction
    bool    dirty;                  // players came in since it was written
} s3page_t;

// a row
typedef struct {
    int64_t size;
    time_t  stamp;
    bool    have;                   // in demos/ already
    int     raw;                    // its index in the page
    char    key[S3_KEY_MAX];
    char    local[S3_KEY_MAX];      // its name in demos/
    char    map[MAX_QPATH];
    char    players[S3_PLAYERS_MAX];
    uint32_t color;                 // MLF_COLOR: right before the text
    char    text[1];                // the columns
} s3entry_t;

#define COL_DATE    0
#define COL_TEAMS   1
#define COL_MAP     2
#define COL_SIZE    3
#define COL_HAVE    4
#define COL_MAX     5

typedef struct {
    menuFrameWork_t menu;
    menuList_t      list;

    int             source;
    int             year, mon;      // the month shown
    char            month[8];
    char            filter[MAX_QPATH];
    void            **all;          // every row of the month; the list shows those the filter lets through
    int             numAll;
    int             widest_map;
    char            title[64];
    char            status[64];
    char            note[160];      // the bottom left: a download, an error

    // the listing under way
    bool            listing;
    int             list_prefix;    // which of the month's prefixes
    int             list_page;
    char            token[512];     // the continuation token of the next page, "" none
    s3raw_t         *raw;
    int             raw_count;

    // the download under way
    bool            fetching;
    char            fetch_key[S3_KEY_MAX];
    char            fetch_local[S3_KEY_MAX];
    char            fetch_map[MAX_QPATH];
    bool            fetch_launch;   // and then its map in the jump mod
    int64_t         fetch_got, fetch_total; // its progress, for the bar

    // the peek under way, and the cursor's rest
    bool            peeking;
    char            peek_key[S3_KEY_MAX];
    s3entry_t       *rest_on;
    unsigned        rest_since;

    s3page_t        pages[S3_PAGES_KEPT];
    unsigned        used;
} m_s3_t;

static m_s3_t   m_s3;
static char     s3_prefilter[MAX_QPATH];    // "aq2world <filter>": typed in before the list shows

static cvar_t   *ui_s3demos_url;

static void S3_BuildList(void);
static void S3_ListStart(void);
static menuSound_t S3_Change(menuCommon_t *self);
static void S3_PageWrite(s3page_t *page);

/*
=======================================================================
NAMES
=======================================================================
*/

static const char *S3_Base(const char *key)
{
    const char *p = strrchr(key, '/');
    return p ? p + 1 : key;
}

// the name it gets in demos/: the key's, without the .gz
static void S3_LocalName(const char *key, char *buf, size_t size)
{
    size_t len;

    Q_strlcpy(buf, S3_Base(key), size);
    len = strlen(buf);
    if (len > 3 && !strcmp(buf + len - 3, ".gz"))
        buf[len - 3] = 0;
}

// the name it is in demos/ under, "" when it is not there: the bot's
// mirror on the laptop prefixes its copies with hl_
static const char *S3_HaveAs(const char *local)
{
    static char name[S3_KEY_MAX + 4];

    if (!os_access(va("%s/demos/%s", fs_gamedir, local), F_OK))
        return local;
    Q_snprintf(name, sizeof(name), "hl_%s", local);
    if (!os_access(va("%s/demos/%s", fs_gamedir, name), F_OK))
        return name;
    return "";
}

static bool S3_IsDigits(const char *s, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (!Q_isdigit(s[i]))
            return false;
    return true;
}

/*
The key's name taken apart for the columns: the date and time, the teams
and the map. Three shapes:
  pickup_2026-10-01_005712_kdxfjgn_TeamDos_wizs_27930.mvd2.gz
  2026-08-30_232544_<t1>_<t2>_<map>_<port>.mvd2.gz
  20260904-225031-TP-MM-<t1>_<t2>-<port>-<map>.mvd2.gz
A name that is none of them shows whole in the teams column.
*/
static void S3_Parse(const char *key, char *date, size_t datesize, char *teams, size_t teamsize,
                     char *map, size_t mapsize)
{
    char name[S3_KEY_MAX], *p, *tok[32];
    const char *base = S3_Base(key);
    int n = 0, i;
    size_t len;

    Q_strlcpy(name, base, sizeof(name));
    len = strlen(name);
    if (len > 3 && !strcmp(name + len - 3, ".gz"))
        name[len -= 3] = 0;
    if (len > 5 && !strcmp(name + len - 5, ".mvd2"))
        name[len -= 5] = 0;

    date[0] = teams[0] = map[0] = 0;

    // the compact shape: 20260904-225031-TP-MM-<t1>_<t2>-<port>-<map>
    if (len > 22 && S3_IsDigits(name, 8) && name[8] == '-' && S3_IsDigits(name + 9, 6) && name[15] == '-') {
        Q_snprintf(date, datesize, "%.4s-%.2s-%.2s %.2s:%.2s", name, name + 4, name + 6, name + 9, name + 11);
        for (p = name; n < 32 && p; n++) {
            tok[n] = p;
            p = strchr(p, '-');
            if (p)
                *p++ = 0;
        }
        // ... TP MM <teams> <port> <map>: the map is last, the port before it
        if (n >= 6) {
            Q_strlcpy(map, tok[n - 1], mapsize);
            Q_strlcpy(teams, tok[n - 3], teamsize);
        } else {
            Q_strlcpy(teams, base, teamsize);
        }
        return;
    }

    for (p = name; n < 32 && p; n++) {
        tok[n] = p;
        p = strchr(p, '_');
        if (p)
            *p++ = 0;
    }
    // the date is the first token of the dashed shape, the second of the stemmed
    i = 0;
    if (n > 1 && !(strlen(tok[0]) == 10 && tok[0][4] == '-'))
        i = 1;
    if (n >= i + 2 && strlen(tok[i]) == 10 && tok[i][4] == '-' && tok[i][7] == '-'
        && strlen(tok[i + 1]) == 6 && S3_IsDigits(tok[i + 1], 6)) {
        Q_snprintf(date, datesize, "%s %.2s:%.2s", tok[i], tok[i + 1], tok[i + 1] + 2);
        // <teams...> <map> <port>
        if (n >= i + 5 && S3_IsDigits(tok[n - 1], strlen(tok[n - 1]))) {
            Q_strlcpy(map, tok[n - 2], mapsize);
            teams[0] = 0;
            for (i += 2; i < n - 2; i++) {
                if (teams[0])
                    Q_strlcat(teams, " ", teamsize);
                Q_strlcat(teams, tok[i], teamsize);
            }
            return;
        }
    }
    Q_strlcpy(teams, base, teamsize);
}

/*
=======================================================================
THE CACHE
=======================================================================
*/

static void S3_CachePath(int source, const char *month, char *buf, size_t size)
{
    Q_snprintf(buf, size, "%s/%s/%s-%s.txt", fs_gamedir, S3_CACHE_DIR, s3_sources[source].name, month);
}

static s3page_t *S3_PageSlot(int source, const char *month, s3page_t **oldest)
{
    s3page_t *page = NULL;
    int i;

    *oldest = NULL;
    for (i = 0; i < S3_PAGES_KEPT; i++) {
        s3page_t *pg = &m_s3.pages[i];

        if (pg->raw && pg->source == source && !strcmp(pg->month, month))
            page = pg;
        if (!*oldest || pg->used < (*oldest)->used)
            *oldest = pg;
    }
    return page;
}

static void S3_PageFree(s3page_t *page)
{
    if (page->dirty)
        S3_PageWrite(page);
    Z_Free(page->raw);
    page->raw = NULL;
    page->count = 0;
}

// the month's listing from this session, or the disk; NULL when stale or absent
static s3page_t *S3_PageFind(int source, const char *month, bool fresh)
{
    time_t now = time(NULL), ttl;
    char path[MAX_OSPATH], line[MAX_STRING_CHARS * 2], stamp[32];
    s3page_t *page, *oldest;
    s3raw_t *raw;
    FILE *f;
    int count = 0;
    long long fetched = 0, size;

    page = S3_PageSlot(source, month, &oldest);

    // this month is asked again after a while, an old one after a day
    ttl = strncmp(month, m_s3.month, 7) ? S3_TTL_PAST : S3_TTL_NOW;
    if (page) {
        if (!fresh || now - page->fetched <= ttl) {
            page->used = ++m_s3.used;
            return page;
        }
        S3_PageFree(page);
        return NULL;
    }

    // the disk
    S3_CachePath(source, month, path, sizeof(path));
    f = fopen(path, "r");
    if (!f)
        return NULL;
    if (!fgets(line, sizeof(line), f) || sscanf(line, "fetched %lld", &fetched) != 1
        || (fresh && now - fetched > ttl)) {
        fclose(f);
        return NULL;
    }
    raw = UI_Malloc(sizeof(*raw) * S3_MAX_KEYS);
    while (count < S3_MAX_KEYS && fgets(line, sizeof(line), f)) {
        s3raw_t *r = &raw[count];
        char *nl = strchr(line, '\n'), *t1, *t2, *t3, *p;

        if (nl)
            *nl = 0;
        memset(r, 0, sizeof(*r));
        t1 = strchr(line, '\t');
        if (t1) {
            // size <tab> stamp <tab> players (the lines as |) <tab> key
            *t1++ = 0;
            t2 = strchr(t1, '\t');
            if (!t2)
                continue;
            *t2++ = 0;
            t3 = strchr(t2, '\t');
            if (!t3)
                continue;
            *t3++ = 0;
            r->size = atoll(line);
            r->stamp = atoll(t1);
            Q_strlcpy(r->players, t2, sizeof(r->players));
            for (p = r->players; *p; p++)
                if (*p == '|')
                    *p = '\n';
            Q_strlcpy(r->key, t3, sizeof(r->key));
        } else {
            // the first form: size stamp key
            if (sscanf(line, "%lld %31s", &size, stamp) != 2)
                continue;
            if (!(nl = strchr(line, ' ')) || !(nl = strchr(nl + 1, ' ')))
                continue;
            Q_strlcpy(r->key, nl + 1, sizeof(r->key));
            r->size = size;
            r->stamp = atoll(stamp);
        }
        if (r->key[0])
            count++;
    }
    fclose(f);

    page = oldest;
    if (page->raw)
        S3_PageFree(page);
    page->raw = UI_Malloc(sizeof(*raw) * max(count, 1));
    memcpy(page->raw, raw, sizeof(*raw) * count);
    Z_Free(raw);
    page->source = source;
    Q_strlcpy(page->month, month, sizeof(page->month));
    page->fetched = fetched;
    page->count = count;
    page->used = ++m_s3.used;
    page->dirty = false;
    return page;
}

static void S3_PageWrite(s3page_t *page)
{
    char path[MAX_OSPATH], players[S3_PLAYERS_MAX], *p;
    FILE *f;
    int i;

    page->dirty = false;
    S3_CachePath(page->source, page->month, path, sizeof(path));
    if (FS_CreatePath(path))
        return;
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "fetched %lld\n", (long long)page->fetched);
    for (i = 0; i < page->count; i++) {
        const s3raw_t *r = &page->raw[i];

        Q_strlcpy(players, r->players, sizeof(players));
        for (p = players; *p; p++)
            if (*p == '\n')
                *p = '|';
        fprintf(f, "%lld\t%lld\t%s\t%s\n", (long long)r->size, (long long)r->stamp, players, r->key);
    }
    fclose(f);
}

// a listing just fetched: into the pages, shrunk to its count, and to disk
static s3page_t *S3_PageStore(int source, const char *month, s3raw_t *raw, int count)
{
    s3page_t *page, *oldest;

    page = S3_PageSlot(source, month, &oldest);
    if (!page)
        page = oldest;
    if (page->raw)
        S3_PageFree(page);
    page->raw = UI_Malloc(sizeof(*raw) * max(count, 1));
    memcpy(page->raw, raw, sizeof(*raw) * count);
    Z_Free(raw);
    page->count = count;
    page->source = source;
    Q_strlcpy(page->month, month, sizeof(page->month));
    page->fetched = time(NULL);
    page->used = ++m_s3.used;
    S3_PageWrite(page);
    return page;
}

/*
=======================================================================
THE LISTING
=======================================================================
*/

// the key prefixes a month's files have in a source, in prefix; how many
static int S3_Prefixes(int source, const char *month, char prefix[2][MAX_QPATH])
{
    const s3source_t *src = &s3_sources[source];
    int n = 0;

    if (src->stem[0]) {
        Q_snprintf(prefix[n++], MAX_QPATH, "%s/%s_%s", src->root, src->stem, month);
        return n;
    }
    if (strcmp(month, S3_DASHED_UNTIL) < 0)
        Q_snprintf(prefix[n++], MAX_QPATH, "%s/%s", src->root, month);
    if (strcmp(month, S3_COMPACT_FROM) >= 0)
        Q_snprintf(prefix[n++], MAX_QPATH, "%s/%.4s%.2s", src->root, month, month + 5);
    return n;
}

static void S3_UrlEncode(const char *s, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;

    for (; *s && n + 4 < size; s++) {
        if (Q_isalnum(*s) || *s == '-' || *s == '_' || *s == '.' || *s == '~') {
            out[n++] = *s;
        } else {
            out[n++] = '%';
            out[n++] = hex[(*s >> 4) & 15];
            out[n++] = hex[*s & 15];
        }
    }
    out[n] = 0;
}

static void S3_KeyUrl(const char *key, char *url, size_t size)
{
    char enc[S3_KEY_MAX * 3];

    S3_UrlEncode(key, enc, sizeof(enc));
    Q_snprintf(url, size, "%s/%s", ui_s3demos_url->string, enc);
}

// a ...T...Z stamp as a time: days since 1970 by the civil calendar,
// as timegm would have it, which mingw has not
static time_t S3_Stamp(const char *s)
{
    int y, m, d, hh, mm, ss;
    int64_t era, yoe, doy, doe, days;

    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &hh, &mm, &ss) != 6)
        return 0;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + hh * 3600 + mm * 60 + ss);
}

// between <tag> and </tag> after from, cut to fit; NULL when there is none
static const char *S3_Tag(const char *from, const char *end, const char *tag, char *buf, size_t size)
{
    char open[32], close[32];
    const char *p, *q;
    size_t n;

    Q_snprintf(open, sizeof(open), "<%s>", tag);
    Q_snprintf(close, sizeof(close), "</%s>", tag);
    p = strstr(from, open);
    if (!p || (end && p >= end))
        return NULL;
    p += strlen(open);
    q = strstr(p, close);
    if (!q || (end && q > end))
        return NULL;
    n = q - p;
    if (n >= size)
        n = size - 1;
    memcpy(buf, p, n);
    buf[n] = 0;
    return q;
}

// one page of a listing into raw; true if there is a next page (token set)
static bool S3_ParseListing(const char *xml)
{
    char buf[MAX_STRING_CHARS], key[S3_KEY_MAX], stamp[64], size[32];
    const char *p = xml, *q, *e;

    m_s3.token[0] = 0;
    if (S3_Tag(xml, NULL, "IsTruncated", buf, sizeof(buf)) && !strcmp(buf, "true"))
        S3_Tag(xml, NULL, "NextContinuationToken", m_s3.token, sizeof(m_s3.token));

    while ((p = strstr(p, "<Contents>")) != NULL) {
        e = strstr(p, "</Contents>");
        if (!e)
            break;
        if (S3_Tag(p, e, "Key", key, sizeof(key)) && S3_Tag(p, e, "LastModified", stamp, sizeof(stamp))
            && S3_Tag(p, e, "Size", size, sizeof(size)) && m_s3.raw_count < S3_MAX_KEYS) {
            s3raw_t *r = &m_s3.raw[m_s3.raw_count];

            // the odd character an XML escape stands for is not in a demo's name
            if (!strchr(key, '&') && (q = strrchr(key, '/')) != NULL && q[1]) {
                memset(r, 0, sizeof(*r));
                Q_strlcpy(r->key, key, sizeof(r->key));
                r->size = atoll(size);
                r->stamp = S3_Stamp(stamp);
                m_s3.raw_count++;
            }
        }
        p = e + 11;
    }
    return m_s3.token[0] != 0;
}

static void S3_ListFail(const char *err)
{
    Q_snprintf(m_s3.note, sizeof(m_s3.note), "listing failed: %s", err);
    m_s3.listing = false;
    Z_Freep(&m_s3.raw);
    S3_BuildList();
}

static void S3_ListRequest(void)
{
    char prefix[2][MAX_QPATH], enc[MAX_QPATH * 3], url[MAX_STRING_CHARS], err[128];
    int n = S3_Prefixes(m_s3.source, m_s3.month, prefix);

    if (m_s3.list_prefix >= n) {
        // done: into the cache, onto the screen; what was known of the
        // players before is carried over
        s3page_t *old, *oldest;
        s3raw_t *raw = m_s3.raw;
        int count = m_s3.raw_count, i, k;

        old = S3_PageSlot(m_s3.source, m_s3.month, &oldest);
        if (old) {
            for (i = 0; i < count; i++)
                for (k = 0; k < old->count; k++)
                    if (old->raw[k].players[0] && !strcmp(old->raw[k].key, raw[i].key)) {
                        Q_strlcpy(raw[i].players, old->raw[k].players, sizeof(raw[i].players));
                        break;
                    }
        }
        m_s3.raw = NULL;
        m_s3.raw_count = 0;
        m_s3.listing = false;
        S3_PageStore(m_s3.source, m_s3.month, raw, count);
        S3_BuildList();
        return;
    }

    S3_UrlEncode(prefix[m_s3.list_prefix], enc, sizeof(enc));
    Q_snprintf(url, sizeof(url), "%s/?list-type=2&prefix=%s", ui_s3demos_url->string, enc);
    if (m_s3.token[0]) {
        S3_UrlEncode(m_s3.token, enc, sizeof(enc));
        Q_strlcat(url, "&continuation-token=", sizeof(url));
        Q_strlcat(url, enc, sizeof(url));
    }
    if (!HTTP_FetchStart(url, NULL, false)) {
        HTTP_FetchState(NULL, NULL, err, sizeof(err));
        S3_ListFail(err[0] ? err : "a download is up");
    }
}

// a listing's page has come in
static void S3_ListGot(void)
{
    char err[128];
    void *data;
    bool more;

    if (HTTP_FetchState(NULL, NULL, err, sizeof(err)) == FETCH_FAILED) {
        HTTP_FetchTake(NULL);
        S3_ListFail(err);
        return;
    }
    HTTP_FetchTake(&data);
    more = data && S3_ParseListing(data);
    HTTP_FreeFile(data);

    if (more && ++m_s3.list_page < S3_MAX_PAGES) {
        S3_ListRequest();
        return;
    }
    m_s3.token[0] = 0;
    m_s3.list_page = 0;
    m_s3.list_prefix++;
    S3_ListRequest();
}

static bool S3_Busy(void)
{
    return m_s3.listing || m_s3.fetching || m_s3.peeking;
}

// the month's listing: from the cache when it is fresh, else fetched
static void S3_ListStart(void)
{
    s3page_t *page = S3_PageFind(m_s3.source, m_s3.month, true);

    if (page) {
        S3_BuildList();
        return;
    }
    if (S3_Busy()) {
        // the fetch up first; the listing is asked for again when it is done
        S3_BuildList();
        return;
    }
    m_s3.listing = true;
    m_s3.list_prefix = m_s3.list_page = 0;
    m_s3.token[0] = 0;
    m_s3.raw = UI_Malloc(sizeof(*m_s3.raw) * S3_MAX_KEYS);
    m_s3.raw_count = 0;
    m_s3.note[0] = 0;
    S3_BuildList();
    S3_ListRequest();
}

/*
=======================================================================
THE PLAYERS

Out of the demo's first block, which carries every player's skin: the
file in demos/ when it is there, else the first kilobytes of the one on
the bucket. CL_GetDemoInfo reads a file; a peek is read here, out of
the unpacked bytes, which may stop short - the whole serverdata block
is in the first few kilobytes.
=======================================================================
*/

#if USE_ZLIB
static size_t S3_Gunzip(const byte *in, size_t inlen, byte *out, size_t outsize)
{
    z_stream zs;
    size_t got;

    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 15 + 16) != Z_OK)
        return 0;
    zs.next_in = (byte *)in;
    zs.avail_in = inlen;
    zs.next_out = out;
    zs.avail_out = outsize;
    inflate(&zs, Z_SYNC_FLUSH);     // whatever it gives: a cut stream is expected
    got = outsize - zs.avail_out;
    inflateEnd(&zs);
    return got;
}
#endif

typedef struct {
    const byte  *d;
    size_t      p, end;
    bool        bad;
} s3rd_t;

static int S3_RdByte(s3rd_t *r)
{
    if (r->p >= r->end) {
        r->bad = true;
        return 0;
    }
    return r->d[r->p++];
}

static int S3_RdShort(s3rd_t *r)
{
    int lo = S3_RdByte(r), hi = S3_RdByte(r);
    return (short)(lo | (hi << 8));
}

static int S3_RdLong(s3rd_t *r)
{
    int lo = S3_RdShort(r) & 0xffff, hi = S3_RdShort(r) & 0xffff;
    return lo | (hi << 16);
}

static void S3_RdString(s3rd_t *r, char *buf, size_t size)
{
    size_t n = 0;

    while (r->p < r->end) {
        int c = r->d[r->p++];
        if (!c) {
            buf[n] = 0;
            return;
        }
        if (n + 1 < size)
            buf[n++] = c;
    }
    buf[n] = 0;
    r->bad = true;
}

// the players' note out of an MVD's head, "" when it cannot be read
static void S3_PeekParse(const byte *data, size_t len, char *players, size_t size)
{
    static demoInfo_t info;
    s3rd_t r;
    char string[MAX_QPATH], *skin;
    int blocklen, cmd, version, flags, index;

    players[0] = 0;
    if (len < 6 || memcmp(data, "MVD2", 4))
        return;
    blocklen = data[4] | (data[5] << 8);
    r.d = data;
    r.p = 6;
    r.end = min((size_t)6 + blocklen, len);
    r.bad = false;

    cmd = S3_RdByte(&r);
    if ((cmd & 31) != 4 || S3_RdLong(&r) != 37)  // mvd_serverdata, PROTOCOL_VERSION_MVD
        return;
    version = S3_RdShort(&r) & 0xffff;
    flags = version >= 2012 ? S3_RdShort(&r) & 0xffff : cmd >> 5;
    if (flags & 1)              // extended limits: other configstring numbers
        return;
    S3_RdLong(&r);              // servercount
    S3_RdString(&r, string, sizeof(string));    // gamedir
    S3_RdShort(&r);             // clientNum

    memset(&info, 0, sizeof(info));
    while (!r.bad) {
        index = S3_RdShort(&r) & 0xffff;
        if (index == 2080)      // CS_END, old protocol
            break;
        S3_RdString(&r, string, sizeof(string));
        if (index < 1312 || index >= 1312 + MAX_CLIENTS)    // CS_PLAYERSKINS
            continue;
        skin = strchr(string, '\\');
        if (skin)
            *skin++ = 0;
        if (string[0] && info.numPlayers < MAX_CLIENTS) {
            Q_strlcpy(info.players[info.numPlayers].name, string, MAX_CLIENT_NAME);
            Q_strlcpy(info.players[info.numPlayers].skin, skin ? skin : "", MAX_QPATH);
            info.numPlayers++;
        }
    }
    CL_DemoPlayersNote(&info, players, size);
}

// what is known of the players, into the row and the month's page
static void S3_SetPlayers(s3entry_t *e, const char *players)
{
    s3page_t *page, *oldest;

    Q_strlcpy(e->players, players, sizeof(e->players));
    page = S3_PageSlot(m_s3.source, m_s3.month, &oldest);
    if (page && e->raw >= 0 && e->raw < page->count && !strcmp(page->raw[e->raw].key, e->key)) {
        Q_strlcpy(page->raw[e->raw].players, players, sizeof(page->raw[e->raw].players));
        page->dirty = true;
    }
}

// a demo in demos/: read there
static void S3_LocalPlayers(s3entry_t *e)
{
    static demoInfo_t info;
    char path[MAX_OSPATH], players[S3_PLAYERS_MAX];
    const char *name = S3_HaveAs(e->local);

    if (!name[0])
        return;
    memset(&info, 0, sizeof(info));
    Q_snprintf(path, sizeof(path), "demos/%s", name);
    if (!CL_GetDemoInfo(path, &info))
        return;
    CL_DemoPlayersNote(&info, players, sizeof(players));
    S3_SetPlayers(e, players[0] ? players : S3_NONE);
}

static void S3_PeekStart(s3entry_t *e)
{
    char url[MAX_STRING_CHARS];

    S3_KeyUrl(e->key, url, sizeof(url));
    if (!HTTP_FetchStartRange(url, S3_PEEK_BYTES))
        return;
    m_s3.peeking = true;
    Q_strlcpy(m_s3.peek_key, e->key, sizeof(m_s3.peek_key));
}

static void S3_PeekDone(void)
{
    char players[S3_PLAYERS_MAX];
    void *data;
    int len, i;
    bool ok = HTTP_FetchState(NULL, NULL, NULL, 0) == FETCH_DONE;

    len = HTTP_FetchTake(&data);
    m_s3.peeking = false;
    players[0] = 0;
#if USE_ZLIB
    if (ok && data && len > 0) {
        byte *out = Z_Malloc(S3_PEEK_OUT);
        size_t got = S3_Gunzip(data, len, out, S3_PEEK_OUT);

        S3_PeekParse(out, got, players, sizeof(players));
        Z_Free(out);
    }
#endif
    HTTP_FreeFile(data);

    for (i = 0; i < m_s3.numAll; i++) {
        s3entry_t *e = m_s3.all[i];

        if (!strcmp(e->key, m_s3.peek_key)) {
            S3_SetPlayers(e, players[0] ? players : ok ? S3_NONE : "");
            break;
        }
    }
}

// the cursor has rested on a row: its players, if they are not known
static void S3_Rest(void)
{
    s3entry_t *e = m_s3.list.numItems ? m_s3.list.items[m_s3.list.curvalue] : NULL;

    if (e != m_s3.rest_on) {
        m_s3.rest_on = e;
        m_s3.rest_since = uis.realtime;
        return;
    }
    if (!e || e->players[0] || e->have || S3_Busy())
        return;
    if (uis.realtime - m_s3.rest_since < S3_PEEK_REST_MS)
        return;
    S3_PeekStart(e);
    if (!m_s3.peeking)
        S3_SetPlayers(e, S3_NONE);  // not asked again this session
}

/*
=======================================================================
THE LIST
=======================================================================
*/

static int S3_RawCmp(const void *a, const void *b)
{
    // newest first: the names carry the date and sort as it does
    return strcmp(S3_Base(((const s3raw_t *)b)->key), S3_Base(((const s3raw_t *)a)->key));
}

static void S3_FreeList(void)
{
    int i;

    if (m_s3.all) {
        for (i = 0; i < m_s3.numAll; i++)
            Z_Free(m_s3.all[i]);
        Z_Freep(&m_s3.all);
        m_s3.numAll = 0;
    }
    Z_Freep(&m_s3.list.items);
    m_s3.list.numItems = 0;
    m_s3.rest_on = NULL;
}

// by the name, or by who is in it
static int S3_Score(const s3entry_t *e)
{
    int s1 = Fuzzy_Score(m_s3.filter, S3_Base(e->key)), s2 = -1;

    if (e->players[0] && strcmp(e->players, S3_NONE))
        s2 = Fuzzy_Score(m_s3.filter, e->players);
    return max(s1, s2);
}

static int S3_ScoreCmp(const void *p1, const void *p2)
{
    const s3entry_t *e1 = *(const s3entry_t **)p1;
    const s3entry_t *e2 = *(const s3entry_t **)p2;
    int s1 = S3_Score(e1);
    int s2 = S3_Score(e2);

    if (s1 != s2)
        return s2 - s1;
    return strcmp(S3_Base(e2->key), S3_Base(e1->key));
}

static void S3_ApplyFilter(void)
{
    int i, n = 0;

    for (i = 0; i < m_s3.numAll; i++) {
        s3entry_t *e = m_s3.all[i];

        if (!*m_s3.filter || S3_Score(e) >= 0)
            m_s3.list.items[n++] = e;
    }
    m_s3.list.numItems = n;
    m_s3.list.curvalue = 0;
    m_s3.list.prestep = 0;
    if (*m_s3.filter)
        qsort(m_s3.list.items, n, sizeof(void *), S3_ScoreCmp);
    MenuList_Init(&m_s3.list);

    if (m_s3.listing)
        Q_snprintf(m_s3.status, sizeof(m_s3.status), "listing... %d so far", m_s3.raw_count);
    else if (*m_s3.filter)
        Q_snprintf(m_s3.status, sizeof(m_s3.status), "%d of %d demos", n, m_s3.numAll);
    else
        Q_snprintf(m_s3.status, sizeof(m_s3.status), "%d demo%s", n, n == 1 ? "" : "s");
}

static void S3_EntryHave(s3entry_t *e, bool have)
{
    char *p = e->text;
    int k;

    e->have = have;
    e->color = have ? MakeColor(130, 255, 130, 255) : U32_WHITE;
    // the last column, past four NULs: room for "yes" was left in it
    for (k = 0; k < COL_HAVE; k++)
        p += strlen(p) + 1;
    strcpy(p, have ? "yes" : "   ");
}

static s3entry_t *S3_Entry(const s3raw_t *r, int raw)
{
    char date[32], teams[MAX_QPATH], map[MAX_QPATH], size[16], local[S3_KEY_MAX];
    s3entry_t *e;
    size_t len;

    S3_Parse(r->key, date, sizeof(date), teams, sizeof(teams), map, sizeof(map));
    S3_LocalName(r->key, local, sizeof(local));
    Com_FormatSize(size, sizeof(size), r->size);
    e = UI_FormatColumns(S3_EXTRASIZE, date, teams, map, size, "   ", NULL);
    e->size = r->size;
    e->stamp = r->stamp;
    e->raw = raw;
    Q_strlcpy(e->key, r->key, sizeof(e->key));
    Q_strlcpy(e->local, local, sizeof(e->local));
    Q_strlcpy(e->map, map, sizeof(e->map));
    Q_strlcpy(e->players, r->players, sizeof(e->players));
    S3_EntryHave(e, S3_HaveAs(local)[0] != 0);
    if (e->have && !e->players[0])
        S3_LocalPlayers(e);

    len = strlen(map);
    if (len > 12)
        len = 12;
    if (len > m_s3.widest_map)
        m_s3.widest_map = len;
    return e;
}

// the rows of the month shown, from the cache; while a listing runs,
// what it has so far
static void S3_BuildList(void)
{
    s3page_t *page = m_s3.listing ? NULL : S3_PageFind(m_s3.source, m_s3.month, false);
    s3raw_t *raw = m_s3.listing ? m_s3.raw : page ? page->raw : NULL;
    int i, count = m_s3.listing ? m_s3.raw_count : page ? page->count : 0;

    S3_FreeList();
    m_s3.widest_map = 3;

    if (raw && count > 1)
        qsort(raw, count, sizeof(*raw), S3_RawCmp);
    m_s3.all = UI_Malloc(sizeof(void *) * (count + 1));
    m_s3.numAll = 0;
    for (i = 0; i < count; i++)
        m_s3.all[m_s3.numAll++] = S3_Entry(&raw[i], i);
    m_s3.list.items = UI_Malloc(sizeof(void *) * (count + 1));

    Q_snprintf(m_s3.title, sizeof(m_s3.title), "aq2world demos: %s, %s", s3_sources[m_s3.source].name, m_s3.month);
    m_s3.menu.size(&m_s3.menu);
    S3_ApplyFilter();
    S3_Change(&m_s3.list.generic);     // the status line for the row the cursor lands on
}

/*
=======================================================================
THE DOWNLOAD, AND THE MAP
=======================================================================
*/

static bool S3_NameOk(const char *s)
{
    return !strchr(s, '"') && !strchr(s, ';') && !strstr(s, "..") && !strchr(s, '\n');
}

// the demo's map in the jump mod, the pathing menu open on the demo
// (jmod_paths_open, read by the game as the player comes in)
static menuSound_t S3_Launch(const char *local, const char *map)
{
    const char *name = S3_HaveAs(local);

    if (!name[0] || !map[0] || !S3_NameOk(name) || !S3_NameOk(map)) {
        Q_strlcpy(m_s3.note, "its map is not known from its name", sizeof(m_s3.note));
        return QMS_BEEP;
    }
    // the other modes off, which the game checks before jump (as Practice Jumps does)
    Cbuf_AddText(&cmd_buffer, va("forcemenuoff; set ltk_loadbots 0; set am 0; "
                 "set teamplay 0; set teamdm 0; set ctf 0; set dom 0; set esp 0; "
                 "set use_tourney 0; set deathmatch 1; set jump 1; "
                 "set jmod_paths_open \"%s\"; map \"%s\" force\n", name, map));
    return QMS_SILENT;
}

static menuSound_t S3_Fetch(s3entry_t *e, bool launch)
{
    char url[MAX_STRING_CHARS], path[MAX_OSPATH], err[128];
    size_t len = strlen(e->local);
    bool gz = strlen(S3_Base(e->key)) > len;

    if (m_s3.fetching) {
        Q_strlcpy(m_s3.note, "one download at a time", sizeof(m_s3.note));
        return QMS_BEEP;
    }
    if (m_s3.listing) {
        Q_strlcpy(m_s3.note, "after the listing", sizeof(m_s3.note));
        return QMS_BEEP;
    }
    if (m_s3.peeking) {
        HTTP_FetchCancel();     // the download comes first
        m_s3.peeking = false;
    }
    if (!S3_NameOk(e->local)) {
        Q_strlcpy(m_s3.note, "that name will not do as a file's", sizeof(m_s3.note));
        return QMS_BEEP;
    }

    S3_KeyUrl(e->key, url, sizeof(url));
    Q_snprintf(path, sizeof(path), "demos/%s", e->local);
    if (!HTTP_FetchStart(url, path, gz)) {
        HTTP_FetchState(NULL, NULL, err, sizeof(err));
        Q_snprintf(m_s3.note, sizeof(m_s3.note), "download failed: %s", err);
        return QMS_BEEP;
    }
    m_s3.fetching = true;
    m_s3.fetch_launch = launch;
    Q_strlcpy(m_s3.fetch_key, e->key, sizeof(m_s3.fetch_key));
    Q_strlcpy(m_s3.fetch_local, e->local, sizeof(m_s3.fetch_local));
    Q_strlcpy(m_s3.fetch_map, e->map, sizeof(m_s3.fetch_map));
    Q_snprintf(m_s3.note, sizeof(m_s3.note), "downloading %s...", e->local);
    return QMS_IN;
}

static void S3_FetchDone(void)
{
    char err[128];
    int i;
    bool ok = HTTP_FetchState(NULL, NULL, err, sizeof(err)) == FETCH_DONE;

    HTTP_FetchTake(NULL);
    m_s3.fetching = false;
    if (!ok) {
        Q_snprintf(m_s3.note, sizeof(m_s3.note), "%s: download failed: %s", m_s3.fetch_local, err);
        return;
    }
    Q_snprintf(m_s3.note, sizeof(m_s3.note), "%s is in demos/: Enter plays it", m_s3.fetch_local);
    for (i = 0; i < m_s3.numAll; i++) {
        s3entry_t *e = m_s3.all[i];

        if (!strcmp(e->key, m_s3.fetch_key)) {
            S3_EntryHave(e, true);
            if (!e->players[0] || !strcmp(e->players, S3_NONE))
                S3_LocalPlayers(e);
        }
    }
    if (m_s3.fetch_launch)
        S3_Launch(m_s3.fetch_local, m_s3.fetch_map);
}

/*
=======================================================================
THE MENU
=======================================================================
*/

static void S3_SetMonth(int year, int mon)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    while (mon < 0) {
        mon += 12;
        year--;
    }
    while (mon > 11) {
        mon -= 12;
        year++;
    }
    // not past this month
    if (tm && (year > tm->tm_year || (year == tm->tm_year && mon > tm->tm_mon))) {
        year = tm->tm_year;
        mon = tm->tm_mon;
    }
    m_s3.year = year;
    m_s3.mon = mon;
    Q_snprintf(m_s3.month, sizeof(m_s3.month), "%04d-%02d", year + 1900, mon + 1);
}

static menuSound_t S3_Change(menuCommon_t *self)
{
    s3entry_t *e;

    if (!m_s3.list.numItems) {
        m_s3.menu.status = m_s3.listing ? "Listing..." : "Nothing listed: Left for an older month, Tab for another source";
        return QMS_BEEP;
    }
    e = m_s3.list.items[m_s3.list.curvalue];
    m_s3.menu.status = e->have ? "Enter: play it   Shift+Enter: its map in the jump mod, with its routes"
        : "Enter: download it   Shift+Enter: download it and start its map in the jump mod";
    return QMS_SILENT;
}

static menuSound_t S3_Activate(menuCommon_t *self)
{
    s3entry_t *e;
    const char *name;

    if (!m_s3.list.numItems)
        return QMS_BEEP;
    e = m_s3.list.items[m_s3.list.curvalue];
    if (e->have) {
        name = S3_HaveAs(e->local);
        if (!name[0] || !S3_NameOk(name))
            return QMS_BEEP;
        Cbuf_AddText(&cmd_buffer, va("demo \"/demos/%s\"\n", name));
        return QMS_SILENT;
    }
    return S3_Fetch(e, false);
}

static menuSound_t S3_Practice(void)
{
    s3entry_t *e;

    if (!m_s3.list.numItems)
        return QMS_BEEP;
    e = m_s3.list.items[m_s3.list.curvalue];
    if (e->have)
        return S3_Launch(e->local, e->map);
    if (!e->map[0]) {
        Q_strlcpy(m_s3.note, "its map is not known from its name", sizeof(m_s3.note));
        return QMS_BEEP;
    }
    return S3_Fetch(e, true);
}

static void S3_Size(menuFrameWork_t *self)
{
    int w1, w2;

    m_s3.list.generic.x      = 0;
    m_s3.list.generic.y      = CONCHAR_HEIGHT;
    m_s3.list.generic.width  = 0;
    m_s3.list.generic.height = uis.height - CONCHAR_HEIGHT * 3 - 1;    // the hint line and the status line under it

    // the date, the size and the have columns are fixed; the teams take the rest
    w1 = 16 + 7 + 4 + m_s3.widest_map;
    w2 = uis.width - w1 * CONCHAR_WIDTH - MLIST_PADDING * 5 - MLIST_SCROLLBAR_WIDTH;
    if (w2 < 8 * CONCHAR_WIDTH)
        w2 = 8 * CONCHAR_WIDTH;
    m_s3.list.columns[COL_DATE].width = 16 * CONCHAR_WIDTH + MLIST_PADDING;
    m_s3.list.columns[COL_TEAMS].width = w2;
    m_s3.list.columns[COL_MAP].width = m_s3.widest_map * CONCHAR_WIDTH + MLIST_PADDING;
    m_s3.list.columns[COL_SIZE].width = 7 * CONCHAR_WIDTH + MLIST_PADDING;
    m_s3.list.columns[COL_HAVE].width = 4 * CONCHAR_WIDTH + MLIST_PADDING;
    m_s3.list.numcolumns = COL_MAX;
}

static menuSound_t S3_Keydown(menuFrameWork_t *self, int key)
{
    size_t len = strlen(m_s3.filter);

    switch (key) {
    case K_BACKSPACE:
        if (!len)
            return QMS_NOTHANDLED;  // out of the menu
        m_s3.filter[len - 1] = 0;
        S3_ApplyFilter();
        return QMS_SILENT;
    case K_ESCAPE:
        if (!len)
            return QMS_NOTHANDLED;
        m_s3.filter[0] = 0;
        S3_ApplyFilter();
        return QMS_SILENT;
    case K_ENTER:
    case K_KP_ENTER:
        if (Key_IsDown(K_SHIFT))
            return S3_Practice();
        return QMS_NOTHANDLED;
    case K_LEFTARROW:
    case K_KP_LEFTARROW:
        S3_SetMonth(m_s3.year, m_s3.mon - 1);
        S3_ListStart();
        return QMS_MOVE;
    case K_RIGHTARROW:
    case K_KP_RIGHTARROW:
        S3_SetMonth(m_s3.year, m_s3.mon + 1);
        S3_ListStart();
        return QMS_MOVE;
    case K_TAB:
        m_s3.source = (m_s3.source + 1) % S3_SOURCES;
        S3_ListStart();
        return QMS_MOVE;
    case K_F5:
        // asked again, whatever the cache has
        if (!m_s3.listing && !m_s3.fetching) {
            s3page_t *page, *oldest;

            page = S3_PageSlot(m_s3.source, m_s3.month, &oldest);
            if (page)
                page->fetched = 0;
            S3_ListStart();
        }
        return QMS_MOVE;
    }

    // letters are for the filter (they come as characters), not the
    // list's h/j/k/l
    if (key >= 32 && key < 127 && !Key_IsDown(K_ALT) && !Key_IsDown(K_CTRL))
        return QMS_SILENT;
    return QMS_NOTHANDLED;
}

static menuSound_t S3_CharEvent(menuFrameWork_t *self, int key)
{
    size_t len = strlen(m_s3.filter);

    if (!Q_isprint(key) || len >= sizeof(m_s3.filter) - 1)
        return QMS_NOTHANDLED;
    m_s3.filter[len] = key;
    m_s3.filter[len + 1] = 0;
    S3_ApplyFilter();
    return QMS_SILENT;
}

// every frame, menu up or not: a listing, a download or a peek under
// way is followed - a download goes on after the menu is left, and a
// map it was to start comes up all the same
void S3_Frame(void)
{
    fetch_state_t state;
    int64_t got, total;
    char size[16];

    if (!S3_Busy())
        return;
    state = HTTP_FetchState(&got, &total, NULL, 0);
    if (state == FETCH_DONE || state == FETCH_FAILED) {
        if (m_s3.listing)
            S3_ListGot();
        else if (m_s3.fetching)
            S3_FetchDone();
        else
            S3_PeekDone();
        // a month asked for during the download
        if (uis.activeMenu == &m_s3.menu && !S3_Busy() && !S3_PageFind(m_s3.source, m_s3.month, true))
            S3_ListStart();
    } else if (m_s3.fetching) {
        m_s3.fetch_got = got;
        m_s3.fetch_total = total;
    }
}

// the download's bar on the hint line: its share filled green, the
// name and the bytes over it
static void S3_DrawProgress(int y)
{
    char got[16], total[16], text[MAX_QPATH + 64];
    int w = uis.width, fill = 0;

    R_DrawFill32(0, y - 2, w, CONCHAR_HEIGHT + 4, MakeColor(0, 0, 0, 220));
    Com_FormatSize(got, sizeof(got), m_s3.fetch_got);
    if (m_s3.fetch_total > 0) {
        fill = (int)(w * m_s3.fetch_got / m_s3.fetch_total);
        Com_FormatSize(total, sizeof(total), m_s3.fetch_total);
        Q_snprintf(text, sizeof(text), "Downloading %s   %d%%   %s of %s", m_s3.fetch_local,
                   (int)(m_s3.fetch_got * 100 / m_s3.fetch_total), got, total);
    } else {
        Q_snprintf(text, sizeof(text), "Downloading %s   %s", m_s3.fetch_local, got);
    }
    if (fill > 0)
        R_DrawFill32(0, y - 2, fill, CONCHAR_HEIGHT + 4, MakeColor(40, 180, 70, 230));
    UI_DrawString(w / 2, y, UI_CENTER, text);
}

static void S3_Draw(menuFrameWork_t *self)
{
    s3entry_t *e;

    S3_Rest();

    // the players under the demo the cursor is on
    m_s3.list.note = NULL;
    if (m_s3.list.numItems) {
        e = m_s3.list.items[m_s3.list.curvalue];
        if (e->players[0] && strcmp(e->players, S3_NONE))
            m_s3.list.note = e->players;
        else if (m_s3.peeking && !strcmp(m_s3.peek_key, e->key))
            m_s3.list.note = "looking who is in it...";
    }
    Menu_Draw(self);
    // the bottom line is the menu's status, centred, and the count at
    // the right; the keys, a note or the download's bar go on the line
    // above, clear of them
    if (uis.width >= 640)
        UI_DrawString(uis.width, uis.height - CONCHAR_HEIGHT, UI_RIGHT, m_s3.status);
    if (m_s3.fetching)
        S3_DrawProgress(uis.height - CONCHAR_HEIGHT * 2);
    else if (*m_s3.note)
        UI_DrawString(0, uis.height - CONCHAR_HEIGHT * 2, UI_LEFT | UI_ALTCOLOR,
                      *m_s3.filter ? va("filter: %s   %s", m_s3.filter, m_s3.note) : m_s3.note);
    else if (*m_s3.filter)
        UI_DrawString(0, uis.height - CONCHAR_HEIGHT * 2, UI_LEFT | UI_ALTCOLOR,
                      va("filter: %s   (Backspace edits, Escape clears)", m_s3.filter));
    else
        UI_DrawString(0, uis.height - CONCHAR_HEIGHT * 2, UI_LEFT | UI_ALTCOLOR,
                      "Left/Right: month   Tab: source   type: filter by name or player   F5: list again");
}

static void S3_Expose(menuFrameWork_t *self)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    if (!m_s3.month[0] && tm)
        S3_SetMonth(tm->tm_year, tm->tm_mon);
    Q_strlcpy(m_s3.filter, s3_prefilter, sizeof(m_s3.filter));
    s3_prefilter[0] = 0;
    if (!m_s3.fetching)
        m_s3.note[0] = 0;
    S3_ListStart();
}

// aq2world [filter]: the list, filtered from the start - the jump mod's
// pathing menu opens it on the map's name
static void S3_Open_f(void)
{
    Q_strlcpy(s3_prefilter, Cmd_Argc() > 1 ? Cmd_Argv(1) : "", sizeof(s3_prefilter));
    UI_PushMenu(&m_s3.menu);
}

static void S3_Pop(menuFrameWork_t *self)
{
    int i;

    // a download goes on; a listing or a peek is dropped
    if (m_s3.listing || m_s3.peeking) {
        HTTP_FetchCancel();
        m_s3.listing = m_s3.peeking = false;
        Z_Freep(&m_s3.raw);
    }
    for (i = 0; i < S3_PAGES_KEPT; i++)
        if (m_s3.pages[i].raw && m_s3.pages[i].dirty)
            S3_PageWrite(&m_s3.pages[i]);
    S3_FreeList();
}

static void S3_Free(menuFrameWork_t *self)
{
    int i;

    S3_Pop(self);
    for (i = 0; i < S3_PAGES_KEPT; i++)
        if (m_s3.pages[i].raw)
            S3_PageFree(&m_s3.pages[i]);
    Z_Free(m_s3.menu.items);
    memset(&m_s3, 0, sizeof(m_s3));
}

void M_Menu_S3Demos(void)
{
    ui_s3demos_url = Cvar_Get("ui_s3demos_url", S3_URL, 0);

    m_s3.menu.name = "s3demos";
    m_s3.menu.title = m_s3.title;
    Q_strlcpy(m_s3.title, "aq2world demos", sizeof(m_s3.title));

    m_s3.menu.draw       = S3_Draw;
    m_s3.menu.expose     = S3_Expose;
    m_s3.menu.pop        = S3_Pop;
    m_s3.menu.size       = S3_Size;
    m_s3.menu.keydown    = S3_Keydown;
    m_s3.menu.charevent  = S3_CharEvent;
    m_s3.menu.free       = S3_Free;
    m_s3.menu.image      = uis.backgroundHandle;
    m_s3.menu.color.u32  = uis.color.background.u32;
    m_s3.menu.transparent = uis.transparent;

    m_s3.list.generic.type   = MTYPE_LIST;
    m_s3.list.generic.flags  = QMF_HASFOCUS;
    m_s3.list.generic.activate = S3_Activate;
    m_s3.list.generic.change = S3_Change;
    m_s3.list.numcolumns     = COL_MAX;
    m_s3.list.extrasize      = S3_EXTRASIZE;
    m_s3.list.mlFlags        = MLF_HEADER | MLF_SCROLLBAR | MLF_COLOR;

    m_s3.list.columns[COL_DATE].name    = "Played";
    m_s3.list.columns[COL_DATE].uiFlags = UI_LEFT;
    m_s3.list.columns[COL_TEAMS].name   = "Teams";
    m_s3.list.columns[COL_TEAMS].uiFlags = UI_LEFT;
    m_s3.list.columns[COL_MAP].name     = "Map";
    m_s3.list.columns[COL_MAP].uiFlags  = UI_CENTER;
    m_s3.list.columns[COL_SIZE].name    = "Size";
    m_s3.list.columns[COL_SIZE].uiFlags = UI_RIGHT;
    m_s3.list.columns[COL_HAVE].name    = "Have";
    m_s3.list.columns[COL_HAVE].uiFlags = UI_CENTER;

    Menu_AddItem(&m_s3.menu, &m_s3.list);

    List_Append(&ui_menus, &m_s3.menu.entry);
    Cmd_AddCommand("aq2world", S3_Open_f);
}
