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

#pragma once

// Fuzzy matching, as a list filter or a history search takes it: the
// query's letters in order anywhere in the text, case ignored.
//
// Fuzzy_Score returns -1 when they are not there; else a score, higher for
// letters in a run, at the start or a word's start, lower for gaps and a
// longer text - so the closest names sort first. An empty query matches
// everything with 0.
int Fuzzy_Score(const char *query, const char *text);

// Where Fuzzy_Score found the query's letters in text, as offsets into
// it, up to max: their number (0 when it does not match)
int Fuzzy_Positions(const char *query, const char *text, int *pos, int max);
