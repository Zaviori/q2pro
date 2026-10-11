# aq2world demos: the servers' demos from the demo browser

The AQ2 servers upload every match's server demo (.mvd2, gzipped) to
the aq2world S3 bucket. The demo browser's first row, **aq2world: the
servers' demos...**, lists them and downloads the one picked into
`demos/`, where it plays like any demo there and the jump mod's player
pathing can read it.

## Using it

The listing is a source and a month at a time: the keys carry the date,
and a bucket listing is capped at a thousand keys, so a month is the
unit.

| Key | What |
| --- | --- |
| Left / Right | the month before / after (not past this one) |
| Tab | the next source: `pickup` (vrol), `cw-pickup` (the clan war server), `aq2world_eu` (de.aq2.rocks) |
| letters | filter the month's list, fuzzy, by the demo's name; Backspace edits, Escape clears |
| Enter | download the demo under the cursor; play it when it is already in `demos/` |
| F5 | list the month again, whatever the cache has |

The columns are the time played (as the server named the file), the
teams, the map, the size and whether it is in `demos/` already (the
laptop's mirror prefixes its copies with `hl_`; those count). One
download at a time, with its progress on the bottom line; it goes on
if the menu is left. Only what is picked is downloaded - the listing
itself is a few kilobytes of XML.

A month's listing is kept for the session and on disk in `s3cache/`
(`<source>-<YYYY-MM>.txt`): the month being played is asked again after
ten minutes, an older month after a day. `ui_s3demos_url` is the
bucket (`https://aq2world.s3.amazonaws.com`).

## How it is built

- `src/client/ui/s3demos.c` is the menu (`pushmenu s3demos`), modelled
  on the demo browser: a list with the browser's fuzzy filter, the
  month and source state, the cache, and the download's progress.
- The key shapes it takes apart: `pickup_<YYYY-MM-DD>_<HHMMSS>_<teams>_<map>_<port>`
  (`cw_` for the clan war server), de.aq2.rocks's
  `<YYYY-MM-DD>_<HHMMSS>_<teams>_<map>_<port>` until August 2026 and
  `<YYYYMMDD>-<HHMMSS>-TP-MM-<teams>-<port>-<map>` from then; the bucket
  prefix for a month follows the same layouts (`q2bot.py` on the laptop
  has the same table).
- `HTTP_FetchStart` / `HTTP_FetchState` / `HTTP_FetchTake` /
  `HTTP_FetchCancel` in `src/client/http.c`: one fetch on a thread of
  its own, into memory (a listing, with S3's continuation token for the
  next page) or into a file under the game directory, written as
  `.part` and moved into place whole, gunzipped on the way with zlib.
  The menu polls it from its draw.
