//-----------------------------------------------------------------------------
// Action (formerly Axshun) menu code
// This is directly from Zoid's CTF code.  Thanks to Zoid again.
// -Fireblade
//
// $Id: a_menu.c,v 1.1.1.1 2001/05/06 17:24:26 igor_rock Exp $
//
//-----------------------------------------------------------------------------
// $Log: a_menu.c,v $
// Revision 1.1.1.1  2001/05/06 17:24:26  igor_rock
// This is the PG Bund Edition V1.25 with all stuff laying around here...
//
//-----------------------------------------------------------------------------

#include "g_local.h"

void PMenu_Open (edict_t *ent, pmenu_t *entries, int cur, int num)
{
	pmenuhnd_t *hnd;
	pmenu_t *p;
	int i;

	if (!ent->client)
		return;
#ifndef NO_BOTS
	if( ent->is_bot )
		return;
#endif

	if (ent->client->layout == LAYOUT_MENU) {
		gi.dprintf("warning, ent already has a menu\n");
		PMenu_Close(ent);
	}

	hnd = &ent->client->menu;

	hnd->entries = entries;
	hnd->num = num;

	if (cur < 0 || !entries[cur].SelectFunc)
	{
		for (i = 0, p = entries; i < num; i++, p++)
			if (p->SelectFunc)
				break;
	}
	else
		i = cur;

	if (i >= num)
		hnd->cur = -1;
	else
		hnd->cur = i;

	ent->client->layout = LAYOUT_MENU;

	PMenu_Update(ent);
	gi.unicast(ent, true);
}

void PMenu_Close (edict_t * ent)
{
	if (ent->client->layout != LAYOUT_MENU)
		return;

	memset(&ent->client->menu, 0, sizeof(ent->client->menu));
	ent->client->layout = LAYOUT_NONE;
}

void PMenu_Update (edict_t * ent)
{
	char string[MAX_NET_STRING];	// the client takes this much of a layout
	int i, len;
	pmenu_t *p;
	int x;
	pmenuhnd_t *hnd;
	char *t;
	qboolean alt = false, note;
	int shift;

	if (ent->client->layout != LAYOUT_MENU)
		return;

	hnd = &ent->client->menu;

	strcpy(string, "xv 32 yv 8 ");
	len = strlen(string);

	// a menu with a long row stands further left as a whole: the virtual
	// screen is 320 wide and the rows begin at 64, which leaves 32
	// conchars; from the edge a row of 40 fits
	shift = 0;
	for (i = 0, p = hnd->entries; i < hnd->num; i++, p++) {
		int tlen;

		if (!p->text)
			continue;
		tlen = strlen(p->text) - (*p->text == PMENU_NOTE || *p->text == '*');
		if (p->align == PMENU_ALIGN_LEFT && 64 + tlen * 8 > 320)
			shift = max(shift, min(64, 64 + tlen * 8 - 320));
	}

	for (i = 0, p = hnd->entries; i < hnd->num; i++, p++)
	{
		if (!p->text || !*(p->text))
			continue;		// blank line

		alt = false;

		t = p->text;
		if (*t == '*')
		{
			alt = true;
			t++;
		}

		// a note under a row, in its own colour where the client has
		// layout colours (Q2PRO's); plain text elsewhere
		note = *t == PMENU_NOTE;
		if (note)
			t++;

		if (p->align == PMENU_ALIGN_CENTER)
			x = 196 / 2 - strlen (t) * 4 + 64;
		else if (p->align == PMENU_ALIGN_RIGHT)
			x = 64 + (196 - strlen (t) * 8);
		else
			x = 64;
		x -= shift;

		// a centred row is sent as one, centred by the client on the
		// box's middle: its proportional font draws a row narrower than
		// its conchars would be, and a row placed by its conchars start
		// sat left of the bar under it
		if (p->align == PMENU_ALIGN_CENTER && hnd->cur != i && !note) {
			// a title - a centred row over a bar - a size up, and
			// lifted off the bar (the client's "size", a no-op elsewhere)
			qboolean title = (i == 0 && alt) || (i + 1 < hnd->num && p[1].text &&
				(((byte)p[1].text[0] & 127) == 0x1d || ((byte)p[1].text[1] & 127) == 0x1d));

			Q_snprintf (string + len, sizeof(string)-len, "yv %d xv %d %s%s \"%s\"%s ",
				32 + i * 8 - (title ? 4 : 0), 64 + 196 / 2 - 160 - shift,
				title ? "size 13 " : "", alt ? "cstring2" : "cstring", t, title ? " size 0" : "");
			len = strlen(string);
			if(len >= sizeof(string)-1)
				break;
			continue;
		}

		Q_snprintf (string + len, sizeof(string)-len, "yv %d xv %d ",
			32 + i * 8, x - ((hnd->cur == i) ? 8 : 0));

		len = strlen(string);

		if (hnd->cur == i)
			Q_snprintf (string + len, sizeof(string)-len, "string2 \"\x0d%s\" ", t);
		else if (note)
			Q_snprintf (string + len, sizeof(string)-len, "color #fc6 string \"%s\" color white ", t);
		else if (alt)
			Q_snprintf (string + len, sizeof(string)-len, "string2 \"%s\" ", t);
		else
			Q_snprintf (string + len, sizeof(string)-len, "string \"%s\" ", t);

		len = strlen(string);

		if(len >= sizeof(string)-1)
			break;
	}

	gi.WriteByte(svc_layout);
	gi.WriteString(string);
}

void PMenu_Next (edict_t * ent)
{
	pmenuhnd_t *hnd;
	int i;
	pmenu_t *p;

	if (ent->client->layout != LAYOUT_MENU)
		return;

	hnd = &ent->client->menu;

	if (hnd->cur < 0)
		return;			// no selectable entries

	i = hnd->cur;
	p = hnd->entries + hnd->cur;
	do
	{
		i++, p++;
		if (i == hnd->num)
			i = 0, p = hnd->entries;
		if (p->SelectFunc)
			break;
	}
	while (i != hnd->cur);

	hnd->cur = i;

	PMenu_Update(ent);
	gi.unicast(ent, true);
}

void PMenu_Prev (edict_t * ent)
{
	pmenuhnd_t *hnd;
	int i;
	pmenu_t *p;

	if (ent->client->layout != LAYOUT_MENU)
		return;

	hnd = &ent->client->menu;

	if (hnd->cur < 0)
		return;			// no selectable entries

	i = hnd->cur;
	p = hnd->entries + hnd->cur;
	do
	{
		if (i == 0)
		{
			i = hnd->num - 1;
			p = hnd->entries + i;
		}
		else
			i--, p--;
		if (p->SelectFunc)
			break;
	}
	while (i != hnd->cur);

	hnd->cur = i;

	PMenu_Update(ent);
	gi.unicast(ent, true);
}

void PMenu_Select (edict_t * ent)
{
	pmenuhnd_t *hnd;
	pmenu_t *p;

	if (ent->client->layout != LAYOUT_MENU)
		return;

	hnd = &ent->client->menu;

	if (hnd->cur < 0)
		return;			// no selectable entries

	p = hnd->entries + hnd->cur;

	if (p->SelectFunc)
		p->SelectFunc(ent, p);
}
