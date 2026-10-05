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

#include "ui.h"
#include "common/files.h"
#include "common/fuzzy.h"

/*
=======================================================================

PRACTICE JUMPS

Every map, and a field that filters them as it is typed: a fuzzy match
(common/fuzzy.h), the letters in order anywhere in the name ("tj" finds
teamjungle), the best matches first. Enter (or a double click) starts the highlighted map
as a local jump mod game. The field keeps the focus, so typing always
goes to it; the arrows move the list.

=======================================================================
*/

#define MAPS_FIELD_WIDTH    24

typedef struct {
    menuFrameWork_t menu;
    menuField_t     filter;
    menuList_t      list;

    char            **names;        // every map, sorted
    int             numNames;
    int             *scores;        // per map, -1 when the filter rules it out
    int             *order;         // the matches, as indexes into names
    char            last[MAX_QPATH];    // the filter the list was built for
    char            status[64];
} m_maps_t;

static m_maps_t     m_maps;

// best score first, then by name
static int OrderCompare(const void *p1, const void *p2)
{
    int i1 = *(const int *)p1, i2 = *(const int *)p2;

    if (m_maps.scores[i1] != m_maps.scores[i2])
        return m_maps.scores[i2] - m_maps.scores[i1];
    return Q_stricmp(m_maps.names[i1], m_maps.names[i2]);
}

static void Refilter(void)
{
    const char *q = m_maps.filter.field.text;
    int i, n = 0;

    Q_strlcpy(m_maps.last, q, sizeof(m_maps.last));

    for (i = 0; i < m_maps.numNames; i++) {
        m_maps.scores[i] = Fuzzy_Score(q, m_maps.names[i]);
        if (m_maps.scores[i] >= 0)
            m_maps.order[n++] = i;
    }
    qsort(m_maps.order, n, sizeof(m_maps.order[0]), OrderCompare);

    for (i = 0; i < n; i++)
        m_maps.list.items[i] = m_maps.names[m_maps.order[i]];
    m_maps.list.numItems = n;

    // the best match under the cursor
    m_maps.list.curvalue = 0;
    m_maps.list.prestep = 0;
    MenuList_SetValue(&m_maps.list, 0);

    if (*q)
        Q_snprintf(m_maps.status, sizeof(m_maps.status), "%d of %d maps", n, m_maps.numNames);
    else
        Q_snprintf(m_maps.status, sizeof(m_maps.status), "%d maps", m_maps.numNames);
}

static menuSound_t Start(menuCommon_t *self)
{
    if (m_maps.list.curvalue < 0 || m_maps.list.curvalue >= m_maps.list.numItems)
        return QMS_BEEP;

    // the other modes off, which the game checks before jump
    Cbuf_AddText(&cmd_buffer, va("forcemenuoff; set ltk_loadbots 0; set am 0; "
                 "set teamplay 0; set teamdm 0; set ctf 0; set dom 0; set esp 0; "
                 "set use_tourney 0; set deathmatch 1; set jump 1; map \"%s\" force\n",
                 (char *)m_maps.list.items[m_maps.list.curvalue]));
    return QMS_IN;
}

static menuSound_t Keydown(menuFrameWork_t *self, int key)
{
    menuList_t *l = &m_maps.list;

    switch (key) {
    case K_UPARROW:
    case K_KP_UPARROW:
        MenuList_SetValue(l, l->curvalue - 1);
        return QMS_MOVE;
    case K_DOWNARROW:
    case K_KP_DOWNARROW:
        MenuList_SetValue(l, l->curvalue + 1);
        return QMS_MOVE;
    case K_PGUP:
    case K_KP_PGUP:
        MenuList_SetValue(l, l->curvalue - max(l->maxItems - 1, 1));
        return QMS_MOVE;
    case K_PGDN:
    case K_KP_PGDN:
        MenuList_SetValue(l, l->curvalue + max(l->maxItems - 1, 1));
        return QMS_MOVE;
    case K_ENTER:
    case K_KP_ENTER:
        return Start(&l->generic);
    case K_ESCAPE:
    case K_MOUSE1:
    case K_MOUSE2:
    case K_MOUSE3:
    case K_MWHEELUP:
    case K_MWHEELDOWN:
        return QMS_NOTHANDLED;
    }

    // whatever is typed goes to the field, wherever the mouse left focus
    if (!(m_maps.filter.generic.flags & QMF_HASFOCUS))
        Menu_SetFocus(&m_maps.filter.generic);
    return QMS_NOTHANDLED;
}

static void Size(menuFrameWork_t *self)
{
    int w = 32 * CONCHAR_WIDTH;

    m_maps.filter.generic.x = uis.width / 2;
    m_maps.filter.generic.y = CONCHAR_HEIGHT * 4;

    m_maps.list.generic.x = (uis.width - w - MLIST_SCROLLBAR_WIDTH) / 2;
    m_maps.list.generic.y = CONCHAR_HEIGHT * 6;
    m_maps.list.generic.height = uis.height - m_maps.list.generic.y - CONCHAR_HEIGHT * 3;
    m_maps.list.columns[0].width = w;
}

static void Draw(menuFrameWork_t *self)
{
    // a change the field made (a key, a backspace): the list follows
    if (strcmp(m_maps.filter.field.text, m_maps.last))
        Refilter();

    Menu_Draw(self);
    UI_DrawString(uis.width / 2, CONCHAR_HEIGHT * 5 / 2, UI_CENTER | UI_ALTCOLOR,
                  "type to filter, Enter to practice");
    UI_DrawString(uis.width / 2, uis.height - CONCHAR_HEIGHT * 2, UI_CENTER, m_maps.status);
}

static void FreeNames(void)
{
    if (m_maps.names)
        FS_FreeList((void **)m_maps.names);
    Z_Freep(&m_maps.scores);
    Z_Freep(&m_maps.order);
    Z_Freep(&m_maps.list.items);
    m_maps.names = NULL;
    m_maps.numNames = 0;
    m_maps.list.numItems = 0;
}

static bool Push(menuFrameWork_t *self)
{
    FreeNames();

    m_maps.names = (char **)FS_ListFiles("maps", ".bsp", FS_SEARCH_STRIPEXT, &m_maps.numNames);
    if (!m_maps.names || !m_maps.numNames) {
        Com_Printf("No maps found.\n");
        FreeNames();
        return false;
    }
    qsort(m_maps.names, m_maps.numNames, sizeof(m_maps.names[0]), SortStrcmp);

    m_maps.scores = UI_Malloc(m_maps.numNames * sizeof(m_maps.scores[0]));
    m_maps.order = UI_Malloc(m_maps.numNames * sizeof(m_maps.order[0]));
    m_maps.list.items = UI_Malloc(m_maps.numNames * sizeof(m_maps.list.items[0]));

    IF_Init(&m_maps.filter.field, m_maps.filter.width, MAX_QPATH - 1);
    m_maps.last[0] = 1;     // not any filter: the first draw builds the list
    Refilter();
    return true;
}

static void Pop(menuFrameWork_t *self)
{
    FreeNames();
}

static void Free(menuFrameWork_t *self)
{
    FreeNames();
    Z_Free(m_maps.menu.items);
    memset(&m_maps, 0, sizeof(m_maps));
}

void M_Menu_Practice(void)
{
    m_maps.menu.name = "practice";
    m_maps.menu.title = "Practice Jumps";
    m_maps.menu.push = Push;
    m_maps.menu.pop = Pop;
    m_maps.menu.size = Size;
    m_maps.menu.draw = Draw;
    m_maps.menu.keydown = Keydown;
    m_maps.menu.free = Free;
    m_maps.menu.image = uis.backgroundHandle;
    m_maps.menu.color.u32 = uis.color.background.u32;
    m_maps.menu.transparent = uis.transparent;

    m_maps.filter.generic.type = MTYPE_FIELD;
    m_maps.filter.generic.flags = QMF_HASFOCUS;
    m_maps.filter.width = MAPS_FIELD_WIDTH;

    m_maps.list.generic.type = MTYPE_LIST;
    m_maps.list.generic.activate = Start;
    m_maps.list.numcolumns = 1;
    m_maps.list.mlFlags = MLF_SCROLLBAR;
    m_maps.list.columns[0].uiFlags = UI_LEFT;

    Menu_AddItem(&m_maps.menu, &m_maps.filter);
    Menu_AddItem(&m_maps.menu, &m_maps.list);

    List_Append(&ui_menus, &m_maps.menu.entry);
}
