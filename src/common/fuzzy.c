/*
Copyright (C) 2026 the q2pro AQtion contributors

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

#include "shared/shared.h"
#include "common/fuzzy.h"

int Fuzzy_Score(const char *query, const char *text)
{
    int score = 0, run = 0, prev = -1, si = 0;
    int tlen = strlen(text), qlen = strlen(query);

    if (!qlen)
        return 0;

    for (int qi = 0; qi < qlen; qi++) {
        int c = Q_tolower(query[qi]);

        // the next place it occurs
        while (si < tlen && Q_tolower(text[si]) != c)
            si++;
        if (si == tlen)
            return -1;

        score += 10;
        if (si == 0)
            score += 15;
        else if (!Q_isalnum(text[si - 1]) ||
                 Q_isdigit(text[si]) != Q_isdigit(text[si - 1]))
            score += 8;     // a word's start: after "_", " ", or a digit run

        if (prev >= 0 && si == prev + 1) {
            run++;
            score += 6 * run;
        } else {
            run = 0;
            if (prev >= 0)
                score -= min(si - prev - 1, 6);
        }

        prev = si++;
    }

    return score + 30 - min(tlen - qlen, 30) / 2;
}
