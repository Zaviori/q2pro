/*
The players' round-start movement, read out of an .mvd2 demo.

Shared by the game library (the jump mod's "player pathing" menus and
ghost) and the client (which draws the routes as lines in the world): both
parse the same demo with this code and so agree on every trail's index.

Only the old protocol's demos are read, which is what every AQ2 server
records; an extended-limits demo is refused rather than misparsed.
*/

#ifndef MVDPATH_H
#define MVDPATH_H

#include "shared/shared.h"

#define MVP_NAME_MAX        16
#define MVP_ACTION_FRAMES   41      // LIGHTS to ACTION!, the same on every server
#define MVP_AFTER_FRAMES    150     // kept past ACTION!: 15 seconds, at 10 frames a second
#define MVP_MIN_SAMPLES     12      // shorter trails - died in the countdown - are dropped

typedef struct {
    short   origin[3];      // eighths of a unit, as the demo has them
    short   pitch, yaw;     // the view's, as angle16
    short   anim;           // the model's animation frame
    signed char viewheight; // the view's height over the origin, in units
} mvp_sample_t;

typedef struct {
    int     round;          // index into rounds
    int     slot;
    int     team;           // 0 or 1: the order the round's skins were met in
    char    name[MVP_NAME_MAX];
    int     first;          // its samples: index into samples, and how many
    int     count;
    bool    died;           // cut short: the player died inside the window
} mvp_trail_t;

typedef struct {
    int     number;         // from 1, in the order played
    int     lights_f;       // the demo frame of its LIGHTS
    int     action_f;       // of its ACTION!, 0 if it never came
    int     trail_first;    // its trails: index into trails, and how many
    int     trail_count;
    char    skin[2][MAX_QPATH]; // each team's model/skin, as the players wore it
} mvp_round_t;

typedef struct {
    char    name[MVP_NAME_MAX];
    int     trails;         // rounds they played in this demo
} mvp_player_t;

typedef struct {
    char            map[MAX_QPATH];
    int             frames;         // blocks read

    mvp_round_t     *rounds;
    int             round_count;
    mvp_trail_t     *trails;
    int             trail_count;
    mvp_sample_t    *samples;
    int             sample_count;
    mvp_player_t    players[MAX_CLIENTS];   // by name, most rounds first
    int             player_count;
} mvp_demo_t;

// The demo's bytes: a whole file. NULL and a message in err on a demo that
// cannot be read at all; a demo cut short gives what it had.
mvp_demo_t *MvdPath_Parse(const byte *data, size_t len, char *err, size_t errlen);
void MvdPath_Free(mvp_demo_t *demo);

// The trails of one player (by name) with the duplicates folded: the
// rounds they took a route already listed, from the same spawn, are
// counted on its row. Returns how many were listed, their trail indices
// in out[], the duplicates each stands for in dups[]. The routes are
// compared over their first `frames` samples.
int MvdPath_Unique(const mvp_demo_t *demo, const char *name, int frames,
                   int *out, int *dups, int max);

// in units, at the sample
static inline void MvdPath_Origin(const mvp_sample_t *s, vec3_t out)
{
    out[0] = s->origin[0] * 0.125f;
    out[1] = s->origin[1] * 0.125f;
    out[2] = s->origin[2] * 0.125f;
}

// the sample `frames` in, or the trail's last
static inline const mvp_sample_t *MvdPath_At(const mvp_demo_t *demo, const mvp_trail_t *t, int frames)
{
    int i = frames < t->count ? frames : t->count - 1;
    return &demo->samples[t->first + (i < 0 ? 0 : i)];
}

#endif // MVDPATH_H
