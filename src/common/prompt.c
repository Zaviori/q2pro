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

//
// prompt.c
//

#include "shared/shared.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/field.h"
#include "common/files.h"
#include "common/prompt.h"
#include "common/fuzzy.h"

static cvar_t   *com_completion_mode;
static cvar_t   *com_completion_treshold;

/*
====================
Prompt_LayoutMatches

Fits the matches into as many columns (at most MAX_MATCH_COLS) as the
prompt's width takes, column-major: match k sits in column
k / numLines. Returns the number of columns.
====================
*/
int Prompt_LayoutMatches(const commandPrompt_t *prompt, char **matches, int count,
                         size_t colwidths[MAX_MATCH_COLS], int *numLines)
{
    int numCols = MAX_MATCH_COLS + 1;
    int i, j, k;
    size_t maxlen, len, total;

    // determine number of columns needed
    do {
        numCols--;
        *numLines = (count + numCols - 1) / numCols;
        total = 0;
        for (i = 0; i < numCols; i++) {
            maxlen = 0;
            k = min((i + 1) * *numLines, count);
            for (j = i * *numLines; j < k; j++) {
                len = strlen(matches[j]);
                maxlen = max(maxlen, len);
            }
            maxlen += 2; // account for intercolumn spaces
            maxlen = min(maxlen, prompt->widthInChars);
            colwidths[i] = maxlen;
            total += maxlen;
        }
        if (total < prompt->widthInChars) {
            break; // this number of columns does fit
        }
    } while (numCols > 1);

    return numCols;
}

static void Prompt_ShowMatches(const commandPrompt_t *prompt, char **matches, int count)
{
    int numCols, numLines;
    int i, j, k;
    size_t colwidths[MAX_MATCH_COLS];

    numCols = Prompt_LayoutMatches(prompt, matches, count, colwidths, &numLines);

    for (i = 0; i < numLines; i++) {
        for (j = 0; j < numCols; j++) {
            k = j * numLines + i;
            if (k >= count) {
                break;
            }
            prompt->printf("%*s", -(int)colwidths[j], matches[k]);
        }
        prompt->printf("\n");
    }
}

static void Prompt_ShowIndividualMatches(const commandPrompt_t *prompt, char **matches,
                                         int numCommands, int numAliases, int numCvars)
{
    if (numCommands) {
        qsort(matches, numCommands, sizeof(matches[0]), SortStrcmp);

        prompt->printf("\n%i possible command%s:\n",
                       numCommands, numCommands != 1 ? "s" : "");

        Prompt_ShowMatches(prompt, matches, numCommands);
        matches += numCommands;
    }

    if (numCvars) {
        qsort(matches, numCvars, sizeof(matches[0]), SortStrcmp);

        prompt->printf("\n%i possible variable%s:\n",
                       numCvars, numCvars != 1 ? "s" : "");

        Prompt_ShowMatches(prompt, matches, numCvars);
        matches += numCvars;
    }

    if (numAliases) {
        qsort(matches, numAliases, sizeof(matches[0]), SortStrcmp);

        prompt->printf("\n%i possible alias%s:\n",
                       numAliases, numAliases != 1 ? "es" : "");

        Prompt_ShowMatches(prompt, matches, numAliases);
        matches += numAliases;
    }
}

static bool find_dup(genctx_t *ctx, const char *s)
{
    int i, r;

    for (i = 0; i < ctx->count; i++) {
        if (ctx->ignorecase)
            r = Q_strcasecmp(ctx->matches[i], s);
        else
            r = strcmp(ctx->matches[i], s);

        if (!r)
            return true;
    }

    return false;
}

/*
====================
Prompt_MatchPartial

Whether s answers the partial: starts with it, or, when the context is
fuzzy, has its letters in order anywhere (case never minding then).
====================
*/
bool Prompt_MatchPartial(const genctx_t *ctx, const char *s)
{
    if (ctx->fuzzy)
        return Fuzzy_Score(ctx->partial, s) >= 0;
    if (ctx->ignorecase)
        return !Q_strncasecmp(ctx->partial, s, ctx->length);
    return !strncmp(ctx->partial, s, ctx->length);
}

void Prompt_AddMatch(genctx_t *ctx, const char *s)
{
    if (!*s)
        return;
    if (ctx->count >= ctx->size)
        return;

    if (!Prompt_MatchPartial(ctx, s))
        return;

    if (ctx->ignoredups && find_dup(ctx, s))
        return;

    ctx->matches = Z_Realloc(ctx->matches, Q_ALIGN(ctx->count + 1, MIN_MATCHES) * sizeof(char *));
    ctx->matches[ctx->count++] = Z_CopyString(s);
}

static bool needs_quotes(const char *s)
{
    int c;

    while (*s) {
        c = *s++;
        if (c == '$' || c == ';' || !Q_isgraph(c)) {
            return true;
        }
    }

    return false;
}

static void Prompt_ClearCycle(commandPrompt_t *prompt)
{
    int i;

    for (i = 0; i < prompt->cycleCount; i++) {
        Z_Free(prompt->cycle[i]);
    }
    Z_Freep(&prompt->cycle);
    prompt->cycleCount = 0;
    prompt->cycleIndex = -1;
    Z_Freep(&prompt->cycleHead);
    Z_Freep(&prompt->cycleTail);
    Z_Freep(&prompt->cyclePrefix);
    Z_Freep(&prompt->cycleLine);
    Z_Freep(&prompt->cycleQuery);
}

// the fuzzy matches best first, Fuzzy_Score against the query being
// completed (qsort carries no context), the names breaking ties
static const char *sort_query;

static int SortFuzzy(const void *p1, const void *p2)
{
    const char *s1 = *(const char **)p1, *s2 = *(const char **)p2;
    int d = Fuzzy_Score(sort_query, s2) - Fuzzy_Score(sort_query, s1);

    return d ? d : strcmp(s1, s2);
}

// the command, cvar and alias names, or the argument's completer
static void Prompt_Generate(genctx_t *ctx, int argnum,
                            int *numCommands, int *numCvars, int *numAliases)
{
    if (argnum) {
        // complete a command/cvar argument
        Com_Generic_c(ctx, argnum);
        *numCommands = *numCvars = *numAliases = 0;
    } else {
        // complete a command/cvar/alias name
        Cmd_Command_g(ctx);
        *numCommands = ctx->count;

        Cvar_Variable_g(ctx);
        *numCvars = ctx->count - *numCommands;

        Cmd_Alias_g(ctx);
        *numAliases = ctx->count - *numCvars - *numCommands;
    }
}

/*
====================
Prompt_CycleLive

The kept matches apply while the line is exactly as the last
completion step left it: anything typed, moved or recalled since
means a fresh completion.
====================
*/
bool Prompt_CycleLive(const commandPrompt_t *prompt)
{
    return prompt->cycle && prompt->cycleLine &&
        !strcmp(prompt->inputLine.text, prompt->cycleLine) &&
        prompt->inputLine.cursorPos == prompt->cyclePos;
}

// Puts the match at cycleIndex (or the common prefix) on the line, the
// way a single match would have gone: quoted if need be, a space after,
// the trailing arguments kept, the cursor after the space or the match
static void Prompt_CycleApply(commandPrompt_t *prompt)
{
    inputField_t *f = &prompt->inputLine;
    size_t size = f->maxChars + 1;
    size_t pos;

    if (prompt->cycleIndex < 0) {
        Q_strlcpy(f->text, prompt->cyclePrefix, size);
        pos = prompt->cyclePrefixPos;
    } else {
        const char *m = prompt->cycle[prompt->cycleIndex];

        Q_strlcpy(f->text, prompt->cycleHead, size);
        if (needs_quotes(m)) {
            Q_strlcat(f->text, "\"", size);
            Q_strlcat(f->text, m, size);
            Q_strlcat(f->text, "\"", size);
        } else {
            Q_strlcat(f->text, m, size);
        }
        pos = strlen(f->text);
        Q_strlcat(f->text, " ", size);
        if (prompt->cycleTail)
            Q_strlcat(f->text, prompt->cycleTail, size);
        else
            pos++;
    }

    f->cursorPos = min(pos, f->maxChars - 1);

    Z_Free(prompt->cycleLine);
    prompt->cycleLine = Z_CopyString(f->text);
    prompt->cyclePos = f->cursorPos;
}

/*
====================
Prompt_CycleMatches

Steps the line through the kept matches: dir +1/-1 to the next or
previous, or by a column of the layout the matches were shown in.
Past either end lands on the common prefix again. Returns false
when there is nothing live to cycle.
====================
*/
bool Prompt_CycleMatches(commandPrompt_t *prompt, int dir, bool column)
{
    int i, n, step = 1;

    if (!Prompt_CycleLive(prompt))
        return false;

    if (column) {
        size_t colwidths[MAX_MATCH_COLS];
        Prompt_LayoutMatches(prompt, prompt->cycle, prompt->cycleCount, colwidths, &step);
    }

    n = prompt->cycleCount;
    i = prompt->cycleIndex;
    if (i < 0) {
        i = dir > 0 ? 0 : n - 1;
    } else {
        i += dir > 0 ? step : -step;
        if (i < 0 || i >= n)
            i = -1;
    }

    prompt->cycleIndex = i;
    Prompt_CycleApply(prompt);
    return true;
}

/*
====================
Prompt_CompleteCommand
====================
*/
void Prompt_CompleteCommand(commandPrompt_t *prompt, bool backslash)
{
    inputField_t *inputLine = &prompt->inputLine;
    char *first, *last, *text, **sorted;
    int i, j, c, pos, size, argnum;
    genctx_t ctx;
    int numCommands, numCvars, numAliases;
    bool keep = false;
    char partial[MAX_FIELD_TEXT];

    if (!inputLine->maxChars)
        return;

    // TAB again on an ambiguous line steps through its matches
    if (Prompt_CycleMatches(prompt, 1, false))
        return;
    Prompt_ClearCycle(prompt);

    text = inputLine->text;
    size = inputLine->maxChars + 1;
    pos = inputLine->cursorPos;

    // prepend backslash if missing
    if (backslash) {
        if (*text != '\\' && *text != '/') {
            memmove(text + 1, text, size - 1);
            *text = '\\';
        } else if (pos) {
            pos--;
        }
        text++;
        size--;
    }

    // skip previous parts if command line is multi-part
    for (i = j = c = 0; i < pos && text[i]; i++) {
        if (text[i] == '"')
            c ^= 1;
        else if (!c && text[i] == ';')
            j = i + 1;
    }
    if (j > 0) {
        text += j;
        size -= j;
        pos -= j;
    }

    // parse the input line into tokens
    Cmd_TokenizeString(text, false);

    // determine argument number to be completed
    argnum = Cmd_FindArgForOffset(pos);

    // generate matches
    memset(&ctx, 0, sizeof(ctx));
    ctx.partial = Cmd_Argv(argnum);
    ctx.length = strlen(ctx.partial);
    ctx.argnum = argnum;
    ctx.size = MAX_MATCHES;
    Q_strlcpy(partial, ctx.partial, sizeof(partial));

    Prompt_Generate(&ctx, argnum, &numCommands, &numCvars, &numAliases);

    // nothing starts with it: the names with its letters in order
    // anywhere, best first (a single one goes on the line as any would)
    if (!ctx.count && ctx.length) {
        ctx.fuzzy = true;
        Prompt_Generate(&ctx, argnum, &numCommands, &numCvars, &numAliases);
    }

    if (!ctx.count) {
        pos = strlen(inputLine->text);
        prompt->tooMany = false;
        goto finish; // nothing found
    }

    if (ctx.count > Cvar_ClampInteger(com_completion_treshold, 1, MAX_MATCHES) && !prompt->tooMany) {
        prompt->printf("Press TAB again to display all %d possibilities.\n", ctx.count);
        pos = strlen(inputLine->text);
        prompt->tooMany = true;
        goto finish;
    }

    prompt->tooMany = false;

    // truncate at current argument position
    text[Cmd_ArgOffset(argnum)] = 0;

    // append whitespace if completing a new argument
    if (argnum && argnum == Cmd_Argc()) {
        Q_strlcat(text, " ", size);
    }

    // the line either side of the argument, for cycling
    prompt->cycleHead = Z_CopyString(inputLine->text);
    if (argnum + 1 < Cmd_Argc())
        prompt->cycleTail = Z_CopyString(Cmd_RawArgsFrom(argnum + 1));

    if (ctx.count == 1) {
        // we have finished completion!
        if (needs_quotes(ctx.matches[0])) {
            Q_strlcat(text, "\"", size);
            Q_strlcat(text, ctx.matches[0], size);
            Q_strlcat(text, "\"", size);
        } else {
            Q_strlcat(text, ctx.matches[0], size);
        }

        pos = strlen(inputLine->text);
        Q_strlcat(text, " ", size);

        // copy trailing arguments
        if (argnum + 1 < Cmd_Argc())
            Q_strlcat(text, Cmd_RawArgsFrom(argnum + 1), size);
        else
            pos++;
        goto finish;
    }

    // sort matches alphabethically, or fuzzy ones best first
    sorted = Z_Malloc(ctx.count * sizeof(sorted[0]));
    memcpy(sorted, ctx.matches, ctx.count * sizeof(sorted[0]));
    if (ctx.fuzzy) {
        sort_query = partial;
        qsort(sorted, ctx.count, sizeof(sorted[0]), SortFuzzy);
    } else {
        qsort(sorted, ctx.count, sizeof(sorted[0]), ctx.ignorecase ? SortStricmp : SortStrcmp);
    }

    if (ctx.fuzzy) {
        // fuzzy matches share nothing to fill in: the line stays as typed
        Q_strlcat(text, partial, size);
    } else {
        // copy matching part
        first = sorted[0];
        last = sorted[ctx.count - 1];
        do {
            if (*first != *last && (!ctx.ignorecase || Q_tolower(*first) != Q_tolower(*last))) {
                break;
            }
            first++;
            last++;
        } while (*first);

        c = *first;
        *first = 0;
        Q_strlcat(text, sorted[0], size);
        *first = c;
    }

    pos = strlen(inputLine->text);

    // copy trailing arguments
    if (argnum + 1 < Cmd_Argc()) {
        Q_strlcat(text, " ", size);
        Q_strlcat(text, Cmd_RawArgsFrom(argnum + 1), size);
    }

    // keep the matches for cycling
    prompt->cycle = sorted;
    prompt->cycleCount = ctx.count;
    prompt->cycleIndex = -1;
    prompt->cyclePrefix = Z_CopyString(inputLine->text);
    prompt->cyclePrefixPos = pos;
    if (ctx.fuzzy)
        prompt->cycleQuery = Z_CopyString(partial);
    keep = true;

    if (prompt->drawMatches)
        goto finish;    // the front end shows them itself

    prompt->printf("]\\%s\n", Cmd_ArgsFrom(0));
    if (argnum || ctx.fuzzy) {
        goto multi;     // fuzzy ones in their order, not by type
    }

    switch (com_completion_mode->integer) {
    case 0:
        // print in solid list
        for (i = 0; i < ctx.count; i++) {
            prompt->printf("%s\n", sorted[i]);
        }
        break;
    case 1:
    multi:
        // print in multiple columns
        Prompt_ShowMatches(prompt, sorted, ctx.count);
        break;
    case 2:
    default:
        // resort matches by type and print in multiple columns
        Prompt_ShowIndividualMatches(prompt, ctx.matches, numCommands, numAliases, numCvars);
        break;
    }

finish:
    // free matches, unless kept (sorted) for cycling
    if (!keep) {
        for (i = 0; i < ctx.count; i++) {
            Z_Free(ctx.matches[i]);
        }
        Prompt_ClearCycle(prompt);
    }
    Z_Free(ctx.matches);

    // move cursor
    inputLine->cursorPos = min(pos, inputLine->maxChars - 1);

    if (keep) {
        prompt->cycleLine = Z_CopyString(inputLine->text);
        prompt->cyclePos = inputLine->cursorPos;
    }
}

void Prompt_CompleteHistory(commandPrompt_t *prompt, bool forward)
{
    const char *s, *m = NULL;
    unsigned i, j;

    if (!prompt->search) {
        s = prompt->inputLine.text;
        if (*s == '/' || *s == '\\') {
            s++;
        }
        if (!*s) {
            return;
        }
        prompt->search = Z_CopyString(s);
    }

    if (forward) {
        j = prompt->inputLineNum;
        if (prompt->historyLineNum == j) {
            return;
        }
        for (i = prompt->historyLineNum + 1; i != j; i++) {
            s = prompt->history[i & HISTORY_MASK];
            if (s && strstr(s, prompt->search)) {
                if (strcmp(s, prompt->inputLine.text)) {
                    m = s;
                    break;
                }
            }
        }
    } else {
        j = prompt->inputLineNum - HISTORY_SIZE;
        if (prompt->historyLineNum == j) {
            return;
        }
        for (i = prompt->historyLineNum - 1; i != j; i--) {
            s = prompt->history[i & HISTORY_MASK];
            if (s && strstr(s, prompt->search)) {
                if (strcmp(s, prompt->inputLine.text)) {
                    m = s;
                    break;
                }
            }
        }
    }

    if (!m) {
        return;
    }

    prompt->historyLineNum = i;
    IF_Replace(&prompt->inputLine, m);
}

void Prompt_ClearState(commandPrompt_t *prompt)
{
    prompt->tooMany = false;
    Z_Freep(&prompt->search);
    Prompt_ClearCycle(prompt);
}

/*
====================
Prompt_Action

User just pressed enter
====================
*/
char *Prompt_Action(commandPrompt_t *prompt)
{
    const char *s = prompt->inputLine.text;
    int i, j;

    Prompt_ClearState(prompt);
    if (s[0] == 0 || ((s[0] == '/' || s[0] == '\\') && s[1] == 0)) {
        IF_Clear(&prompt->inputLine);
        return NULL; // empty line
    }

    // save current line in history
    i = prompt->inputLineNum & HISTORY_MASK;
    j = (prompt->inputLineNum - 1) & HISTORY_MASK;
    if (!prompt->history[j] || strcmp(prompt->history[j], s)) {
        Z_Free(prompt->history[i]);
        prompt->history[i] = Z_CopyString(s);
        prompt->inputLineNum++;
    } else {
        i = j;
    }

    // stop history search
    prompt->historyLineNum = prompt->inputLineNum;

    IF_Clear(&prompt->inputLine);

    return prompt->history[i];
}

/*
====================
Prompt_HistoryUp
====================
*/
void Prompt_HistoryUp(commandPrompt_t *prompt)
{
    int i;

    Prompt_ClearState(prompt);

    if (prompt->historyLineNum == prompt->inputLineNum) {
        // save current line in history
        i = prompt->inputLineNum & HISTORY_MASK;
        Z_Free(prompt->history[i]);
        prompt->history[i] = Z_CopyString(prompt->inputLine.text);
    }

    if (prompt->inputLineNum - prompt->historyLineNum < HISTORY_SIZE &&
        prompt->history[(prompt->historyLineNum - 1) & HISTORY_MASK]) {
        prompt->historyLineNum--;
    }

    i = prompt->historyLineNum & HISTORY_MASK;
    IF_Replace(&prompt->inputLine, prompt->history[i]);
}

/*
====================
Prompt_HistoryDown
====================
*/
void Prompt_HistoryDown(commandPrompt_t *prompt)
{
    int i;

    Prompt_ClearState(prompt);

    if (prompt->historyLineNum == prompt->inputLineNum) {
        return;
    }

    prompt->historyLineNum++;

    i = prompt->historyLineNum & HISTORY_MASK;
    IF_Replace(&prompt->inputLine, prompt->history[i]);
}

/*
====================
Prompt_Clear
====================
*/
void Prompt_Clear(commandPrompt_t *prompt)
{
    int i;

    Prompt_ClearState(prompt);

    for (i = 0; i < HISTORY_SIZE; i++) {
        Z_Freep(&prompt->history[i]);
    }

    prompt->historyLineNum = 0;
    prompt->inputLineNum = 0;

    IF_Clear(&prompt->inputLine);
}

void Prompt_SaveHistoryTo(const commandPrompt_t *prompt, const char *filename, int lines, unsigned fsflags)
{
    qhandle_t f;
    const char *s;
    unsigned i;

    if (lines < 1) {
        return;
    }

    FS_OpenFile(filename, &f, FS_MODE_WRITE | fsflags);
    if (!f) {
        return;
    }

    if (lines > HISTORY_SIZE) {
        lines = HISTORY_SIZE;
    }

    for (i = prompt->inputLineNum - lines; i != prompt->inputLineNum; i++) {
        s = prompt->history[i & HISTORY_MASK];
        if (s && *s) {
            FS_FPrintf(f, "%s\n", s);
        }
    }

    FS_CloseFile(f);
}

void Prompt_SaveHistory(const commandPrompt_t *prompt, const char *filename, int lines)
{
    Prompt_SaveHistoryTo(prompt, filename, lines, FS_PATH_BASE);
}

bool Prompt_LoadHistoryFrom(commandPrompt_t *prompt, const char *filename, unsigned fsflags)
{
    char buffer[MAX_FIELD_TEXT];
    qhandle_t f;
    unsigned i;

    FS_OpenFile(filename, &f, FS_MODE_READ | FS_TYPE_REAL | FS_DIR_HOME | fsflags);
    if (!f) {
        return false;
    }

    i = 0;
    while (1) {
        int len = FS_ReadLine(f, buffer, sizeof(buffer));
        if (len <= 0)
            break;
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r'))
            len--;
        if (!len)
            continue;
        buffer[len] = 0;
        Z_Free(prompt->history[i & HISTORY_MASK]);
        prompt->history[i & HISTORY_MASK] = Z_CopyString(buffer);
        i++;
    }

    FS_CloseFile(f);

    prompt->historyLineNum = i;
    prompt->inputLineNum = i;
    return true;
}

void Prompt_LoadHistory(commandPrompt_t *prompt, const char *filename)
{
    Prompt_LoadHistoryFrom(prompt, filename, FS_PATH_BASE);
}

static int                      fuzzy_scores[HISTORY_SIZE];

static int fuzzycmp(const void *p1, const void *p2)
{
    int i1 = *(const int *)p1, i2 = *(const int *)p2;

    if (fuzzy_scores[i1] != fuzzy_scores[i2])
        return fuzzy_scores[i2] - fuzzy_scores[i1];
    return i1 - i2;     // gathered newest first
}

int Prompt_FuzzyHistory(const commandPrompt_t *prompt, const char *query, const char **out, int max)
{
    const char *lines[HISTORY_SIZE];
    int order[HISTORY_SIZE];
    int i, j, n = 0;
    unsigned k;

    // the distinct lines, newest first; the leading slash is not matched
    for (k = prompt->inputLineNum - 1; k != prompt->inputLineNum - 1 - HISTORY_SIZE; k--) {
        const char *s = prompt->history[k & HISTORY_MASK];
        if (!s || !*s)
            continue;
        for (j = 0; j < n && strcmp(lines[j], s); j++)
            ;
        if (j < n)
            continue;
        const char *t = (*s == '/' || *s == '\\') ? s + 1 : s;
        int score = Fuzzy_Score(query, t);
        if (score < 0)
            continue;
        fuzzy_scores[n] = score;
        order[n] = n;
        lines[n++] = s;
    }

    qsort(order, n, sizeof(order[0]), fuzzycmp);

    for (i = 0; i < n && i < max; i++)
        out[i] = lines[order[i]];
    return i;
}

/*
====================
Prompt_Init
====================
*/
void Prompt_Init(void)
{
    com_completion_mode = Cvar_Get("com_completion_mode", "1", 0);
    com_completion_treshold = Cvar_Get("com_completion_treshold", "50", 0);
}
