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

/*
=======================================================================

AQ2WORLD DEMOS

The servers' demos, as the aq2world S3 bucket lists them, a source and
a month at a time (the keys carry the date, and a listing is capped at
a thousand keys, so the month is the unit): the vrol pickups, the clan
war pickups, and de.aq2.rocks. Left and Right move a month, Tab the
source, letters filter the list as the demo browser does, and Enter
downloads the one under the cursor into demos/ - gunzipped, so it plays
and reads like any demo there - or plays it when it is there already.
Only what is picked is downloaded. A listing is kept for the session
and in s3cache/ on disk: the month being played is asked again after
ten minutes, an older one after a day.

=======================================================================
*/

#define S3_URL          "https://aq2world.s3.amazonaws.com"
#define S3_NS           "http://s3.amazonaws.com/doc/2006-03-01/"
#define S3_MAX_KEYS     4096        // of a month
#define S3_MAX_PAGES    8           // listings of a thousand a month may take
#define S3_CACHE_DIR    "s3cache"
#define S3_TTL_NOW      (10 * 60)   // seconds a listing of this month is good for
#define S3_TTL_PAST     (24 * 60 * 60)
#define S3_PAGES_KEPT   16          // months kept in memory
#define S3_EXTRASIZE    q_offsetof(s3entry_t, text)
#define S3_KEY_MAX      192         // a key with its directory: longer than MAX_QPATH

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
} s3raw_t;

// a month's listing, as fetched
typedef struct {
    int     source;
    char    month[8];               // YYYY-MM
    time_t  fetched;
    int     count;
    s3raw_t *raw;
    unsigned used;                  // for the eviction
} s3page_t;

// a row
typedef struct {
    int64_t size;
    time_t  stamp;
    bool    have;                   // in demos/ already
    char    key[S3_KEY_MAX];
    char    local[S3_KEY_MAX];      // its name in demos/
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
    int             widest_teams, widest_map;
    char            title[64];
    char            status[64];
    char            note[128];      // the bottom left: the filter, a download, an error

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
    unsigned        fetch_shown;

    s3page_t        pages[S3_PAGES_KEPT];
    unsigned        used;
} m_s3_t;

static m_s3_t   m_s3;

static cvar_t   *ui_s3demos_url;

static void S3_BuildList(void);
static void S3_ListStart(void);

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

static bool S3_Have(const char *local)
{
    // the bot's mirror on the laptop prefixes its copies with hl_
    return !os_access(va("%s/demos/%s", fs_gamedir, local), F_OK)
        || !os_access(va("%s/demos/hl_%s", fs_gamedir, local), F_OK);
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

static time_t S3_Ttl(const char *month)
{
    return strcmp(month, m_s3.month) || !m_s3.listing ? S3_TTL_PAST : S3_TTL_NOW;
}

// the month's listing from this session, or the disk; NULL when stale or absent
static s3page_t *S3_PageFind(int source, const char *month, bool fresh)
{
    time_t now = time(NULL), ttl;
    char path[MAX_OSPATH], line[MAX_STRING_CHARS], stamp[32];
    s3page_t *page = NULL, *oldest = NULL;
    FILE *f;
    int i, count = 0;
    long long fetched = 0, size;

    for (i = 0; i < S3_PAGES_KEPT; i++) {
        s3page_t *pg = &m_s3.pages[i];

        if (pg->raw && pg->source == source && !strcmp(pg->month, month))
            page = pg;
        if (!oldest || pg->used < oldest->used)
            oldest = pg;
    }

    // this month is asked again after a while, an old one after a day
    ttl = strncmp(month, m_s3.month, 7) ? S3_TTL_PAST : S3_TTL_NOW;
    if (page) {
        if (!fresh || now - page->fetched <= ttl) {
            page->used = ++m_s3.used;
            return page;
        }
        Z_Free(page->raw);
        page->raw = NULL;
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
    page = oldest;
    Z_Free(page->raw);
    page->raw = UI_Malloc(sizeof(*page->raw) * S3_MAX_KEYS);
    while (count < S3_MAX_KEYS && fgets(line, sizeof(line), f)) {
        s3raw_t *r = &page->raw[count];
        char *nl = strchr(line, '\n');

        if (nl)
            *nl = 0;
        if (sscanf(line, "%lld %31s", &size, stamp) != 2)
            continue;
        if (!(nl = strchr(line, ' ')) || !(nl = strchr(nl + 1, ' ')))
            continue;
        Q_strlcpy(r->key, nl + 1, sizeof(r->key));
        r->size = size;
        r->stamp = atoll(stamp);
        count++;
    }
    fclose(f);
    page->source = source;
    Q_strlcpy(page->month, month, sizeof(page->month));
    page->fetched = fetched;
    page->count = count;
    page->used = ++m_s3.used;
    return page;
}

static s3page_t *S3_PageStore(int source, const char *month, s3raw_t *raw, int count)
{
    char path[MAX_OSPATH];
    s3page_t *page = NULL, *oldest = NULL;
    FILE *f;
    int i;

    for (i = 0; i < S3_PAGES_KEPT; i++) {
        s3page_t *pg = &m_s3.pages[i];

        if (pg->raw && pg->source == source && !strcmp(pg->month, month))
            page = pg;
        if (!oldest || pg->used < oldest->used)
            oldest = pg;
    }
    if (!page)
        page = oldest;
    Z_Free(page->raw);
    page->raw = raw;
    page->count = count;
    page->source = source;
    Q_strlcpy(page->month, month, sizeof(page->month));
    page->fetched = time(NULL);
    page->used = ++m_s3.used;

    S3_CachePath(source, month, path, sizeof(path));
    if (FS_CreatePath(path))
        return page;
    f = fopen(path, "w");
    if (!f)
        return page;
    fprintf(f, "fetched %lld\n", (long long)page->fetched);
    for (i = 0; i < count; i++)
        fprintf(f, "%lld %lld %s\n", (long long)raw[i].size, (long long)raw[i].stamp, raw[i].key);
    fclose(f);
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

// a ...T...Z stamp as a time
static time_t S3_Stamp(const char *s)
{
    struct tm tm;
    int n;

    memset(&tm, 0, sizeof(tm));
    n = sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    if (n != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return timegm(&tm);
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

static void S3_ListRequest(void)
{
    char prefix[2][MAX_QPATH], enc[MAX_QPATH * 3], url[MAX_STRING_CHARS];
    int n = S3_Prefixes(m_s3.source, m_s3.month, prefix);

    if (m_s3.list_prefix >= n) {
        // done: into the cache, onto the screen
        s3raw_t *raw = m_s3.raw;
        int count = m_s3.raw_count;

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
        char err[128];

        HTTP_FetchState(NULL, NULL, err, sizeof(err));
        Q_snprintf(m_s3.note, sizeof(m_s3.note), "listing failed: %s", err[0] ? err : "a download is up");
        m_s3.listing = false;
        Z_Free(m_s3.raw);
        m_s3.raw = NULL;
        S3_BuildList();
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
        Q_snprintf(m_s3.note, sizeof(m_s3.note), "listing failed: %s", err);
        m_s3.listing = false;
        Z_Free(m_s3.raw);
        m_s3.raw = NULL;
        S3_BuildList();
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

// the month's listing: from the cache when it is fresh, else fetched
static void S3_ListStart(void)
{
    s3page_t *page = S3_PageFind(m_s3.source, m_s3.month, true);

    if (page) {
        S3_BuildList();
        return;
    }
    if (m_s3.listing || m_s3.fetching) {
        // the download first; the listing is asked for again when it is done
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
}

static int S3_ScoreCmp(const void *p1, const void *p2)
{
    const s3entry_t *e1 = *(const s3entry_t **)p1;
    const s3entry_t *e2 = *(const s3entry_t **)p2;
    int s1 = Fuzzy_Score(m_s3.filter, S3_Base(e1->key));
    int s2 = Fuzzy_Score(m_s3.filter, S3_Base(e2->key));

    if (s1 != s2)
        return s2 - s1;
    return strcmp(S3_Base(e2->key), S3_Base(e1->key));
}

static void S3_ApplyFilter(void)
{
    int i, n = 0;

    for (i = 0; i < m_s3.numAll; i++) {
        s3entry_t *e = m_s3.all[i];

        if (!*m_s3.filter || Fuzzy_Score(m_s3.filter, S3_Base(e->key)) >= 0)
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

static s3entry_t *S3_Entry(const s3raw_t *r)
{
    char date[32], teams[MAX_QPATH], map[MAX_QPATH], size[16], local[S3_KEY_MAX];
    s3entry_t *e;
    bool have;
    size_t len;

    S3_Parse(r->key, date, sizeof(date), teams, sizeof(teams), map, sizeof(map));
    S3_LocalName(r->key, local, sizeof(local));
    have = S3_Have(local);
    Com_FormatSize(size, sizeof(size), r->size);
    e = UI_FormatColumns(S3_EXTRASIZE, date, teams, map, size, have ? "yes" : "", NULL);
    e->size = r->size;
    e->stamp = r->stamp;
    e->have = have;
    Q_strlcpy(e->key, r->key, sizeof(e->key));
    Q_strlcpy(e->local, local, sizeof(e->local));

    len = strlen(teams);
    if (len > 24)
        len = 24;
    if (len > m_s3.widest_teams)
        m_s3.widest_teams = len;
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
    m_s3.widest_teams = 5;
    m_s3.widest_map = 3;

    if (raw && count > 1)
        qsort(raw, count, sizeof(*raw), S3_RawCmp);
    m_s3.all = UI_Malloc(sizeof(void *) * (count + 1));
    m_s3.numAll = 0;
    for (i = 0; i < count; i++)
        m_s3.all[m_s3.numAll++] = S3_Entry(&raw[i]);
    m_s3.list.items = UI_Malloc(sizeof(void *) * (count + 1));

    Q_snprintf(m_s3.title, sizeof(m_s3.title), "aq2world demos: %s, %s", s3_sources[m_s3.source].name, m_s3.month);
    m_s3.menu.size(&m_s3.menu);
    S3_ApplyFilter();
}

/*
=======================================================================
THE DOWNLOAD
=======================================================================
*/

static menuSound_t S3_Fetch(s3entry_t *e)
{
    char enc[S3_KEY_MAX * 3], url[MAX_STRING_CHARS], path[MAX_OSPATH], err[128];
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
    if (strchr(e->local, '"') || strchr(e->local, ';') || strstr(e->local, "..")) {
        Q_strlcpy(m_s3.note, "that name will not do as a file's", sizeof(m_s3.note));
        return QMS_BEEP;
    }

    S3_UrlEncode(e->key, enc, sizeof(enc));
    Q_snprintf(url, sizeof(url), "%s/%s", ui_s3demos_url->string, enc);
    Q_snprintf(path, sizeof(path), "demos/%s", e->local);
    if (!HTTP_FetchStart(url, path, gz)) {
        HTTP_FetchState(NULL, NULL, err, sizeof(err));
        Q_snprintf(m_s3.note, sizeof(m_s3.note), "download failed: %s", err);
        return QMS_BEEP;
    }
    m_s3.fetching = true;
    Q_strlcpy(m_s3.fetch_key, e->key, sizeof(m_s3.fetch_key));
    Q_strlcpy(m_s3.fetch_local, e->local, sizeof(m_s3.fetch_local));
    Q_snprintf(m_s3.note, sizeof(m_s3.note), "downloading %s...", e->local);
    return QMS_IN;
}

static void S3_FetchDone(void)
{
    char err[128];
    int i;

    if (HTTP_FetchState(NULL, NULL, err, sizeof(err)) == FETCH_DONE) {
        Q_snprintf(m_s3.note, sizeof(m_s3.note), "%s is in demos/: Enter plays it", m_s3.fetch_local);
        for (i = 0; i < m_s3.numAll; i++) {
            s3entry_t *e = m_s3.all[i];

            if (!strcmp(e->key, m_s3.fetch_key) && !e->have) {
                e->have = true;
                // the last column, after four NULs
                char *p = e->text;
                int k;
                for (k = 0; k < COL_HAVE; k++)
                    p += strlen(p) + 1;
                if (strlen(p) >= 3)
                    strcpy(p, "yes");
                else
                    *p = '*';   // no room: a mark
            }
        }
    } else {
        Q_snprintf(m_s3.note, sizeof(m_s3.note), "%s: download failed: %s", m_s3.fetch_local, err);
    }
    HTTP_FetchTake(NULL);
    m_s3.fetching = false;
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
    m_s3.menu.status = e->have ? "Press Enter to play the demo" : "Press Enter to download the demo";
    return QMS_SILENT;
}

static menuSound_t S3_Activate(menuCommon_t *self)
{
    s3entry_t *e;

    if (!m_s3.list.numItems)
        return QMS_BEEP;
    e = m_s3.list.items[m_s3.list.curvalue];
    if (e->have) {
        if (strchr(e->local, '"') || strchr(e->local, ';'))
            return QMS_BEEP;
        Cbuf_AddText(&cmd_buffer, va("demo \"/demos/%s%s\"\n",
            os_access(va("%s/demos/%s", fs_gamedir, e->local), F_OK) ? "hl_" : "", e->local));
        return QMS_SILENT;
    }
    return S3_Fetch(e);
}

static void S3_Size(menuFrameWork_t *self)
{
    int w1, w2;

    m_s3.list.generic.x      = 0;
    m_s3.list.generic.y      = CONCHAR_HEIGHT;
    m_s3.list.generic.width  = 0;
    m_s3.list.generic.height = uis.height - CONCHAR_HEIGHT * 2 - 1;

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
            char path[MAX_OSPATH];
            int i;

            for (i = 0; i < S3_PAGES_KEPT; i++)
                if (m_s3.pages[i].raw && m_s3.pages[i].source == m_s3.source
                    && !strcmp(m_s3.pages[i].month, m_s3.month))
                    m_s3.pages[i].fetched = 0;
            S3_CachePath(m_s3.source, m_s3.month, path, sizeof(path));
            remove(path);
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

// each frame: a listing or a download under way is followed
static void S3_Draw(menuFrameWork_t *self)
{
    fetch_state_t state;
    int64_t got, total;
    char size[16];

    if (m_s3.listing || m_s3.fetching) {
        state = HTTP_FetchState(&got, &total, NULL, 0);
        if (state == FETCH_DONE || state == FETCH_FAILED) {
            if (m_s3.listing)
                S3_ListGot();
            else
                S3_FetchDone();
            // a month asked for during the download
            if (!m_s3.listing && !m_s3.fetching && !S3_PageFind(m_s3.source, m_s3.month, true))
                S3_ListStart();
        } else if (m_s3.fetching && uis.realtime - m_s3.fetch_shown > 200) {
            m_s3.fetch_shown = uis.realtime;
            Com_FormatSize(size, sizeof(size), got);
            if (total > 0)
                Q_snprintf(m_s3.note, sizeof(m_s3.note), "downloading %s: %d%%, %s",
                           m_s3.fetch_local, (int)(got * 100 / total), size);
            else
                Q_snprintf(m_s3.note, sizeof(m_s3.note), "downloading %s: %s", m_s3.fetch_local, size);
        } else if (m_s3.listing && m_s3.raw_count && m_s3.list.numItems != m_s3.raw_count) {
            // what has come in so far, a page at a time
        }
    }

    Menu_Draw(self);
    if (uis.width >= 640)
        UI_DrawString(uis.width, uis.height - CONCHAR_HEIGHT, UI_RIGHT, m_s3.status);
    if (*m_s3.filter)
        UI_DrawString(0, uis.height - CONCHAR_HEIGHT, UI_LEFT | UI_ALTCOLOR,
                      va("filter: %s   %s", m_s3.filter, m_s3.note));
    else if (*m_s3.note)
        UI_DrawString(0, uis.height - CONCHAR_HEIGHT, UI_LEFT | UI_ALTCOLOR, m_s3.note);
    else
        UI_DrawString(0, uis.height - CONCHAR_HEIGHT, UI_LEFT | UI_ALTCOLOR,
                      "Left/Right: month   Tab: source   type to filter   F5: list again");
}

static void S3_Expose(menuFrameWork_t *self)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    if (!m_s3.month[0] && tm)
        S3_SetMonth(tm->tm_year, tm->tm_mon);
    m_s3.filter[0] = 0;
    if (!m_s3.fetching)
        m_s3.note[0] = 0;
    S3_ListStart();
}

static void S3_Pop(menuFrameWork_t *self)
{
    // a download goes on; a listing is dropped
    if (m_s3.listing) {
        HTTP_FetchCancel();
        m_s3.listing = false;
        Z_Free(m_s3.raw);
        m_s3.raw = NULL;
    }
    S3_FreeList();
}

static void S3_Free(menuFrameWork_t *self)
{
    int i;

    S3_Pop(self);
    for (i = 0; i < S3_PAGES_KEPT; i++)
        Z_Free(m_s3.pages[i].raw);
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
    m_s3.list.mlFlags        = MLF_HEADER | MLF_SCROLLBAR;

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
}

