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
// console.c

#include "client.h"
#include "common/fuzzy.h"

#define CON_TIMES       16
#define CON_TIMES_MASK  (CON_TIMES - 1)

#define CON_TOTALLINES          1024    // total lines in console scrollback
#define CON_TOTALLINES_MASK     (CON_TOTALLINES - 1)

#define CON_LINEWIDTH   126     // fixed width, do not need more

typedef enum {
    CHAT_NONE,
    CHAT_DEFAULT,
    CHAT_TEAM,
    CHAT_PROMPT     // a question from the game: messageprompt
} chatMode_t;

typedef enum {
    CON_POPUP,
    CON_DEFAULT,
    CON_REMOTE
} consoleMode_t;

typedef struct {
    byte    color;
    byte    ts_len;
    char    text[CON_LINEWIDTH];
} consoleLine_t;

typedef struct {
    consoleLine_t   text[CON_TOTALLINES];

    int     current;        // line where next message will be printed
    int     x;              // offset in current line for next print
    int     display;        // bottom of console displays this line
    int     color;
    int     newline;

    int     linewidth;      // characters across screen
    int     vidWidth, vidHeight;
    float   scale;
    color_t ts_color;

    unsigned    times[CON_TIMES];   // cls.realtime time the line was generated
                                    // for transparent notify lines
    bool    skipNotify;
    bool    initialized;

    qhandle_t   backImage;
    qhandle_t   charsetImage;

    float   currentHeight;  // aproaches scr_conlines at scr_conspeed
    float   destHeight;     // 0.0 to 1.0 lines of console to display

    commandPrompt_t chatPrompt;
    commandPrompt_t prompt;

    chatMode_t chat;
    char promptLabel[64];   // messageprompt: the question,
    char promptHint[64];    // a line on what to answer,
    char promptCmd[64];     // and the command the answer is sent with
    consoleMode_t mode;
    netadr_t remoteAddress;
    char *remotePassword;

    load_state_t loadstate;

    // Ctrl-R: a bash-like search back through the history, fuzzy
    struct {
        bool        active;
        char        query[MAX_FIELD_TEXT];
        const char  *matches[HISTORY_SIZE];     // best first
        int         count, pos;
        char        *saved;     // the line before it, for a cancel
    } search;
} console_t;

static console_t    con;

static const char *Con_SearchMatch(void);

static cvar_t   *con_notifytime;
static cvar_t   *con_notifylines;
static cvar_t   *con_notify_font;
static cvar_t   *con_notify_size;
static cvar_t   *con_text_font;
static cvar_t   *con_hitcolors;
static cvar_t   *con_text_size;
static cvar_t   *con_clock;
static cvar_t   *con_height;
static cvar_t   *con_speed;
static cvar_t   *con_alpha;
static cvar_t   *con_scale;
static cvar_t   *con_font;
static cvar_t   *con_background;
static cvar_t   *con_scroll;
static cvar_t   *con_history;
static cvar_t   *con_timestamps;
static cvar_t   *con_timestampsformat;
static cvar_t   *con_timestampscolor;
static cvar_t   *con_auto_chat;

// ============================================================================

/*
================
Con_SkipNotify
================
*/
void Con_SkipNotify(bool skip)
{
    con.skipNotify = skip;
}

/*
================
Con_ClearTyping
================
*/
void Con_ClearTyping(void)
{
    // clear any typing
    IF_Clear(&con.prompt.inputLine);
    Prompt_ClearState(&con.prompt);
}

/*
================
Con_Close

Instantly removes the console. Unless `force' is true, does not remove the console
if user has typed something into it since the last call to Con_Popup.
================
*/
void Con_Close(bool force)
{
    if (con.mode > CON_POPUP && !force) {
        return;
    }

    // if not connected, console or menu should be up
    if (cls.state < ca_active && !(cls.key_dest & KEY_MENU)) {
        return;
    }

    Con_ClearTyping();
    Con_ClearNotify_f();

    Key_SetDest(cls.key_dest & ~KEY_CONSOLE);

    con.destHeight = con.currentHeight = 0;
    con.mode = CON_POPUP;
    con.chat = CHAT_NONE;
}

/*
================
Con_Popup

Drop to connection screen. Unless `force' is true, does not change console mode to popup.
================
*/
void Con_Popup(bool force)
{
    if (force) {
        con.mode = CON_POPUP;
    }

    Key_SetDest(cls.key_dest | KEY_CONSOLE);
    Con_RunConsole();
}

/*
================
Con_ToggleConsole_f

Toggles console up/down animation.
================
*/
static void toggle_console(consoleMode_t mode, chatMode_t chat)
{
    SCR_EndLoadingPlaque();    // get rid of loading plaque

    Con_ClearTyping();
    Con_ClearNotify_f();

    if (cls.key_dest & KEY_CONSOLE) {
        Key_SetDest(cls.key_dest & ~KEY_CONSOLE);
        con.mode = CON_POPUP;
        con.chat = CHAT_NONE;
        return;
    }

    // toggling console discards chat message
    Key_SetDest((cls.key_dest | KEY_CONSOLE) & ~KEY_MESSAGE);
    con.mode = mode;
    con.chat = chat;
}

void Con_ToggleConsole_f(void)
{
    toggle_console(CON_DEFAULT, CHAT_NONE);
}

/*
================
Con_Clear_f
================
*/
static void Con_Clear_f(void)
{
    memset(con.text, 0, sizeof(con.text));
    con.display = con.current = 0;
    con.newline = '\r';
}

static void Con_Dump_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        FS_File_g("condumps", ".txt", FS_SEARCH_STRIPEXT, ctx);
    }
}

/*
================
Con_Dump_f

Save the console contents out to a file
================
*/
static void Con_Dump_f(void)
{
    int     l;
    qhandle_t f;
    char    name[MAX_OSPATH];

    if (Cmd_Argc() != 2) {
        Com_Printf("Usage: %s <filename>\n", Cmd_Argv(0));
        return;
    }

    f = FS_EasyOpenFile(name, sizeof(name), FS_MODE_WRITE | FS_FLAG_TEXT,
                        "condumps/", Cmd_Argv(1), ".txt");
    if (!f) {
        return;
    }

    // skip empty lines
    for (l = con.current - CON_TOTALLINES + 1; l <= con.current; l++) {
        if (con.text[l & CON_TOTALLINES_MASK].text[0]) {
            break;
        }
    }

    // write the remaining lines
    for (; l <= con.current; l++) {
        char buffer[CON_LINEWIDTH + 1];
        const char *p = con.text[l & CON_TOTALLINES_MASK].text;
        int i;

        for (i = 0; i < CON_LINEWIDTH && p[i]; i++)
            buffer[i] = Q_charascii(p[i]);
        buffer[i] = '\n';

        FS_Write(buffer, i + 1, f);
    }

    if (FS_CloseFile(f))
        Com_EPrintf("Error writing %s\n", name);
    else
        Com_Printf("Dumped console text to %s.\n", name);
}

/*
================
Con_ClearNotify_f
================
*/
void Con_ClearNotify_f(void)
{
    int     i;

    for (i = 0; i < CON_TIMES; i++)
        con.times[i] = 0;
}

/*
================
Con_MessageMode_f
================
*/
static void start_message_mode(chatMode_t mode)
{
    if (cls.state != ca_active || cls.demo.playback) {
        Com_Printf("You must be in a level to chat.\n");
        return;
    }

    // starting messagemode closes console
    if (cls.key_dest & KEY_CONSOLE) {
        Con_Close(true);
    }

    con.chat = mode;
    IF_Replace(&con.chatPrompt.inputLine, COM_StripQuotes(Cmd_RawArgs()));
    Key_SetDest(cls.key_dest | KEY_MESSAGE);
}

static void Con_MessageMode_f(void)
{
    start_message_mode(CHAT_DEFAULT);
}

static void Con_MessageMode2_f(void)
{
    start_message_mode(CHAT_TEAM);
}

/*
================
Con_MessagePrompt_f

messageprompt <label> <hint> <command>: asks for a line of text in a box
under the label, the hint (which may be "") below it, and sends it to
the server as: command "text". Escape sends
the bare command, so the asker knows it was turned down. For the game
to stuff; a client without it forwards the unknown command instead,
which is how the game tells.
================
*/
static void Con_MessagePrompt_f(void)
{
    if (Cmd_Argc() < 4) {
        Com_Printf("Usage: %s <label> <hint> <command>\n", Cmd_Argv(0));
        return;
    }
    if (cls.state != ca_active || cls.demo.playback)
        return;

    if (cls.key_dest & KEY_CONSOLE)
        Con_Close(true);

    Q_strlcpy(con.promptLabel, Cmd_Argv(1), sizeof(con.promptLabel));
    Q_strlcpy(con.promptHint, Cmd_Argv(2), sizeof(con.promptHint));
    Q_strlcpy(con.promptCmd, Cmd_ArgsFrom(3), sizeof(con.promptCmd));
    con.chat = CHAT_PROMPT;
    IF_Clear(&con.chatPrompt.inputLine);
    Key_SetDest(cls.key_dest | KEY_MESSAGE);
}

/*
================
Con_RemoteMode_f
================
*/
static void Con_RemoteMode_f(void)
{
    netadr_t adr;
    char *s;

    if (Cmd_Argc() != 3) {
        Com_Printf("Usage: %s <address> <password>\n", Cmd_Argv(0));
        return;
    }

    s = Cmd_Argv(1);
    if (!NET_StringToAdr(s, &adr, PORT_SERVER)) {
        Com_Printf("Bad address: %s\n", s);
        return;
    }

    s = Cmd_Argv(2);

    if (!(cls.key_dest & KEY_CONSOLE)) {
        toggle_console(CON_REMOTE, CHAT_NONE);
    } else {
        con.mode = CON_REMOTE;
        con.chat = CHAT_NONE;
    }

    Z_Free(con.remotePassword);

    con.remoteAddress = adr;
    con.remotePassword = Z_CopyString(s);
}

static void CL_RemoteMode_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        Com_Address_g(ctx);
    }
}

/*
================
Con_CheckResize

If the line width has changed, reformat the buffer.
================
*/
void Con_CheckResize(void)
{
    con.scale = R_ClampScale(con_scale);

    con.vidWidth = Q_rint(r_config.width * con.scale);
    con.vidHeight = Q_rint(r_config.height * con.scale);

    con.linewidth = Q_clip(con.vidWidth / CONCHAR_WIDTH - 2, 0, CON_LINEWIDTH);
    con.prompt.inputLine.visibleChars = con.linewidth;
    con.prompt.widthInChars = con.linewidth;
    con.chatPrompt.inputLine.visibleChars = con.linewidth;

    if (con_timestamps->integer) {
        char temp[CON_LINEWIDTH];
        con.prompt.widthInChars -= Com_FormatLocalTime(temp, con.linewidth, con_timestampsformat->string);
    }
}

/*
================
Con_CheckTop

Make sure at least one line is visible if console is backscrolled.
================
*/
static void Con_CheckTop(void)
{
    int top = con.current - CON_TOTALLINES + 1;

    if (top < 0) {
        top = 0;
    }
    if (con.display < top) {
        con.display = top;
    }
}

static void con_media_changed(cvar_t *self)
{
    if (con.initialized && cls.ref_initialized) {
        Con_RegisterMedia();
    }
}

static void con_width_changed(cvar_t *self)
{
    if (con.initialized && cls.ref_initialized) {
        Con_CheckResize();
    }
}

static void con_timestampscolor_changed(cvar_t *self)
{
    if (!SCR_ParseColor(self->string, &con.ts_color)) {
        Com_WPrintf("Invalid value '%s' for '%s'\n", self->string, self->name);
        Cvar_Reset(self);
        con.ts_color.u32 = MakeColor(170, 170, 170, 255);
    }
}

static const cmdreg_t c_console[] = {
    { "toggleconsole", Con_ToggleConsole_f },
    { "messagemode", Con_MessageMode_f },
    { "messagemode2", Con_MessageMode2_f },
    { "messageprompt", Con_MessagePrompt_f },
    { "remotemode", Con_RemoteMode_f, CL_RemoteMode_c },
    { "clear", Con_Clear_f },
    { "clearnotify", Con_ClearNotify_f },
    { "condump", Con_Dump_f, Con_Dump_c },

    { NULL }
};

/*
================
Con_Init
================
*/
void Con_Init(void)
{
    memset(&con, 0, sizeof(con));

//
// register our commands
//
    Cmd_Register(c_console);

    con_notifytime = Cvar_Get("con_notifytime", "3", 0);
    con_notifytime->changed = cl_timeout_changed;
    con_notifytime->changed(con_notifytime);
    con_notifylines = Cvar_Get("con_notifylines", "4", 0);
    // The notify lines - chat, obituaries, server prints over the game - in
    // the TrueType font (r_font); 0 is conchars. The size is the font's
    // pixel height in console units, a conchar being 8.
    con_notify_font = Cvar_Get("con_notify_font", "1", CVAR_ARCHIVE);
    con_notify_size = Cvar_Get("con_notify_size", "12", CVAR_ARCHIVE);
    // The console itself in the TrueType font: its lines keep their
    // conchar rows, so 10 (a conchar's cap height) and the columns of a
    // table are held by SCR_DrawTextGrid; 0 is conchars
    con_text_font = Cvar_Get("con_text_font", "1", CVAR_ARCHIVE);
    con_text_size = Cvar_Get("con_text_size", "10", CVAR_ARCHIVE);
    // Colour the fight's messages: hits you land blue, damage you take
    // red, the players' kill reports ("Enemy Down", the skull) cyan;
    // 0 leaves them be
    con_hitcolors = Cvar_Get("con_hitcolors", "1", CVAR_ARCHIVE);
    con_clock = Cvar_Get("con_clock", "0", 0);
    con_height = Cvar_Get("con_height", "0.5", 0);
    con_speed = Cvar_Get("scr_conspeed", "3", 0);
    con_alpha = Cvar_Get("con_alpha", "1", 0);
    con_scale = Cvar_Get("con_scale", "0", 0);
    con_scale->changed = con_width_changed;
    con_font = Cvar_Get("con_font", "conchars", 0);
    con_font->changed = con_media_changed;
    con_background = Cvar_Get("con_background", "conback", 0);
    con_background->changed = con_media_changed;
    con_scroll = Cvar_Get("con_scroll", "0", 0);
    con_history = Cvar_Get("con_history", STRINGIFY(HISTORY_SIZE), 0);
    con_timestamps = Cvar_Get("con_timestamps", "0", 0);
    con_timestamps->changed = con_width_changed;
    con_timestampsformat = Cvar_Get("con_timestampsformat", "%H:%M:%S ", 0);
    con_timestampsformat->changed = con_width_changed;
    con_timestampscolor = Cvar_Get("con_timestampscolor", "#aaa", 0);
    con_timestampscolor->changed = con_timestampscolor_changed;
    con_timestampscolor_changed(con_timestampscolor);
    con_auto_chat = Cvar_Get("con_auto_chat", "0", 0);

    IF_Init(&con.prompt.inputLine, 0, MAX_FIELD_TEXT - 1);
    IF_Init(&con.chatPrompt.inputLine, 0, MAX_FIELD_TEXT - 1);

    con.prompt.printf = Con_Printf;

    // use default width since no video is initialized yet
    r_config.width = 640;
    r_config.height = 480;
    con.linewidth = -1;
    con.scale = 1;
    con.color = COLOR_NONE;
    con.newline = '\r';

    Con_CheckResize();

    con.initialized = true;
}

// The history lives in the game's directory (action/.conhistory); one
// kept where it used to be, the base game's, is read once to carry over
void Con_PostInit(void)
{
    if (con_history->integer > 0) {
        if (!Prompt_LoadHistoryFrom(&con.prompt, COM_HISTORYFILE_NAME, FS_PATH_GAME))
            Prompt_LoadHistory(&con.prompt, COM_HISTORYFILE_NAME);
    }
}

static void Con_SaveHistory(void)
{
    if (con_history->integer > 0) {
        Prompt_SaveHistoryTo(&con.prompt, COM_HISTORYFILE_NAME, con_history->integer, FS_PATH_GAME);
    }
}

/*
================
Con_Shutdown
================
*/
void Con_Shutdown(void)
{
    Con_SaveHistory();
    Prompt_Clear(&con.prompt);
}

static void Con_CarriageRet(void)
{
    consoleLine_t *line = &con.text[con.current & CON_TOTALLINES_MASK];

    // add color from last line
    line->color = con.color;

    // add timestamp
    con.x = 0;
    if (con_timestamps->integer)
        con.x = Com_FormatLocalTime(line->text, con.linewidth, con_timestampsformat->string);
    line->ts_len = con.x;

    // init text (must be after timestamp format which may overflow)
    memset(line->text + con.x, 0, CON_LINEWIDTH - con.x);

    // update time for transparent overlay
    if (!con.skipNotify)
        con.times[con.current & CON_TIMES_MASK] = cls.realtime;
}

static void Con_Linefeed(void)
{
    if (con.display == con.current)
        con.display++;
    con.current++;

    Con_CarriageRet();

    if (con_scroll->integer & 2) {
        con.display = con.current;
    } else {
        Con_CheckTop();
    }

    // wrap to avoid integer overflow
    if (con.current >= CON_TOTALLINES * 2) {
        con.current -= CON_TOTALLINES;
        con.display -= CON_TOTALLINES;
    }
}

void Con_SetColor(color_index_t color)
{
    con.color = color;
}

/*
=================
CL_LoadState
=================
*/
void CL_LoadState(load_state_t state)
{
    con.loadstate = state;
    SCR_UpdateScreen();
    if (vid)
        vid->pump_events();
}

/*
================
Con_Print

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be displayed on screen
If no console is visible, the text will appear at the top of the game window
================
*/
void Con_Print(const char *txt)
{
    char *p;
    int l;

    if (!con.initialized)
        return;

    while (*txt) {
        if (con.newline) {
            if (con.newline == '\n') {
                Con_Linefeed();
            } else {
                Con_CarriageRet();
            }
            con.newline = 0;
        }

        // count word length
        for (p = (char *)txt; *p > 32; p++)
            ;
        l = p - txt;

        // word wrap
        if (l < con.linewidth && con.x + l > con.linewidth) {
            Con_Linefeed();
        }

        switch (*txt) {
        case '\r':
        case '\n':
            con.newline = *txt;
            break;
        default:    // display character and advance
            if (con.x == con.linewidth) {
                Con_Linefeed();
            }
            p = con.text[con.current & CON_TOTALLINES_MASK].text;
            p[con.x++] = *txt;
            break;
        }

        txt++;
    }

    // update time for transparent overlay
    if (!con.skipNotify)
        con.times[con.current & CON_TIMES_MASK] = cls.realtime;
}

/*
================
Con_Printf

Print text to graphical console only,
bypassing system console and logfiles
================
*/
void Con_Printf(const char *fmt, ...)
{
    va_list     argptr;
    char        msg[MAXPRINTMSG];

    va_start(argptr, fmt);
    Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    Con_Print(msg);
}

/*
================
Con_RegisterMedia
================
*/
void Con_RegisterMedia(void)
{
    con.charsetImage = R_RegisterFont(con_font->string);
    if (!con.charsetImage) {
        if (strcmp(con_font->string, con_font->default_string)) {
            Cvar_Reset(con_font);
            con.charsetImage = R_RegisterFont(con_font->default_string);
        }
        if (!con.charsetImage) {
            Com_Error(ERR_FATAL, "%s", Com_GetLastError());
        }
    }

    con.backImage = R_RegisterPic(con_background->string);
    if (!con.backImage) {
        if (strcmp(con_background->string, con_background->default_string)) {
            Cvar_Reset(con_background);
            con.backImage = R_RegisterPic(con_background->default_string);
        }
    }
}

/*
==============================================================================

DRAWING

==============================================================================
*/

/*
Con_HitColor: the colour con_hitcolors gives a line, or 0. The lines are
the mod's own (g_combat.c, p_hud.c): "You hit X in the chest" and the
helmet/vest notices go to the attacker, "Chest damage" and "Kevlar Vest
absorbed..." to the one hit. The players' team-chat kill reports are
"Enemy Down" or carry the charset's skull (byte 6) or dead face (8),
typed into their binds: "(m4tic): 1 <skull> iK. MaggeR!", drawn cyan to stand
out of the chat. A hit on a teammate keeps its colour.
*/
#define HIT_COLOR_GIVEN     MakeColor( 90, 160, 255, 255)
#define HIT_COLOR_TAKEN     MakeColor(255,  90,  80, 255)
#define HIT_COLOR_KILL      MakeColor( 80, 225, 235, 255)

uint32_t Con_HitColor(const char *s, size_t len)
{
    char buf[CON_LINEWIDTH + 1];

    if (!con_hitcolors->integer)
        return 0;

    // plain ASCII, no trailing newline or blanks, for the matching; `len`
    // is the room the line has, the text may end before it
    len = min(Q_strnlen(s, len), sizeof(buf) - 1);
    for (size_t i = 0; i < len; i++)
        buf[i] = s[i] & 127;
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\n'))
        len--;
    buf[len] = 0;

    // the charset's skull (6) or dead face (8, FragBait's binds)
    if (Q_strcasestr(buf, "enemy down") || strchr(buf, 6) || strchr(buf, 8))
        return HIT_COLOR_KILL;      // a player's kill report
    if (!strncmp(buf, "You hit ", 8))
        return strncmp(buf, "You hit your TEAMMATE", 21) ? HIT_COLOR_GIVEN : 0;
    if (strstr(buf, " - AIM FOR THE "))
        return HIT_COLOR_GIVEN;
    if (!strncmp(buf, "Kevlar Vest absorbed ", 21) ||
        !strncmp(buf, "Kevlar Helmet absorbed ", 23))
        return HIT_COLOR_TAKEN;
    if (len < 16 && len > 7 && !strcmp(buf + len - 7, " damage"))
        return HIT_COLOR_TAKEN;     // Head / Chest / Stomach / Leg damage
    return 0;
}

static bool Con_TextTTF(void)
{
    return con_text_font->integer && R_TextAvailable();
}

static float Con_TextSize(void)
{
    return Cvar_ClampValue(con_text_size, 6, 20);
}

/*
The console's rows. A conchar's height, which the text fits up to size
10 - a capital is as tall as a conchar there - and past that growing
with the text, in the same proportion: at a fixed 8 a bigger
con_text_size ran every line into the next.
*/
static int Con_RowHeight(void)
{
    if (!Con_TextTTF())
        return CONCHAR_HEIGHT;
    return max(CONCHAR_HEIGHT, Q_rint(CONCHAR_HEIGHT * Con_TextSize() / 10));
}

#define CON_ROW     Con_RowHeight()

static int Con_DrawLine(int v, int row, float alpha, bool notify)
{
    const consoleLine_t *line = &con.text[row & CON_TOTALLINES_MASK];
    const char *s = line->text;
    int flags = 0;
    int x = CONCHAR_WIDTH;
    int w = con.linewidth;

    bool ttf = !notify && Con_TextTTF();
    float size = Con_TextSize();

    if (notify) {
        s += line->ts_len;
    } else if (line->ts_len) {
        if (ttf) {
            R_ClearColor();
            R_SetAlpha(alpha);
            SCR_DrawTextCell(x, v, CON_ROW, 0, TEXT_SHADOW, size,
                             con.ts_color.u32 | MakeColor(0, 0, 0, 255), s, line->ts_len);
            x += line->ts_len * CONCHAR_WIDTH;  // the stamp keeps its columns
        } else {
            R_SetColor(con.ts_color.u32);
            R_SetAlpha(alpha);
            x = R_DrawString(x, v, 0, line->ts_len, s, con.charsetImage);
        }
        s += line->ts_len;
        w -= line->ts_len;
    }
    if (w < 1)
        return x;

    uint32_t hit = Con_HitColor(s, w);

    if (ttf) {
        uint32_t color = U32_WHITE;
        int flags = 0;

        if (hit)
            color = hit;
        else if (line->color == COLOR_ALT)
            flags = UI_ALTCOLOR;
        else if (line->color != COLOR_NONE)
            color = colorTable[line->color & 7];
        R_ClearColor();
        R_SetAlpha(alpha);
        // chat is prose, never a table: its runs of spaces are the
        // players' own, so it is drawn as it comes; so is a coloured
        // line, the charset's coloured letters keeping their colour
        if (line->color == COLOR_ALT || hit)
            return SCR_DrawTextCell(x, v, CON_ROW, flags,
                                    TEXT_SHADOW | (hit ? TEXT_OWNTINT : 0),
                                    size, color, s, w);
        return SCR_DrawTextGrid(x, v, CON_ROW, flags, size, color, NULL, s, w);
    }

    if (hit) {
        R_SetColor(hit);
        R_SetAlpha(alpha);
        return R_DrawString(x, v, 0, w, s, con.charsetImage);
    }

    switch (line->color) {
    case COLOR_ALT:
        flags = UI_ALTCOLOR;
        // fall through
    case COLOR_NONE:
        R_ClearColor();
        break;
    default:
        R_SetColor(colorTable[line->color & 7]);
        break;
    }
    R_SetAlpha(alpha);

    return R_DrawString(x, v, flags, w, s, con.charsetImage);
}

// A notify line in the TrueType font: Con_DrawLine's colours, the line
// `lh` tall
static void Con_DrawNotifyText(int v, int row, float alpha, float size, int lh)
{
    const consoleLine_t *line = &con.text[row & CON_TOTALLINES_MASK];
    uint32_t color = U32_WHITE;
    int flags = 0;
    int w = con.linewidth - line->ts_len;

    if (w < 1)
        return;

    switch (line->color) {
    case COLOR_ALT:
        flags = UI_ALTCOLOR;
        break;
    case COLOR_NONE:
        break;
    default:
        color = colorTable[line->color & 7];
        break;
    }

    uint32_t hit = Con_HitColor(line->text + line->ts_len, w);
    if (hit) {
        color = hit;
        flags = 0;
    }

    // a coloured line takes its colour, the charset's coloured letters
    // (the brackets) keeping theirs
    R_ClearColor();
    R_SetAlpha(alpha);
    SCR_DrawTextCell(CONCHAR_WIDTH, v, lh, flags,
                     TEXT_SHADOW | (hit ? TEXT_OWNTINT : 0), size, color,
                     line->text + line->ts_len, w);
}

// An input line in the TrueType font: the field scrolled the way IF_Draw
// scrolls it, and the cursor after the measured text before it - the
// overstrike block is conchars' own picture. Returns where the text ends.
static int Con_DrawInputText(const inputField_t *f, int x, int v, float size, int lh)
{
    size_t cursor = f->cursorPos, offset = 0;

    if (!f->maxChars || !f->visibleChars)
        return x;
    if (cursor >= f->visibleChars) {
        cursor = f->visibleChars - 1;
        offset = f->cursorPos - cursor;
    }

    int end = SCR_DrawTextCell(x, v, lh, 0, TEXT_SHADOW, size, U32_WHITE,
                               f->text + offset, f->visibleChars);
    if (com_localTime & BIT(8)) {
        int cx = x + R_MeasureText(TEXT_SHADOW, size, f->text + offset, cursor);
        SCR_DrawTextCell(cx, v, lh, 0, TEXT_SHADOW, size, U32_WHITE,
                         Key_GetOverstrikeMode() ? "\x0b" : "_", 1);
    }
    return end;
}

// The chat input under the notify lines, prompt first
static void Con_DrawChatInputText(int v, const char *prompt, float size, int lh)
{
    R_ClearColor();
    R_SetAlpha(1);
    int x = SCR_DrawTextCell(CONCHAR_WIDTH, v, lh, 0, TEXT_SHADOW, size,
                             U32_WHITE, prompt, MAX_STRING_CHARS);
    Con_DrawInputText(&con.chatPrompt.inputLine, x + Q_rint(size * 0.4f),
                      v, size, lh);
}

#define CON_PRESTEP     (CON_ROW * 3 + CON_ROW / 4)

#define CON_PROMPT_KEYS     "Enter: OK    Esc: cancel"
#define CON_PROMPT_CHARS    44  // the line typed, at least

// messageprompt's box: the question, the line being typed, the hint and
// the keys - as wide as the longest of them. In the font where the
// notify lines are, at their size, as the chat input is.
static void Con_DrawPromptBox(void)
{
    bool ttf = con_notify_font->integer && R_TextAvailable();
    float size = Cvar_ClampValue(con_notify_size, 6, 40);
    int chars = max(strlen(con.promptLabel), strlen(con.promptHint));
    int lh = CONCHAR_HEIGHT, pad = CONCHAR_WIDTH;
    int w, h, x, y = con.vidHeight / 3;

    chars = min(max(chars, CON_PROMPT_CHARS), con.linewidth - 2);
    w = chars * CONCHAR_WIDTH;
    if (ttf) {
        lh = R_TextLineHeight(0, size);
        pad = lh * 3 / 4;
        w = max(R_MeasureText(TEXT_BOLD, size, con.promptLabel, MAX_STRING_CHARS),
                R_MeasureText(0, size, con.promptHint, MAX_STRING_CHARS));
        w = max(w, Q_rint(size * 0.55f * CON_PROMPT_CHARS));
        w = min(w, con.vidWidth - 4 * pad);
    }
    h = lh * 6 + pad;
    w += 2 * pad;
    x = (con.vidWidth - w) / 2;

    R_ClearColor();
    R_DrawFill32(x, y, w, h, MakeColor(0, 0, 0, 210));
    R_DrawFill32(x, y, w, 1, MakeColor(255, 220, 0, 255));
    x += pad;
    y += pad;
    con.chatPrompt.inputLine.visibleChars = chars;

    if (ttf) {
        SCR_DrawTextCell(x, y, lh, 0, TEXT_SHADOW | TEXT_BOLD, size, U32_WHITE,
                         con.promptLabel, MAX_STRING_CHARS);
        Con_DrawInputText(&con.chatPrompt.inputLine, x, y + lh * 3 / 2, size, lh);
        R_SetAlpha(0.55f);
        SCR_DrawTextCell(x, y + lh * 7 / 2, lh, 0, TEXT_SHADOW, size, U32_WHITE,
                         con.promptHint, MAX_STRING_CHARS);
        SCR_DrawTextCell(x, y + lh * 9 / 2, lh, 0, TEXT_SHADOW, size, U32_WHITE,
                         CON_PROMPT_KEYS, MAX_STRING_CHARS);
        R_ClearColor();
        return;
    }

    R_DrawString(x, y, 0, chars, con.promptLabel, con.charsetImage);
    IF_Draw(&con.chatPrompt.inputLine, x, y + lh * 3 / 2,
            UI_DRAWCURSOR, con.charsetImage);
    R_SetAlpha(0.5f);
    R_DrawString(x, y + lh * 7 / 2, 0, chars, con.promptHint, con.charsetImage);
    R_DrawString(x, y + lh * 9 / 2, 0, chars, CON_PROMPT_KEYS, con.charsetImage);
    R_ClearColor();
}

/*
================
Con_DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
static void Con_DrawNotify(void)
{
    int     v;
    const char  *text;
    int     i, j;
    unsigned    time;
    int     skip;
    float   alpha;

    // only draw notify in game
    if (cls.state != ca_active) {
        return;
    }
    if (cls.key_dest & (KEY_MENU | KEY_CONSOLE)) {
        return;
    }
    if (con.currentHeight) {
        return;
    }

    j = con_notifylines->integer;
    if (j > CON_TIMES) {
        j = CON_TIMES;
    }

    bool ttf = con_notify_font->integer && R_TextAvailable();
    float size = Cvar_ClampValue(con_notify_size, 6, 40);
    int lh = ttf ? R_TextLineHeight(0, size) : CONCHAR_HEIGHT;

    v = 0;
    for (i = con.current - j + 1; i <= con.current; i++) {
        if (i < 0)
            continue;
        time = con.times[i & CON_TIMES_MASK];
        if (time == 0)
            continue;
        // alpha fade the last string left on screen
        alpha = SCR_FadeAlpha(time, con_notifytime->integer, 300);
        if (!alpha)
            continue;
        if (v || i != con.current) {
            alpha = 1;  // don't fade
        }

        if (ttf)
            Con_DrawNotifyText(v, i, alpha, size, lh);
        else
            Con_DrawLine(v, i, alpha, true);

        v += lh;
    }

    R_ClearColor();

    if (cls.key_dest & KEY_MESSAGE) {
        if (con.chat == CHAT_PROMPT) {
            Con_DrawPromptBox();
            return;
        }
        if (con.chat == CHAT_TEAM) {
            text = "say_team:";
            skip = 11;
        } else {
            text = "say:";
            skip = 5;
        }

        con.chatPrompt.inputLine.visibleChars = con.linewidth - skip + 1;
        if (ttf) {
            Con_DrawChatInputText(v, text, size, lh);
            return;
        }
        R_DrawString(CONCHAR_WIDTH, v, 0, MAX_STRING_CHARS, text,
                     con.charsetImage);
        IF_Draw(&con.chatPrompt.inputLine, skip * CONCHAR_WIDTH, v,
                UI_DRAWCURSOR, con.charsetImage);
    }
}

/*
================
Con_DrawSolidConsole

Draws the console with the solid background
================
*/
/*
The history search's candidates over the console's last lines, as fzf
lists them: the best next to the prompt and the rest going up, the
current one marked, the query's letters picked out in each, and how many
match under them all.
*/
#define CON_SEARCH_ROWS     10

static void Con_DrawSearchLine(int x, int y, const char *s, bool current)
{
    const char *t = (*s == '/' || *s == '\\') ? s + 1 : s;
    int pos[MAX_FIELD_TEXT], n, k = 0;

    n = Fuzzy_Positions(con.search.query, t, pos, q_countof(pos));

    if (Con_TextTTF()) {
        // in the font: runs of matched and unmatched letters, each where
        // the text before it ends
        float size = Con_TextSize();
        uint32_t base = current ? U32_WHITE : MakeColor(192, 192, 192, 255);
        int off = t - s, i = 0;

        while (s[i]) {
            bool hit = k < n && pos[k] == i - off;
            int j = i;
            while (s[j] && (k < n && pos[k] == j - off) == hit) {
                if (hit)
                    k++;
                j++;
            }
            SCR_DrawTextCell(x + R_MeasureText(TEXT_SHADOW, size, s, i), y, CON_ROW,
                             0, TEXT_SHADOW, size, hit ? U32_GREEN : base, s + i, j - i);
            i = j;
        }
        return;
    }

    for (int i = 0; s[i] && x < con.vidWidth - CONCHAR_WIDTH; i++, x += CONCHAR_WIDTH) {
        bool hit = k < n && pos[k] == i - (int)(t - s);
        if (hit)
            k++;
        R_SetColor(hit ? U32_GREEN : current ? U32_WHITE : MakeColor(192, 192, 192, 255));
        R_DrawChar(x, y, 0, s[i], con.charsetImage);
    }
    R_ClearColor();
}

static void Con_DrawSearch(int y, int vislines)
{
    int rows = min(con.search.count, CON_SEARCH_ROWS);
    int room = (vislines - CON_PRESTEP) / CON_ROW - 3;
    int first, top;

    rows = min(rows, room);
    if (rows < 0)
        rows = 0;

    // the window keeps the current one in it
    first = max(0, con.search.pos - rows + 1);

    // solid over the console's own transparency: its text must not show
    // through between the candidates
    R_SetAlpha(1);
    top = y - (rows + 1) * CON_ROW;
    R_DrawFill32(0, top - 2, con.vidWidth, (rows + 1) * CON_ROW + 2,
                 MakeColor(0, 0, 0, 255));

    for (int k = 0; k < rows; k++) {
        int i = first + k;
        int ry = y - (k + 2) * CON_ROW;

        if (i == con.search.pos) {
            R_DrawFill32(0, ry, con.vidWidth, CON_ROW, MakeColor(64, 64, 64, 224));
            if (Con_TextTTF()) {
                SCR_DrawTextCell(CONCHAR_WIDTH, ry, CON_ROW, 0, TEXT_SHADOW,
                                 Con_TextSize(), U32_YELLOW, ">", 1);
            } else {
                R_SetColor(U32_YELLOW);
                R_DrawChar(CONCHAR_WIDTH, ry, 0, '>', con.charsetImage);
                R_ClearColor();
            }
        }
        Con_DrawSearchLine(3 * CONCHAR_WIDTH, ry, con.search.matches[i], i == con.search.pos);
    }

    // how many match, as fzf counts them
    char count[32];
    Q_snprintf(count, sizeof(count), "  %d/%d", con.search.count ? con.search.pos + 1 : 0,
               con.search.count);
    if (Con_TextTTF()) {
        SCR_DrawTextCell(CONCHAR_WIDTH, y - CON_ROW, CON_ROW, 0, TEXT_SHADOW,
                         Con_TextSize(), MakeColor(160, 160, 96, 255), count, MAX_STRING_CHARS);
    } else {
        R_SetColor(MakeColor(160, 160, 96, 255));
        R_DrawString(CONCHAR_WIDTH, y - CON_ROW, 0, MAX_STRING_CHARS, count, con.charsetImage);
        R_ClearColor();
    }
}

static void Con_DrawSolidConsole(void)
{
    int             i, x, y;
    int             rows;
    const char      *text;
    int             row;
    char            buffer[CON_LINEWIDTH];
    int             vislines;
    float           alpha;
    int             widths[2];
    bool            ttf = Con_TextTTF();
    float           size = Con_TextSize();

    vislines = con.vidHeight * con.currentHeight;
    if (vislines <= 0)
        return;

    if (vislines > con.vidHeight)
        vislines = con.vidHeight;

// setup transparency
    if (cls.state >= ca_active && !(cls.key_dest & KEY_MENU) && con_alpha->value) {
        alpha = 0.5f + 0.5f * (con.currentHeight / con_height->value);
        R_SetAlpha(alpha * Cvar_ClampValue(con_alpha, 0, 1));
    }

// draw the background
    if (cls.state < ca_active || (cls.key_dest & KEY_MENU) || con_alpha->value) {
        R_DrawKeepAspectPic(0, vislines - con.vidHeight,
                            con.vidWidth, con.vidHeight, con.backImage);
    }

// draw the text
    y = vislines - CON_PRESTEP;
    rows = y / CON_ROW + 1;  // rows of text to draw

// draw arrows to show the buffer is backscrolled
    if (con.display != con.current) {
        R_SetColor(U32_RED);
        for (i = 1; i < con.linewidth / 2; i += 4) {
            if (ttf)
                SCR_DrawTextCell(i * CONCHAR_WIDTH, y, CON_ROW, 0,
                                 TEXT_SHADOW, Con_TextSize(), U32_RED, "^", 1);
            else
                R_DrawChar(i * CONCHAR_WIDTH, y, 0, '^', con.charsetImage);
        }

        y -= CON_ROW;
        rows--;
    }

// draw from the bottom up
    R_ClearColor();
    row = con.display;
    widths[0] = widths[1] = 0;
    for (i = 0; i < rows; i++) {
        if (row < 0)
            break;
        if (con.current - row > CON_TOTALLINES - 1)
            break;      // past scrollback wrap point

        x = Con_DrawLine(y, row, 1, false);
        if (i < 2) {
            widths[i] = x;
        }

        y -= CON_ROW;
        row--;
    }

    R_ClearColor();

    // draw the download bar
    if (cls.download.current) {
        char pos[16], suf[32];
        int n, j;

        if ((text = strrchr(cls.download.current->path, '/')) != NULL)
            text++;
        else
            text = cls.download.current->path;

        Com_FormatSizeLong(pos, sizeof(pos), cls.download.position);
        n = 4 + Q_scnprintf(suf, sizeof(suf), " %d%% (%s)", cls.download.percent, pos);

        // figure out width
        x = con.linewidth;
        y = x - strlen(text) - n;
        i = x / 3;
        if (strlen(text) > i) {
            y = x - i - n - 3;
            memcpy(buffer, text, i);
            buffer[i] = 0;
            strcat(buffer, "...");
        } else {
            strcpy(buffer, text);
        }
        strcat(buffer, ": ");
        i = strlen(buffer);
        buffer[i++] = '\x80';
        // where's the dot go?
        n = y * cls.download.percent / 100;
        for (j = 0; j < y; j++) {
            if (j == n) {
                buffer[i++] = '\x83';
            } else {
                buffer[i++] = '\x81';
            }
        }
        buffer[i++] = '\x82';
        buffer[i] = 0;

        Q_strlcat(buffer, suf, sizeof(buffer));

        // draw it
        y = vislines - CON_PRESTEP + CON_ROW * 2;
        R_DrawString(CONCHAR_WIDTH, y, 0, con.linewidth, buffer, con.charsetImage);
    } else if (cls.state == ca_loading) {
        // draw loading state
        switch (con.loadstate) {
        case LOAD_MAP:
            text = cl.configstrings[cl.csr.models + 1];
            break;
        case LOAD_MODELS:
            text = "models";
            break;
        case LOAD_IMAGES:
            text = "images";
            break;
        case LOAD_CLIENTS:
            text = "clients";
            break;
        case LOAD_SOUNDS:
            text = "sounds";
            break;
        default:
            text = NULL;
            break;
        }

        if (text) {
            Q_snprintf(buffer, sizeof(buffer), "Loading %s...", text);

            // draw it
            y = vislines - CON_PRESTEP + CON_ROW * 2;
            if (ttf)
                SCR_DrawTextCell(CONCHAR_WIDTH, y, CON_ROW, 0, TEXT_SHADOW,
                                 size, U32_WHITE, buffer, con.linewidth);
            else
                R_DrawString(CONCHAR_WIDTH, y, 0, con.linewidth, buffer, con.charsetImage);
        }
    }

// draw the input prompt, user text, and cursor if desired
    x = 0;
    if (cls.key_dest & KEY_CONSOLE) {
        y = vislines - CON_PRESTEP + CON_ROW;

        if (con.search.active)
            Con_DrawSearch(y, vislines);

        // draw command prompt
        i = con.mode == CON_REMOTE ? '#' : 17;

        // the input line, or the history search in its place
        const char *m = NULL;
        if (con.search.active) {
            m = Con_SearchMatch();
            Q_snprintf(buffer, sizeof(buffer), "(%sfuzzy-search)`%s': ",
                       m ? "" : "failed ", con.search.query);
        }

        if (ttf) {
            char p[2] = { i, 0 };
            R_ClearColor();
            SCR_DrawTextCell(CONCHAR_WIDTH, y, CON_ROW, 0, TEXT_SHADOW,
                             size, U32_YELLOW, p, 1);
            if (con.search.active) {
                x = SCR_DrawTextCell(2 * CONCHAR_WIDTH, y, CON_ROW, 0, TEXT_SHADOW,
                                     size, U32_GREEN, buffer, MAX_STRING_CHARS);
                if (m)
                    x = SCR_DrawTextCell(x, y, CON_ROW, 0, TEXT_SHADOW,
                                         size, U32_WHITE, m, MAX_STRING_CHARS);
            } else {
                x = Con_DrawInputText(&con.prompt.inputLine, 2 * CONCHAR_WIDTH, y,
                                      size, CON_ROW);
            }
        } else {
            R_SetColor(U32_YELLOW);
            R_DrawChar(CONCHAR_WIDTH, y, 0, i, con.charsetImage);
            R_ClearColor();

            if (con.search.active) {
                x = R_DrawString(2 * CONCHAR_WIDTH, y, UI_ALTCOLOR, MAX_STRING_CHARS,
                                 buffer, con.charsetImage);
                if (m)
                    x = R_DrawString(x, y, 0, con.linewidth, m, con.charsetImage);
            } else {
                x = IF_Draw(&con.prompt.inputLine, 2 * CONCHAR_WIDTH, y,
                            UI_DRAWCURSOR, con.charsetImage);
            }
        }
    }

#define APP_VERSION APPLICATION " " VERSION
#define VER_WIDTH ((int)(sizeof(APP_VERSION) + 1) * CONCHAR_WIDTH)

    y = vislines - CON_PRESTEP + CON_ROW;
    row = 0;
    // shift version upwards to prevent overdraw
    if (x > con.vidWidth - VER_WIDTH) {
        y -= CON_ROW;
        row++;
    }

    R_SetColor(U32_CYAN);

// draw clock
    if (con_clock->integer) {
        x = Com_Time_m(buffer, sizeof(buffer)) * CONCHAR_WIDTH;
        if (widths[row] + x + CONCHAR_WIDTH <= con.vidWidth) {
            if (ttf)
                SCR_DrawTextCell(con.vidWidth - CONCHAR_WIDTH, y - CON_ROW,
                                 CON_ROW, UI_RIGHT, TEXT_SHADOW, size,
                                 U32_CYAN, buffer, MAX_STRING_CHARS);
            else
                R_DrawString(con.vidWidth - CONCHAR_WIDTH - x, y - CON_ROW,
                             UI_RIGHT, MAX_STRING_CHARS, buffer, con.charsetImage);
        }
    }

// draw version
    if (!row || widths[0] + VER_WIDTH <= con.vidWidth) {
        if (ttf)
            SCR_DrawTextCell(con.vidWidth - CONCHAR_WIDTH, y, CON_ROW,
                             UI_RIGHT, TEXT_SHADOW, size, U32_CYAN,
                             APP_VERSION, MAX_STRING_CHARS);
        else
            SCR_DrawStringEx(con.vidWidth - CONCHAR_WIDTH, y, UI_RIGHT,
                             MAX_STRING_CHARS, APP_VERSION, con.charsetImage);
    }

    // restore rendering parameters
    R_ClearColor();
}

//=============================================================================

/*
==================
Con_RunConsole

Scroll it up or down
==================
*/
void Con_RunConsole(void)
{
    if (cls.disable_screen) {
        con.destHeight = con.currentHeight = 0;
        return;
    }

    if (!(cls.key_dest & KEY_MENU)) {
        if (cls.state == ca_disconnected) {
            // draw fullscreen console
            con.destHeight = con.currentHeight = 1;
            return;
        }
        if (cls.state > ca_disconnected && cls.state < ca_active) {
            // draw half-screen console
            con.destHeight = con.currentHeight = 0.5f;
            return;
        }
    }

// decide on the height of the console
    if (cls.key_dest & KEY_CONSOLE) {
        con.destHeight = Cvar_ClampValue(con_height, 0.1f, 1);
    } else {
        con.destHeight = 0;             // none visible
    }

    if (con_speed->value <= 0) {
        con.currentHeight = con.destHeight;
        return;
    }

    CL_AdvanceValue(&con.currentHeight, con.destHeight, con_speed->value);
}

/*
==================
SCR_DrawConsole
==================
*/
void Con_DrawConsole(void)
{
    R_SetScale(con.scale);
    Con_DrawSolidConsole();
    Con_DrawNotify();
    R_SetScale(1.0f);
}


/*
==============================================================================

            LINE TYPING INTO THE CONSOLE AND COMMAND COMPLETION

==============================================================================
*/

static void Con_Say(const char *msg)
{
    if (con.chat == CHAT_PROMPT) {
        CL_ClientCommand(va("%s \"%s\"", con.promptCmd, msg));
        return;
    }
    CL_ClientCommand(va("say%s \"%s\"", con.chat == CHAT_TEAM ? "_team" : "", msg));
}

// don't close console after connecting
static void Con_InteractiveMode(void)
{
    if (con.mode == CON_POPUP) {
        con.mode = CON_DEFAULT;
    }
}

static void Con_Action(void)
{
    const char *cmd = Prompt_Action(&con.prompt);

    Con_InteractiveMode();

    if (!cmd) {
        Con_Printf("]\n");
        return;
    }

    // every line kept as it is entered: a crash loses none of them
    Con_SaveHistory();

    // backslash text are commands, else chat
    int backslash = cmd[0] == '\\' || cmd[0] == '/';

    if (con.mode == CON_REMOTE) {
        CL_SendRcon(&con.remoteAddress, con.remotePassword, cmd + backslash);
    } else {
        if (!backslash && cls.state == ca_active) {
            switch (con_auto_chat->integer) {
            case CHAT_DEFAULT:
                Cbuf_AddText(&cmd_buffer, "cmd say ");
                break;
            case CHAT_TEAM:
                Cbuf_AddText(&cmd_buffer, "cmd say_team ");
                break;
            }
        }
        Cbuf_AddText(&cmd_buffer, cmd + backslash);
        Cbuf_AddText(&cmd_buffer, "\n");
    }

    Con_Printf("]%s\n", cmd);

    if (cls.state == ca_disconnected) {
        // force an update, because the command may take some time
        SCR_UpdateScreen();
    }
}

static void Con_Paste(char *(*func)(void))
{
    char *cbd, *s;

    Con_InteractiveMode();

    if (!func || !(cbd = func())) {
        return;
    }

    s = cbd;
    while (*s) {
        int c = *s++;
        switch (c) {
        case '\n':
            if (*s) {
                Con_Action();
            }
            break;
        case '\r':
        case '\t':
            IF_CharEvent(&con.prompt.inputLine, ' ');
            break;
        default:
            if (!Q_isprint(c)) {
                c = '?';
            }
            IF_CharEvent(&con.prompt.inputLine, c);
            break;
        }
    }

    Z_Free(cbd);
}

// console lines are not necessarily NUL-terminated
static void Con_ClearLine(char *buf, int row)
{
    const consoleLine_t *line = &con.text[row & CON_TOTALLINES_MASK];
    const char *s = line->text + line->ts_len;
    int w = con.linewidth - line->ts_len;

    while (w-- > 0 && *s)
        *buf++ = *s++ & 127;
    *buf = 0;
}

static void Con_SearchUp(void)
{
    char buf[CON_LINEWIDTH + 1];
    const char *s = con.prompt.inputLine.text;
    int top = con.current - CON_TOTALLINES + 1;

    if (top < 0)
        top = 0;

    if (!*s)
        return;

    for (int row = con.display - 1; row >= top; row--) {
        Con_ClearLine(buf, row);
        if (Q_stristr(buf, s)) {
            con.display = row;
            break;
        }
    }
}

static void Con_SearchDown(void)
{
    char buf[CON_LINEWIDTH + 1];
    const char *s = con.prompt.inputLine.text;

    if (!*s)
        return;

    for (int row = con.display + 1; row <= con.current; row++) {
        Con_ClearLine(buf, row);
        if (Q_stristr(buf, s)) {
            con.display = row;
            break;
        }
    }
}

/*
====================
History search

Ctrl-R opens it with what the line holds as the query; typing narrows it,
Backspace widens it. The matches are the history's lines the query fuzzy
matches, best first and the newest among equals, listed over the console
best nearest the prompt: Up, Ctrl-R or Ctrl-P go up the list, Down,
Ctrl-S or Ctrl-N back, the page keys and the wheel a page at a time.
Enter runs the match, Escape or Ctrl-G puts the line back as it was, and
any other key (Left, Right, Home, End, Tab ...) takes the match into the
line to edit.
====================
*/

static void Con_SearchUpdate(void)
{
    con.search.count = Prompt_FuzzyHistory(&con.prompt, con.search.query,
                                           con.search.matches, HISTORY_SIZE);
    con.search.pos = 0;
}

static const char *Con_SearchMatch(void)
{
    return con.search.pos < con.search.count ? con.search.matches[con.search.pos] : NULL;
}

static void Con_SearchStart(void)
{
    const char *s = con.prompt.inputLine.text;

    con.search.active = true;
    con.search.saved = Z_CopyString(s);
    if (*s == '/' || *s == '\\')
        s++;
    Q_strlcpy(con.search.query, s, sizeof(con.search.query));
    Con_SearchUpdate();
}

static void Con_SearchEnd(bool accept)
{
    const char *m = Con_SearchMatch();

    if (accept && m)
        IF_Replace(&con.prompt.inputLine, m);
    else if (!accept)
        IF_Replace(&con.prompt.inputLine, con.search.saved);
    Z_Freep(&con.search.saved);
    con.search.active = false;
    Prompt_ClearState(&con.prompt);
}

// Escape while searching: back to the line, the console stays
bool Con_SearchCancel(void)
{
    if (!con.search.active)
        return false;
    Con_SearchEnd(false);
    return true;
}

// a key while searching; false when it ends the search and goes on as usual
static bool Con_SearchKey(int key)
{
    bool ctrl = Key_IsDown(K_CTRL);

    // through the list as fzf goes: it is drawn best next to the prompt
    // and going up, so up (or Ctrl-R) is the next match, down the last
    if ((key == 'r' && ctrl) || (key == 'p' && ctrl) ||
        key == K_UPARROW || key == K_KP_UPARROW) {
        if (con.search.pos + 1 < con.search.count)
            con.search.pos++;
        return true;
    }
    if ((key == 's' && ctrl) || (key == 'n' && ctrl) ||
        key == K_DOWNARROW || key == K_KP_DOWNARROW) {
        if (con.search.pos > 0)
            con.search.pos--;
        return true;
    }
    if (key == K_PGUP || key == K_KP_PGUP || key == K_MWHEELUP) {
        con.search.pos = min(con.search.pos + CON_SEARCH_ROWS, max(con.search.count - 1, 0));
        return true;
    }
    if (key == K_PGDN || key == K_KP_PGDN || key == K_MWHEELDOWN) {
        con.search.pos = max(con.search.pos - CON_SEARCH_ROWS, 0);
        return true;
    }
    if (key == 'g' && ctrl) {
        Con_SearchEnd(false);
        return true;
    }
    if (key == 'c' && ctrl && !Key_IsDown(K_SHIFT)) {
        Con_SearchEnd(false);
        return false;   // and the line goes, below
    }
    if (key == K_BACKSPACE) {
        size_t len = strlen(con.search.query);
        if (len) {
            con.search.query[len - 1] = 0;
            Con_SearchUpdate();
        }
        return true;
    }
    // the modifiers themselves, and letters (they come as characters)
    if (key == K_SHIFT || key == K_CTRL || key == K_ALT ||
        (key >= 32 && key < 127 && !ctrl && !Key_IsDown(K_ALT)))
        return true;

    Con_SearchEnd(true);
    return false;
}

/*
====================
Key_Console

Interactive line editing and console scrollback
====================
*/
void Key_Console(int key)
{
    if (con.search.active) {
        if (key == K_ENTER || key == K_KP_ENTER) {
            Con_SearchEnd(true);
            Con_Action();
            goto scroll;
        }
        if (Con_SearchKey(key))
            return;
    }

    if (key == 'l' && Key_IsDown(K_CTRL)) {
        Con_Clear_f();
        return;
    }

    if (key == 'd' && Key_IsDown(K_CTRL)) {
        con.mode = CON_DEFAULT;
        return;
    }

    // Ctrl-C drops the line, as a shell does: shown with ^C, not kept in
    // the history, a fresh prompt; Ctrl-Shift-C copies it as before
    if (key == 'c' && Key_IsDown(K_CTRL) && !Key_IsDown(K_SHIFT)) {
        Con_Printf("]%s^C\n", con.prompt.inputLine.text);
        IF_Clear(&con.prompt.inputLine);
        Prompt_ClearState(&con.prompt);
        con.prompt.historyLineNum = con.prompt.inputLineNum;
        Con_InteractiveMode();
        goto scroll;
    }

    if (key == K_ENTER || key == K_KP_ENTER) {
        Con_Action();
        goto scroll;
    }

    if (key == 'v' && Key_IsDown(K_CTRL)) {
        if (vid)
            Con_Paste(vid->get_clipboard_data);
        goto scroll;
    }

    if ((key == K_INS && Key_IsDown(K_SHIFT)) || key == K_MOUSE3) {
        if (vid)
            Con_Paste(vid->get_selection_data);
        goto scroll;
    }

    if (key == K_TAB) {
        if (con_timestamps->integer)
            Con_CheckResize();
        Prompt_CompleteCommand(&con.prompt, true);
        goto scroll;
    }

    if (key == 'r' && Key_IsDown(K_CTRL)) {
        Con_SearchStart();
        goto scroll;
    }

    if (key == 's' && Key_IsDown(K_CTRL)) {
        Prompt_CompleteHistory(&con.prompt, true);
        goto scroll;
    }

    if (key == K_UPARROW && Key_IsDown(K_CTRL)) {
        Con_SearchUp();
        return;
    }

    if (key == K_DOWNARROW && Key_IsDown(K_CTRL)) {
        Con_SearchDown();
        return;
    }

    if (key == K_UPARROW || (key == 'p' && Key_IsDown(K_CTRL))) {
        Prompt_HistoryUp(&con.prompt);
        goto scroll;
    }

    if (key == K_DOWNARROW || (key == 'n' && Key_IsDown(K_CTRL))) {
        Prompt_HistoryDown(&con.prompt);
        goto scroll;
    }

    if (key == K_PGUP || key == K_MWHEELUP) {
        if (Key_IsDown(K_CTRL)) {
            con.display -= 6;
        } else {
            con.display -= 2;
        }
        Con_CheckTop();
        return;
    }

    if (key == K_PGDN || key == K_MWHEELDOWN) {
        if (Key_IsDown(K_CTRL)) {
            con.display += 6;
        } else {
            con.display += 2;
        }
        if (con.display > con.current) {
            con.display = con.current;
        }
        return;
    }

    if (key == K_HOME && Key_IsDown(K_CTRL)) {
        con.display = 0;
        Con_CheckTop();
        return;
    }

    if (key == K_END && Key_IsDown(K_CTRL)) {
        con.display = con.current;
        return;
    }

    if (IF_KeyEvent(&con.prompt.inputLine, key)) {
        Prompt_ClearState(&con.prompt);
        Con_InteractiveMode();
    }

scroll:
    if (con_scroll->integer & 1) {
        con.display = con.current;
    }
}

void Char_Console(int key)
{
    if (con.search.active) {
        size_t len = strlen(con.search.query);
        if (Q_isprint(key) && len < sizeof(con.search.query) - 1) {
            con.search.query[len] = key;
            con.search.query[len + 1] = 0;
            Con_SearchUpdate();
        }
        return;
    }

    if (IF_CharEvent(&con.prompt.inputLine, key)) {
        Con_InteractiveMode();
    }
}

/*
====================
Key_Message
====================
*/
void Key_Message(int key)
{
    if (key == 'l' && Key_IsDown(K_CTRL)) {
        IF_Clear(&con.chatPrompt.inputLine);
        return;
    }

    if (key == K_ENTER || key == K_KP_ENTER) {
        const char *cmd = Prompt_Action(&con.chatPrompt);

        if (cmd) {
            Con_Say(cmd);
        }
        Key_SetDest(cls.key_dest & ~KEY_MESSAGE);
        return;
    }

    if (key == K_ESCAPE) {
        if (con.chat == CHAT_PROMPT)
            CL_ClientCommand(con.promptCmd);
        Key_SetDest(cls.key_dest & ~KEY_MESSAGE);
        IF_Clear(&con.chatPrompt.inputLine);
        return;
    }

    if (key == 'r' && Key_IsDown(K_CTRL)) {
        Prompt_CompleteHistory(&con.chatPrompt, false);
        return;
    }

    if (key == 's' && Key_IsDown(K_CTRL)) {
        Prompt_CompleteHistory(&con.chatPrompt, true);
        return;
    }

    if (key == K_UPARROW || (key == 'p' && Key_IsDown(K_CTRL))) {
        Prompt_HistoryUp(&con.chatPrompt);
        return;
    }

    if (key == K_DOWNARROW || (key == 'n' && Key_IsDown(K_CTRL))) {
        Prompt_HistoryDown(&con.chatPrompt);
        return;
    }

    if (IF_KeyEvent(&con.chatPrompt.inputLine, key)) {
        Prompt_ClearState(&con.chatPrompt);
    }
}

void Char_Message(int key)
{
    IF_CharEvent(&con.chatPrompt.inputLine, key);
}
