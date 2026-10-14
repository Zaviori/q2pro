/*
Copyright (C) 1997-2001 Id Software, Inc.

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

//
// cl_download.c -- queue manager and UDP downloads
//

#include "client.h"
#include "common/mdfour.h"
#include "format/md2.h"
#include "format/sp2.h"

typedef enum {
    PRECACHE_MODELS,
    PRECACHE_OTHER,
    PRECACHE_MAP,
    PRECACHE_FINAL
} precache_t;

static precache_t precache_check;
static int precache_sexed_sounds[MAX_SOUNDS];
static int precache_sexed_total;

// Track if we've already attempted a CRC-based map re-download
static bool map_crc_retry_attempted = false;

/*
A map we have that differs from the server's (cl_map_override): the
server's copy is fetched into <gamedir>/override, which the filesystem
searches before the paks - a loose maps/foo.bsp beside a pak's loses to
it, and the retry above, which deleted the local file and downloaded
again, could not touch a map in a pak. 1 fetches without asking; 0 (the
default) asks in the console, where `yes` fetches and `no` disconnects;
-1 keeps the local map and reports the mismatch as before. The question
holds the precache at PRECACHE_MAP until it is answered.
*/
static cvar_t *cl_map_override;
static char map_ask_path[MAX_QPATH];    // the map asked about, while asking
static bool map_asking;
static void map_ask_clear(void);
// The fetch: over HTTP first when there is a download server, but AQtion
// servers advertise the public asset host by default, whose copy is the
// one we have - so what arrived is checked against the server's checksum
// at PRECACHE_MAP, and a second fetch goes to the game server itself.
static char map_fetch_path[MAX_QPATH];
static int  map_fetch_tries;
static bool map_fetch_http;             // the last fetch went over HTTP

cvar_t   *r_override_textures;
cvar_t   *r_texture_overrides;

/*
===============
CL_QueueDownload

Adds new download path into queue, incrementing pending count.
Entry will stay in queue for entire lifetime of server connection,
to make sure each path is tried exactly once.
===============
*/
int CL_QueueDownloadEx(const char *path, dltype_t type, bool override, bool udp_only)
{
    dlqueue_t *q;
    size_t len;

    FOR_EACH_DLQ(q) {
        // avoid sending duplicate requests
        if (!FS_pathcmp(path, q->path)) {
            Com_DDPrintf("%s: %s [DUP]\n", __func__, path);
            return Q_ERR(EEXIST);
        }
    }

    len = strlen(path);
    Q_assert(len < MAX_QPATH);

    q = Z_Malloc(sizeof(*q) + len);
    memcpy(q->path, path, len + 1);
    q->type = type;
    q->state = DL_PENDING;
    q->override = override;
    q->udp_only = udp_only;

#if USE_CURL
    // paks get bumped to the top and HTTP switches to single downloading.
    // this prevents someone on 28k dialup trying to do both the main .pak
    // and referenced configstrings data at once.
    if (type == DL_PAK) {
        dlqueue_t *p;

        FOR_EACH_DLQ(p) {
            if (p->type != DL_PAK)
                break;
        }
        List_Append(&p->entry, &q->entry);
    } else
#endif
        List_Append(&cls.download.queue, &q->entry);

    cls.download.pending++;
    Com_DPrintf("%s: %s [%d]\n", __func__, path, cls.download.pending);
    return Q_ERR_SUCCESS;
}

int CL_QueueDownload(const char *path, dltype_t type)
{
    return CL_QueueDownloadEx(path, type, false, false);
}

/*
===============
CL_IgnoreDownload

Returns true if specified path matches against an entry in download ignore
list. Path should be converted to lower case by the caller.
===============
*/
bool CL_IgnoreDownload(const char *path)
{
    string_entry_t *entry;

    if (!strncmp(path, CONST_STR_LEN("save/")))
        return true;
    if (!strncmp(path, CONST_STR_LEN("music/")))
        return true;
    if (!strncmp(path, CONST_STR_LEN("video/")))
        return true;

    for (entry = cls.download.ignores; entry; entry = entry->next) {
        if (Com_WildCmp(entry->string, path)) {
            return true;
        }
    }

    return false;
}

/*
===============
CL_FinishDownload

Mark the queue entry as done, decrementing pending count.
===============
*/
void CL_FinishDownload(dlqueue_t *q)
{
    Q_assert(q);
    Q_assert(q->state == DL_PENDING || q->state == DL_RUNNING);
    Q_assert(cls.download.pending > 0);

    q->state = DL_DONE;
    cls.download.pending--;
    Com_DPrintf("%s: %s [%d]\n", __func__, q->path, cls.download.pending);
}

/*
===============
CL_CleanupDownloads

Disconnected from server, clean up.
===============
*/
void CL_CleanupDownloads(void)
{
    dlqueue_t *q, *n;

    HTTP_CleanupDownloads();

    FOR_EACH_DLQ_SAFE(q, n) {
        Z_Free(q);
    }

    List_Init(&cls.download.queue);
    cls.download.pending = 0;

    map_crc_retry_attempted = false;  // Reset retry flag on disconnect
    map_ask_clear();
    map_fetch_tries = 0;

    cls.download.current = NULL;
    cls.download.percent = 0;
    cls.download.position = 0;

    if (cls.download.file) {
        FS_CloseFile(cls.download.file);
        cls.download.file = 0;
    }

    cls.download.temp[0] = 0;

#if USE_ZLIB
    inflateEnd(&cls.download.z);
#endif
}

/*
===============
CL_LoadDownloadIgnores

Allow mods to provide a list of paths that are known to be non-existent and
should never be downloaded (e.g. model specific sounds).
===============
*/
void CL_LoadDownloadIgnores(void)
{
    CL_LoadFilterList(&cls.download.ignores, "download-ignores.txt", "#/", MAX_QPATH);
}

static bool start_udp_download(dlqueue_t *q)
{
    size_t len;
    qhandle_t f;
    int64_t ret;

    len = strlen(q->path);
    Q_assert(len < MAX_QPATH);

    // download to a temp name, and only rename
    // to the real name when done, so if interrupted
    // a runt file wont be left; the server's version of a file we have
    // goes under override/
    Q_concat(cls.download.temp, sizeof(cls.download.temp),
             q->override ? "override/" : "", q->path, ".tmp");

    // check to see if we already have a tmp for this file, if so, try to resume
    // open the file if not opened yet
    ret = FS_OpenFile(cls.download.temp, &f, FS_MODE_RDWR);
    if (ret > INT_MAX) {
        FS_CloseFile(f);
        ret = Q_ERR(EFBIG);
    }
    if (ret >= 0) {  // it exists
        cls.download.file = f;
        cls.download.position = ret;
        // give the server an offset to start the download
        Com_DPrintf("[UDP] Resuming %s\n", q->path);
#if USE_ZLIB
        if (cls.serverProtocol == PROTOCOL_VERSION_R1Q2)
            CL_ClientCommand(va("download \"%s\" %d udp-zlib", q->path, (int)ret));
        else
#endif
            CL_ClientCommand(va("download \"%s\" %d", q->path, (int)ret));
    } else if (ret == Q_ERR(ENOENT)) {  // it doesn't exist
        Com_DPrintf("[UDP] Downloading %s\n", q->path);
#if USE_ZLIB
        if (cls.serverProtocol == PROTOCOL_VERSION_R1Q2)
            CL_ClientCommand(va("download \"%s\" %d udp-zlib", q->path, 0));
        else
#endif
            CL_ClientCommand(va("download \"%s\"", q->path));
    } else { // error happened
        Com_EPrintf("[UDP] Couldn't open %s for appending: %s\n",
                    cls.download.temp, Q_ErrorString(ret));
        CL_FinishDownload(q);
        return false;
    }

    q->state = DL_RUNNING;
    cls.download.current = q;
    return true;
}

/*
===============
CL_StartNextDownload

Start another UDP download if possible
===============
*/
void CL_StartNextDownload(void)
{
    dlqueue_t *q;

    if (!cls.download.pending || cls.download.current) {
        return;
    }

    FOR_EACH_DLQ(q) {
        // HTTP's entries are its own: with it up, only a fetch from the
        // game server itself (udp_only) goes this way
        if (q->state == DL_PENDING && (q->udp_only || !HTTP_Downloading())) {
            if (start_udp_download(q)) {
                break;
            }
        }
    }
}

static void finish_udp_download(const char *msg)
{
    dlqueue_t *q = cls.download.current;

    // finished with current path
    CL_FinishDownload(q);

    cls.download.current = NULL;
    cls.download.percent = 0;
    cls.download.position = 0;

    if (cls.download.file) {
        FS_CloseFile(cls.download.file);
        cls.download.file = 0;
    }

    cls.download.temp[0] = 0;

#if USE_ZLIB
    inflateReset(&cls.download.z);
#endif

    if (msg) {
        Com_Printf("[UDP] %s [%s] [%d remaining file%s]\n",
                   q->path, msg, cls.download.pending,
                   cls.download.pending == 1 ? "" : "s");
    }

    // get another file if needed
    CL_RequestNextDownload();
    CL_StartNextDownload();
}

static bool write_udp_download(const byte *data, int size)
{
    int ret;

    ret = FS_Write(data, size, cls.download.file);
    if (ret != size) {
        Com_EPrintf("[UDP] Couldn't write %s: %s\n",
                    cls.download.temp, Q_ErrorString(ret));
        finish_udp_download(NULL);
        return false;
    }

    return true;
}

#if USE_ZLIB
// handles both continuous deflate stream for entire download and chunked
// per-packet streams for compatibility.
static bool inflate_udp_download(const byte *data, int size, int decompressed_size)
{
    z_streamp   z = &cls.download.z;
    byte        buffer[0x10000];
    int         ret;

    // initialize stream if not done yet
    Q_assert(z->state || inflateInit2(z, -MAX_WBITS) == Z_OK);

    if (!size)
        return true;

    // run inflate() until output buffer not full
    z->next_in = (Bytef *)data;
    z->avail_in = size;
    do {
        z->next_out = buffer;
        z->avail_out = sizeof(buffer);

        ret = inflate(z, Z_SYNC_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            Com_EPrintf("[UDP] Error %d decompressing download\n", ret);
            finish_udp_download(NULL);
            return false;
        }

        if (!write_udp_download(buffer, sizeof(buffer) - z->avail_out))
            return false;
    } while (z->avail_out == 0);

    // check decompressed size if known
    if (decompressed_size > 0 && decompressed_size != z->total_out) {
        Com_WPrintf("[UDP] Decompressed size mismatch: expected %d, got %lu\n",
                    decompressed_size, z->total_out);
    }

    // prepare for the next stream if done
    if (ret == Z_STREAM_END)
        inflateReset(z);

    return true;
}
#else
#define inflate_udp_download(data, size, decompressed_size)   false
#endif

/*
=====================
CL_HandleDownload

An UDP download data has been received from the server.
=====================
*/
void CL_HandleDownload(const byte *data, int size, int percent, int decompressed_size)
{
    dlqueue_t *q = cls.download.current;
    int ret;

    if (!q) {
        Com_Error(ERR_DROP, "%s: no download requested", __func__);
    }

    // -1 size means download was aborted
    if (size == -1) {
        if (!percent) {
            finish_udp_download("FAIL");
        } else {
            finish_udp_download("STOP");
        }
        return;
    }

    // open the file if not opened yet
    if (!cls.download.file) {
        ret = FS_OpenFile(cls.download.temp, &cls.download.file, FS_MODE_WRITE);
        if (!cls.download.file) {
            Com_EPrintf("[UDP] Couldn't open %s for writing: %s\n",
                        cls.download.temp, Q_ErrorString(ret));
            finish_udp_download(NULL);
            return;
        }
    }

    // non-zero decompressed_size means deflated download
    // can be -1 if streaming from .pkz
    if (decompressed_size) {
        if (!inflate_udp_download(data, size, decompressed_size))
            return;
    } else {
        if (!write_udp_download(data, size))
            return;
    }

    if (percent != 100) {
        // request next block
        // change display routines by zoid
        cls.download.percent = percent;
        cls.download.position += size;

        CL_ClientCommand("nextdl");
    } else {
        // close the file before renaming
        FS_CloseFile(cls.download.file);
        cls.download.file = 0;

        // rename the temp file to its final name
        char dest[MAX_QPATH + 16];
        Q_concat(dest, sizeof(dest), q->override ? "override/" : "", q->path);
        ret = FS_RenameFile(cls.download.temp, dest);
        if (ret) {
            Com_EPrintf("[UDP] Couldn't rename %s to %s: %s\n",
                        cls.download.temp, dest, Q_ErrorString(ret));
            finish_udp_download(NULL);
        } else {
            finish_udp_download("DONE");
        }
    }
}


/*
===============
CL_CheckDownloadExtension

Only predefined set of filename extensions is allowed,
to prevent the server from uploading arbitrary files.
===============
*/
bool CL_CheckDownloadExtension(const char *ext)
{
    static const char allowed[][4] = {
        "bsp", "dm2", "ent", "jpg", "loc", "md2", "md3", "ogg", "pcx", "png",
        "sp2", "tga", "txt", "wal", "wav",
    };

    for (int i = 0; i < q_countof(allowed); i++)
        if (!Q_stricmp(ext, allowed[i]))
            return true;

    return false;
}

// attempts to start a download from the server if file doesn't exist.
static void map_ask_clear(void)
{
    if (!map_asking)
        return;
    map_asking = false;
    Cmd_RemoveCommand("yes");
    Cmd_RemoveCommand("no");
}

// a finished entry forgotten, so the path can be queued once more
static void queue_forget(const char *path)
{
    dlqueue_t *q, *n;

    FOR_EACH_DLQ_SAFE(q, n) {
        if (!FS_pathcmp(path, q->path) && q->state == DL_DONE) {
            List_Remove(&q->entry);
            Z_Free(q);
        }
    }
}

// the server's copy of the map at `path`, queued under override/: over
// HTTP unless `udp`, from the game server itself then
static bool map_override_fetch(const char *path, bool udp)
{
    int ret;

    Com_Printf("Fetching the server's %s into override/%s\n", path, udp ? " from the server itself" : "");
    Q_strlcpy(map_fetch_path, path, sizeof(map_fetch_path));
    map_crc_retry_attempted = true;     // the load's report is about the fetched copy then
    BSP_Evict(path);                    // a cached copy of ours must not stand in for it
    ret = udp ? Q_ERR(ENOSYS) : HTTP_QueueDownload(path, DL_MAP, true);
    map_fetch_http = ret == Q_ERR_SUCCESS;
    if (ret == Q_ERR(ENOSYS)) {
        ret = CL_QueueDownloadEx(path, DL_MAP, true, udp);
        if (ret == Q_ERR_SUCCESS)
            CL_StartNextDownload();
    }
    if (ret != Q_ERR_SUCCESS) {
        Com_EPrintf("Couldn't queue %s: %s\n", path, Q_ErrorString(ret));
        return false;
    }
    map_fetch_tries++;
    return true;
}

// The fetched copy against the server's checksum: true to go on and load
// it (or report it, if it is still wrong), false while a second fetch,
// from the game server itself, is on its way
static bool map_override_fetched(void)
{
    unsigned server = (unsigned)Q_atoi(cl.configstrings[cl.csr.mapchecksum]);
    unsigned local;
    char full[MAX_OSPATH];
    byte *raw;
    int len;

    len = FS_LoadFile(map_fetch_path, (void **)&raw);
    if (!raw)
        return true;
    local = Com_BlockChecksum(raw, len);
    FS_FreeFile(raw);
    if (local == server) {
        Com_Printf("The server's %s is in override/ now\n", map_fetch_path);
        map_fetch_tries = 0;
        return true;
    }
    if (map_fetch_http && map_fetch_tries == 1) {
        Com_Printf("The download server's %s differs from the server's as well\n", map_fetch_path);
        Q_concat(full, sizeof(full), fs_gamedir, "/override/", map_fetch_path);
        remove(full);       // rename() will not replace it on Windows
        Q_strlcat(full, ".tmp", sizeof(full));
        remove(full);       // and the UDP fetch must not resume onto its bytes
        queue_forget(map_fetch_path);
        return !map_override_fetch(map_fetch_path, true);
    }
    return true;
}

static void map_ask_yes_f(void)
{
    char path[MAX_QPATH];

    if (!map_asking)
        return;
    Q_strlcpy(path, map_ask_path, sizeof(path));
    map_ask_clear();
    map_override_fetch(path, false);
    CL_RequestNextDownload();
}

static void map_ask_no_f(void)
{
    if (!map_asking)
        return;
    map_ask_clear();
    Com_Error(ERR_DROP, "Local %s kept; the server runs another version of it", map_ask_path);
}

static void map_ask(const char *path, unsigned local, unsigned server)
{
    Q_strlcpy(map_ask_path, path, sizeof(map_ask_path));
    if (!map_asking) {
        Cmd_AddCommand("yes", map_ask_yes_f);
        Cmd_AddCommand("no", map_ask_no_f);
    }
    map_asking = true;
    Com_Printf("Local %s differs from the server's (checksum %u here, %u there).\n"
               "Fetch the server's version into override/ and play on it? Type yes or no in the console.\n"
               "(cl_map_override 1 fetches without asking, -1 never)\n", path, local, server);
}

static int check_file_len(const char *path, size_t len, dltype_t type)
{
    char buffer[MAX_QPATH], *ext;
    path_valid_t valid;
    int ret;

    // check for oversize path
    if (len >= MAX_QPATH)
        return Q_ERR(ENAMETOOLONG);

    // normalize path
    len = FS_NormalizePathBuffer(buffer, path, sizeof(buffer));
    valid = FS_ValidatePath(buffer);

    // check path
    if (valid == PATH_INVALID
        || !Q_ispath(buffer[0])
        || !Q_ispath(buffer[len - 1])
        || strstr(buffer, "..")
        || !strchr(buffer, '/')) {
        // some of these checks are too conservative or even redundant
        // once we have normalized the path, however they have to stay for
        // compatibility reasons with older servers (some would even ban us
        // for sending .. for example)
        return Q_ERR_INVALID_PATH;
    }

    // check extension
    ext = COM_FileExtension(buffer);
    if (*ext != '.' || !CL_CheckDownloadExtension(ext + 1))
        return Q_ERR_INVALID_PATH;

    if (FS_FileExists(buffer)) {
        // For map files, validate CRC before skipping download
        if (type == DL_MAP && !map_crc_retry_attempted) {
            // Parse server checksum as signed int (server stores with "%d"), then
            // treat as unsigned so both sides compare as the same 32-bit bit pattern.
            unsigned server_checksum = (unsigned)Q_atoi(cl.configstrings[cl.csr.mapchecksum]);

            if (server_checksum != 0) {  // Server sent a valid checksum
                // Compute local CRC directly from raw file bytes.
                // Do NOT use BSP_Load here: it caches in memory and may return a
                // stale entry if this map was previously loaded (e.g. listen server).
                byte *raw;
                int filelen = FS_LoadFile(buffer, (void **)&raw);
                if (raw) {
                    unsigned local_checksum = Com_BlockChecksum(raw, filelen);
                    FS_FreeFile(raw);

                    if (local_checksum != server_checksum) {
                        // ours stays where it is; the server's goes under
                        // override/, fetched now, asked about, or not at all
                        // (then CL_RegisterBspModels reports the mismatch)
                        if (cl_map_override->integer > 0)
                            map_override_fetch(buffer, false);
                        else if (cl_map_override->integer == 0)
                            map_ask(buffer, local_checksum, server_checksum);
                        return Q_ERR(EEXIST);
                    } else {
                        return Q_ERR(EEXIST);  // CRC matches, skip download
                    }
                } else {
                    // Could not read the file — re-download it
                    char fullpath[MAX_OSPATH];
                    Com_Printf("Could not read local map for CRC check, re-downloading: %s\n", buffer);
                    Q_concat(fullpath, sizeof(fullpath), fs_gamedir, "/", buffer);
                    remove(fullpath);
                    map_crc_retry_attempted = true;
                }
            } else {
                // No server checksum available, skip download
                return Q_ERR(EEXIST);
            }
        } else {
            // Non-map files OR already retried: keep existing behavior
            return Q_ERR(EEXIST);
        }
    }

    if (valid == PATH_MIXED_CASE)
        // convert to lower case to make download server happy
        Q_strlwr(buffer);

    if (CL_IgnoreDownload(buffer))
        return Q_ERR(EPERM);

    ret = HTTP_QueueDownload(buffer, type, false);
    if (ret != Q_ERR(ENOSYS))
        return ret;

    // queue and start legacy UDP download
    ret = CL_QueueDownload(buffer, type);
    if (ret == Q_ERR_SUCCESS)
        CL_StartNextDownload();

    return ret;
}

#define check_file(path, type) \
    check_file_len(path, strlen(path), type)

static void check_skins(const char *name)
{
    size_t          i, num_skins, ofs_skins, end_skins;
    byte            *model;
    size_t          len;
    dmd2header_t    *md2header;
    dsp2header_t    *sp2header;
    char            *md2skin;
    dsp2frame_t     *sp2frame;
    uint32_t        ident;
    char            fn[MAX_QPATH];

    len = FS_LoadFile(name, (void **)&model);
    if (!model) {
        // couldn't load it
        return;
    }

    if (len < sizeof(ident)) {
        // file too small
        goto done;
    }

    // check ident
    ident = LittleLong(*(uint32_t *)model);
    switch (ident) {
    case MD2_IDENT:
        // alias model
        md2header = (dmd2header_t *)model;
        if (len < sizeof(*md2header) ||
            LittleLong(md2header->ident) != MD2_IDENT ||
            LittleLong(md2header->version) != MD2_VERSION) {
            // not an alias model
            goto done;
        }

        num_skins = LittleLong(md2header->num_skins);
        ofs_skins = LittleLong(md2header->ofs_skins);
        end_skins = ofs_skins + num_skins * MD2_MAX_SKINNAME;
        if (num_skins > MD2_MAX_SKINS || end_skins < ofs_skins || end_skins > len) {
            // bad alias model
            goto done;
        }

        md2skin = (char *)model + ofs_skins;
        for (i = 0; i < num_skins; i++) {
            if (!Q_memccpy(fn, md2skin, 0, sizeof(fn))) {
                // bad alias model
                goto done;
            }
            check_file(fn, DL_OTHER);
            md2skin += MD2_MAX_SKINNAME;
        }
        break;

    case SP2_IDENT:
        // sprite model
        sp2header = (dsp2header_t *)model;
        if (len < sizeof(*sp2header) ||
            LittleLong(sp2header->ident) != SP2_IDENT ||
            LittleLong(sp2header->version) != SP2_VERSION) {
            // not a sprite model
            goto done;
        }

        num_skins = LittleLong(sp2header->numframes);
        ofs_skins = sizeof(*sp2header);
        end_skins = ofs_skins + num_skins * sizeof(dsp2frame_t);
        if (num_skins > SP2_MAX_FRAMES || end_skins < ofs_skins || end_skins > len) {
            // bad sprite model
            goto done;
        }

        sp2frame = (dsp2frame_t *)(model + ofs_skins);
        for (i = 0; i < num_skins; i++) {
            if (!Q_memccpy(fn, sp2frame->name, 0, sizeof(fn))) {
                // bad sprite model
                goto done;
            }
            check_file(fn, DL_OTHER);
            sp2frame++;
        }
        break;

    default:
        // unknown file format
        goto done;
    }

done:
    FS_FreeFile(model);
}

static void check_player(const char *name)
{
    char fn[MAX_QPATH], model[MAX_QPATH], skin[MAX_QPATH], *p;
    size_t len;
    int i, j;

    CL_ParsePlayerSkin(NULL, model, skin, name);

    // model
    len = Q_concat(fn, sizeof(fn), "players/", model, "/tris.md2");
    check_file_len(fn, len, DL_OTHER);

    // weapon models
    for (i = 0; i < cl.numWeaponModels; i++) {
        len = Q_concat(fn, sizeof(fn), "players/", model, "/", cl.weaponModels[i]);
        check_file_len(fn, len, DL_OTHER);
    }

    // default weapon skin
    len = Q_concat(fn, sizeof(fn), "players/", model, "/weapon.pcx");
    check_file_len(fn, len, DL_OTHER);

    // skin
    len = Q_concat(fn, sizeof(fn), "players/", model, "/", skin, ".pcx");
    check_file_len(fn, len, DL_OTHER);

    // skin_i
    len = Q_concat(fn, sizeof(fn), "players/", model, "/", skin, "_i.pcx");
    check_file_len(fn, len, DL_OTHER);

    // sexed sounds
    for (i = 0; i < precache_sexed_total; i++) {
        j = precache_sexed_sounds[i];
        p = cl.configstrings[cl.csr.sounds + j];

        if (*p == '*') {
            len = Q_concat(fn, sizeof(fn), "players/", model, "/", p + 1);
            check_file_len(fn, len, DL_OTHER);
        }
    }
}

// for precaching dependencies
static bool downloads_pending(dltype_t type)
{
    dlqueue_t *q;

    // DL_OTHER just checks for any download
    if (type == DL_OTHER) {
        return cls.download.pending;
    }

    // see if there are pending downloads of the given type
    FOR_EACH_DLQ(q) {
        if (q->state != DL_DONE && q->type == type) {
            return true;
        }
    }

    return false;
}

/*
=====================
CL_RequestNextDownload

Runs precache check and dispatches downloads.
=====================
*/
void CL_RequestNextDownload(void)
{
    char fn[MAX_QPATH], *name;
    size_t len;
    int i;

    if (cls.state != ca_connected && cls.state != ca_loading)
        return;

    if (allow_download->integer <= 0 || NET_IsLocalAddress(&cls.serverAddress)) {
        if (precache_check <= PRECACHE_MAP) {
            CL_RegisterBspModels();
        }

        CL_Begin();
        return;
    }

    switch (precache_check) {
    case PRECACHE_MODELS:
        // confirm map
        if (allow_download_maps->integer)
            check_file(cl.configstrings[cl.csr.models + 1], DL_MAP);

        // checking for models
        if (allow_download_models->integer) {
            for (i = 2; i < cl.csr.max_models; i++) {
                name = cl.configstrings[cl.csr.models + i];
                if (!name[0] && i != MODELINDEX_PLAYER) {
                    break;
                }
                if (name[0] == '*' || name[0] == '#' || name[0] == 0) {
                    continue;
                }
                check_file(name, DL_MODEL);
            }
        }

        precache_check = PRECACHE_OTHER;
        // fall through

    case PRECACHE_OTHER:
        if (allow_download_models->integer) {
            if (downloads_pending(DL_MODEL)) {
                // pending downloads (models), let's wait here before we can check skins.
                Com_DPrintf("%s: waiting for models...\n", __func__);
                return;
            }

            for (i = 2; i < cl.csr.max_models; i++) {
                name = cl.configstrings[cl.csr.models + i];
                if (!name[0] && i != MODELINDEX_PLAYER) {
                    break;
                }
                if (name[0] == '*' || name[0] == '#' || name[0] == 0) {
                    continue;
                }
                check_skins(name);
            }

            // check skins for RF_CUSTOMSKIN
            if (cl.csr.extended) {
                for (i = 1; i < cl.csr.max_images; i++) {
                    name = cl.configstrings[cl.csr.images + i];
                    if (!name[0]) {
                        break;
                    }
                    if (name[0] == '/' || name[0] == '\\') {
                        continue;
                    }
                    if (!*COM_FileExtension(name) || !strchr(name, '/')) {
                        continue;
                    }
                    check_file(name, DL_OTHER);
                }
            }
        }

        if (allow_download_sounds->integer) {
            for (i = 1; i < cl.csr.max_sounds; i++) {
                name = cl.configstrings[cl.csr.sounds + i];
                if (!name[0]) {
                    break;
                }
                if (name[0] == '*') {
                    continue;
                }
                if (name[0] == '#') {
                    len = Q_strlcpy(fn, name + 1, sizeof(fn));
                } else {
                    len = Q_concat(fn, sizeof(fn), "sound/", name);
                }
                check_file_len(fn, len, DL_OTHER);
            }
        }

        if (allow_download_pics->integer) {
            for (i = 1; i < cl.csr.max_images; i++) {
                name = cl.configstrings[cl.csr.images + i];
                if (!name[0]) {
                    break;
                }
                if (name[0] == '/' || name[0] == '\\') {
                    len = Q_strlcpy(fn, name + 1, sizeof(fn));
                } else if (cl.csr.extended && *COM_FileExtension(name) && strchr(name, '/')) {
                    continue;
                } else {
                    len = Q_concat(fn, sizeof(fn), "pics/", name, ".pcx");
                }
                check_file_len(fn, len, DL_OTHER);
            }
        }

        if (allow_download_players->integer) {
            // find sexed sounds
            precache_sexed_total = 0;
            for (i = 1; i < cl.csr.max_sounds; i++) {
                if (cl.configstrings[cl.csr.max_sounds + i][0] == '*') {
                    precache_sexed_sounds[precache_sexed_total++] = i;
                }
            }

            for (i = 0; i < MAX_CLIENTS; i++) {
                name = cl.configstrings[cl.csr.playerskins + i];
                if (!name[0]) {
                    continue;
                }
                check_player(name);
            }
        }

        if (allow_download_textures->integer) {
            for (i = 0; i < 6; i++) {
                len = Q_concat(fn, sizeof(fn), "env/", cl.configstrings[CS_SKY], com_env_suf[i], ".tga");
                check_file_len(fn, len, DL_OTHER);
            }
        }

        precache_check = PRECACHE_MAP;
        // fall through

    case PRECACHE_MAP:
        if (map_asking) {
            // the console's question about the map (cl_map_override) is open
            Com_DPrintf("%s: waiting for the map question...\n", __func__);
            return;
        }

        if (downloads_pending(DL_MAP)) {
            // map might still be downloading?
            Com_DPrintf("%s: waiting for map...\n", __func__);
            return;
        }

        // the server's version fetched (cl_map_override): is it, or is
        // a second fetch on its way?
        if (map_fetch_tries && !map_override_fetched())
            return;

        // load the map file before checking textures
        CL_RegisterBspModels();

        if (allow_download_textures->integer) {
            for (i = 0; i < cl.bsp->numtexinfo; i++) {
                if (cl.bsp->texinfo[i].c.flags & SURF_NODRAW)
                    continue;
                if (r_override_textures->integer == 2 || (r_texture_overrides->integer & 16)) {
                    len = Q_concat(fn, sizeof(fn), "textures/", cl.bsp->texinfo[i].name, ".jpg");
                    check_file_len(fn, len, DL_OTHER);
                }
                len = Q_concat(fn, sizeof(fn), "textures/", cl.bsp->texinfo[i].name, ".wal");
                check_file_len(fn, len, DL_OTHER);
            }
        }

        precache_check = PRECACHE_FINAL;
        // fall through

    case PRECACHE_FINAL:
        if (downloads_pending(DL_OTHER)) {
            // pending downloads (possibly textures), let's wait here.
            Com_DPrintf("%s: waiting for others...\n", __func__);
            return;
        }

        // all done, tell server we are ready
        CL_Begin();
        break;

    default:
        Q_assert(!"bad precache_check");
    }
}

void CL_ResetPrecacheCheck(void)
{
    precache_check = PRECACHE_MODELS;
    map_crc_retry_attempted = false;  // Reset retry flag for new connection
    map_ask_clear();
    map_fetch_tries = 0;
}

bool CL_MapRetryAttempted(void)
{
    return map_crc_retry_attempted;
}

/*
===============
CL_Download_f

Request a download from the server
===============
*/
static void CL_Download_f(void)
{
    char *path;
    int ret;

    if (cls.state < ca_connected) {
        Com_Printf("Must be connected to a server.\n");
        return;
    }

    if (allow_download->integer == -1) {
        Com_Printf("Downloads are permanently disabled.\n");
        return;
    }

    if (Cmd_Argc() != 2) {
        Com_Printf("Usage: download <filename>\n");
        return;
    }

    path = Cmd_Argv(1);

    ret = check_file(path, DL_OTHER);
    if (ret) {
        Com_Printf("Couldn't download %s: %s\n", path, Q_ErrorString(ret));
    }
}

void CL_InitDownloads(void)
{
    Cmd_AddCommand("download", CL_Download_f);
    cl_map_override = Cvar_Get("cl_map_override", "0", CVAR_ARCHIVE);

    List_Init(&cls.download.queue);
}
