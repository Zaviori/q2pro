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

#pragma once

#include "common/field.h"
#include "common/cmd.h"

#define HISTORY_SIZE    1024
#define HISTORY_MASK    (HISTORY_SIZE - 1)

#define MIN_MATCHES     64
#define MAX_MATCHES     250000000
#define MAX_MATCH_COLS  6

typedef struct {
    unsigned    inputLineNum;
    unsigned    historyLineNum;

    inputField_t inputLine;
    char        *history[HISTORY_SIZE];
    char        *search;

    int         widthInChars;
    bool        tooMany;

    // the matches of the last ambiguous completion, kept for cycling
    // (TAB again, the arrows); live while the line is as the last
    // step left it, see Prompt_CycleLive()
    char        **cycle;        // sorted
    int         cycleCount;
    int         cycleIndex;     // -1: the common prefix, else the match on the line
    char        *cycleHead;     // the line up to the argument
    char        *cycleTail;     // the arguments after it, or NULL
    char        *cyclePrefix;   // the line with the common prefix
    size_t      cyclePrefixPos;
    char        *cycleLine;     // the line as the last step left it
    size_t      cyclePos;
    char        *cycleQuery;    // the fuzzy query the matches answer, or NULL
    bool        drawMatches;    // the front end draws the matches: do not print them

    void        (* q_printf(1, 2) printf)(const char *fmt, ...);
} commandPrompt_t;

void Prompt_Init(void);
void Prompt_AddMatch(genctx_t *ctx, const char *s);
bool Prompt_MatchPartial(const genctx_t *ctx, const char *s);
void Prompt_CompleteCommand(commandPrompt_t *prompt, bool backslash);
int Prompt_LayoutMatches(const commandPrompt_t *prompt, char **matches, int count,
                         size_t colwidths[MAX_MATCH_COLS], int *numLines);
bool Prompt_CycleLive(const commandPrompt_t *prompt);
bool Prompt_CycleMatches(commandPrompt_t *prompt, int dir, bool column);
void Prompt_CompleteHistory(commandPrompt_t *prompt, bool forward);
void Prompt_ClearState(commandPrompt_t *prompt);
char *Prompt_Action(commandPrompt_t *prompt);
void Prompt_HistoryUp(commandPrompt_t *prompt);
void Prompt_HistoryDown(commandPrompt_t *prompt);
void Prompt_Clear(commandPrompt_t *prompt);
void Prompt_SaveHistory(const commandPrompt_t *prompt, const char *filename, int lines);
void Prompt_LoadHistory(commandPrompt_t *prompt, const char *filename);
// the same, in the file system place fsflags names (FS_PATH_GAME ...);
// the above use the base game's directory
void Prompt_SaveHistoryTo(const commandPrompt_t *prompt, const char *filename, int lines, unsigned fsflags);
bool Prompt_LoadHistoryFrom(commandPrompt_t *prompt, const char *filename, unsigned fsflags);
// the distinct history lines query fuzzy matches (common/fuzzy.h), best
// first and the newest among equals, up to max of them; their number
int Prompt_FuzzyHistory(const commandPrompt_t *prompt, const char *query, const char **out, int max);
