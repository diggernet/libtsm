/*
 * libtsm - VT Emulator
 *
 * Copyright (c) 2019-2020 Fredrik Wikstrom <fredrik@a500.org>
 * Copyright (c) 2018 Aetf <aetf@unlimitedcodeworks.xyz>
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Virtual Terminal Emulator
 * This is the VT implementation. It is written from scratch. It uses the
 * screen state-machine as output and is tightly bound to it. It supports
 * functionality from vt100 up to vt500 series. It doesn't implement an
 * explicitly selected terminal but tries to support the most important commands
 * to be compatible with existing implementations. However, full vt102
 * compatibility is the least that is provided.
 *
 * The main parser in this file controls the parser-state and dispatches the
 * actions to the related handlers. The parser is based on the state-diagram
 * from Paul Williams: http://vt100.net/emu/
 * It is written from scratch, though.
 * This parser is fully compatible up to the vt500 series. It requires UTF-8 and
 * does not support any other input encoding. The G0 and G1 sets are therefore
 * defined as subsets of UTF-8. You may still map G0-G3 into GL, though.
 *
 * However, the CSI/DCS/etc handlers are not designed after a specific VT
 * series. We try to support all vt102 commands but implement several other
 * often used sequences, too. Feel free to add further.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "shl-llog.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#ifdef _MSC_VER
#define ESC "\x1b"
#else
#define ESC "\e"
#endif

#define LLOG_SUBSYSTEM "tsm-vte"

/* Input parser states */
enum parser_state {
	STATE_NONE,		/* placeholder */
	STATE_GROUND,		/* initial state and ground */
	STATE_ESC,		/* ESC sequence was started */
	STATE_ESC_INT,		/* intermediate escape characters */
	STATE_CSI_ENTRY,	/* starting CSI sequence */
	STATE_CSI_PARAM,	/* CSI parameters */
	STATE_CSI_INT,		/* intermediate CSI characters */
	STATE_CSI_IGNORE,	/* CSI error; ignore this CSI sequence */
	STATE_DCS_ENTRY,	/* starting DCS sequence */
	STATE_DCS_PARAM,	/* DCS parameters */
	STATE_DCS_INT,		/* intermediate DCS characters */
	STATE_DCS_PASS,		/* DCS data passthrough */
	STATE_DCS_IGNORE,	/* DCS error; ignore this DCS sequence */
	STATE_OSC_STRING,	/* parsing OCS sequence */
	STATE_ST_IGNORE,	/* unimplemented seq; ignore until ST */
	STATE_NUM
};

/* Input parser actions */
enum parser_action {
	ACTION_NONE,		/* placeholder */
	ACTION_IGNORE,		/* ignore the character entirely */
	ACTION_PRINT,		/* print the character on the console */
	ACTION_EXECUTE,		/* execute single control character (C0/C1) */
	ACTION_CLEAR,		/* clear current parameter state */
	ACTION_COLLECT,		/* collect intermediate character */
	ACTION_PARAM,		/* collect parameter character */
	ACTION_ESC_DISPATCH,	/* dispatch escape sequence */
	ACTION_CSI_DISPATCH,	/* dispatch csi sequence */
	ACTION_DCS_START,	/* start of DCS data */
	ACTION_DCS_COLLECT,	/* collect DCS data */
	ACTION_DCS_END,		/* end of DCS data */
	ACTION_OSC_START,	/* start of OSC data */
	ACTION_OSC_COLLECT,	/* collect OSC data */
	ACTION_OSC_END,		/* end of OSC data */
	ACTION_NUM
};

/* CSI flags */
#define CSI_BANG	0x0001		/* CSI: ! */
#define CSI_CASH	0x0002		/* CSI: $ */
#define CSI_WHAT	0x0004		/* CSI: ? */
#define CSI_GT		0x0008		/* CSI: > */
#define CSI_SPACE	0x0010		/* CSI:   */
#define CSI_SQUOTE	0x0020		/* CSI: ' */
#define CSI_DQUOTE	0x0040		/* CSI: " */
#define CSI_MULT	0x0080		/* CSI: * */
#define CSI_PLUS	0x0100		/* CSI: + */
#define CSI_POPEN	0x0200		/* CSI: ( */
#define CSI_PCLOSE	0x0400		/* CSI: ) */

/* max CSI arguments */
#define CSI_ARG_MAX 16

/* max length of an OSC code */
#define OSC_MAX_LEN 128

/* terminal flags */
#define FLAG_CURSOR_KEY_MODE			0x00000001 /* DEC cursor key mode */
#define FLAG_KEYPAD_APPLICATION_MODE		0x00000002 /* DEC keypad application mode; TODO: toggle on numlock? */
#define FLAG_LINE_FEED_NEW_LINE_MODE		0x00000004 /* DEC line-feed/new-line mode */
#define FLAG_8BIT_MODE				0x00000008 /* Disable UTF-8 mode and enable 8bit compatible mode */
#define FLAG_7BIT_MODE				0x00000010 /* Disable 8bit mode and use 7bit compatible mode */
#define FLAG_USE_C1				0x00000020 /* Explicitly use 8bit C1 codes; TODO: implement */
#define FLAG_KEYBOARD_ACTION_MODE		0x00000040 /* Disable keyboard; TODO: implement? */
#define FLAG_INSERT_REPLACE_MODE		0x00000080 /* Enable insert mode */
#define FLAG_SEND_RECEIVE_MODE			0x00000100 /* Disable local echo */
#define FLAG_TEXT_CURSOR_MODE			0x00000200 /* Show cursor */
#define FLAG_INVERSE_SCREEN_MODE		0x00000400 /* Inverse colors */
#define FLAG_ORIGIN_MODE			0x00000800 /* Relative origin for cursor */
#define FLAG_AUTO_WRAP_MODE			0x00001000 /* Auto line wrap mode */
#define FLAG_AUTO_REPEAT_MODE			0x00002000 /* Auto repeat key press; TODO: implement */
#define FLAG_NATIONAL_CHARSET_MODE		0x00004000 /* Send keys from nation charsets; TODO: implement */
#define FLAG_BACKGROUND_COLOR_ERASE_MODE	0x00008000 /* Set background color on erase (bce) */
#define FLAG_PREPEND_ESCAPE			0x00010000 /* Prepend escape character to next output */
#define FLAG_TITE_INHIBIT_MODE			0x00020000 /* Prevent switching to alternate screen buffer */

struct vte_saved_state {
	unsigned int cursor_x;
	unsigned int cursor_y;
	struct tsm_screen_attr cattr;
	tsm_vte_charset **gl;
	tsm_vte_charset **gr;
	bool wrap_mode;
	bool origin_mode;
};

struct tsm_vte {
	unsigned long ref;
	tsm_log_t llog;
	void *llog_data;
	struct tsm_screen *con;
	tsm_vte_write_cb write_cb;
	void *data;
	char *palette_name;

	struct tsm_utf8_mach mach;
	unsigned long parse_cnt;

	unsigned int state;
	unsigned int csi_argc;
	int csi_argv[CSI_ARG_MAX];
	unsigned int csi_flags;

	tsm_vte_bell_cb bell_cb;
	void *bell_data;

	tsm_vte_osc_cb osc_cb;
	void *osc_data;
	unsigned int osc_len;
	char osc_arg[OSC_MAX_LEN];

	uint8_t (*custom_palette_storage)[3];
	uint8_t (*palette)[3];
	struct tsm_screen_attr def_attr;
	struct tsm_screen_attr cattr;
	unsigned int flags;

	tsm_vte_charset **gl;
	tsm_vte_charset **gr;
	tsm_vte_charset **glt;
	tsm_vte_charset **grt;
	tsm_vte_charset *g0;
	tsm_vte_charset *g1;
	tsm_vte_charset *g2;
	tsm_vte_charset *g3;

	struct vte_saved_state saved_state;
	unsigned int alt_cursor_x;
	unsigned int alt_cursor_y;
};

static uint8_t color_palette[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {   0,   0,   0 }, /* black */
	[TSM_COLOR_RED]           = { 205,   0,   0 }, /* red */
	[TSM_COLOR_GREEN]         = {   0, 205,   0 }, /* green */
	[TSM_COLOR_YELLOW]        = { 205, 205,   0 }, /* yellow */
	[TSM_COLOR_BLUE]          = {   0,   0, 238 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 205,   0, 205 }, /* magenta */
	[TSM_COLOR_CYAN]          = {   0, 205, 205 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 229, 229, 229 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = { 127, 127, 127 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 255,   0,   0 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = {   0, 255,   0 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 255, 255,   0 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = {  92,  92, 255 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 255,   0, 255 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = {   0, 255, 255 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 255, 255, 255 }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 229, 229, 229 }, /* light grey */
	[TSM_COLOR_BACKGROUND]    = {   0,   0,   0 }, /* black */
};

static uint8_t color_palette_solarized[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {   7,  54,  66 }, /* black */
	[TSM_COLOR_RED]           = { 220,  50,  47 }, /* red */
	[TSM_COLOR_GREEN]         = { 133, 153,   0 }, /* green */
	[TSM_COLOR_YELLOW]        = { 181, 137,   0 }, /* yellow */
	[TSM_COLOR_BLUE]          = {  38, 139, 210 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 211,  54, 130 }, /* magenta */
	[TSM_COLOR_CYAN]          = {  42, 161, 152 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = {   0,  43,  54 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 203,  75,  22 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = {  88, 110, 117 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 101, 123, 131 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = { 131, 148, 150 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 108, 113, 196 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = { 147, 161, 161 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 253, 246, 227 }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_BACKGROUND]    = {   7,  54,  66 }, /* black */
};

static uint8_t color_palette_solarized_black[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {   0,   0,   0 }, /* black */
	[TSM_COLOR_RED]           = { 220,  50,  47 }, /* red */
	[TSM_COLOR_GREEN]         = { 133, 153,   0 }, /* green */
	[TSM_COLOR_YELLOW]        = { 181, 137,   0 }, /* yellow */
	[TSM_COLOR_BLUE]          = {  38, 139, 210 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 211,  54, 130 }, /* magenta */
	[TSM_COLOR_CYAN]          = {  42, 161, 152 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = {   0,  43,  54 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 203,  75,  22 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = {  88, 110, 117 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 101, 123, 131 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = { 131, 148, 150 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 108, 113, 196 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = { 147, 161, 161 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 253, 246, 227 }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_BACKGROUND]    = {   0,   0,   0 }, /* black */
};

static uint8_t color_palette_solarized_white[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {   7,  54,  66 }, /* black */
	[TSM_COLOR_RED]           = { 220,  50,  47 }, /* red */
	[TSM_COLOR_GREEN]         = { 133, 153,   0 }, /* green */
	[TSM_COLOR_YELLOW]        = { 181, 137,   0 }, /* yellow */
	[TSM_COLOR_BLUE]          = {  38, 139, 210 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 211,  54, 130 }, /* magenta */
	[TSM_COLOR_CYAN]          = {  42, 161, 152 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = {   0,  43,  54 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 203,  75,  22 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = {  88, 110, 117 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 101, 123, 131 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = { 131, 148, 150 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 108, 113, 196 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = { 147, 161, 161 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 253, 246, 227 }, /* white */

	[TSM_COLOR_FOREGROUND]    = {   7,  54,  66 }, /* black */
	[TSM_COLOR_BACKGROUND]    = { 238, 232, 213 }, /* light grey */
};

static uint8_t color_palette_soft_black[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = { 0x3f, 0x3f, 0x3f }, /* black */
	[TSM_COLOR_RED]           = { 0x70, 0x50, 0x50 }, /* red */
	[TSM_COLOR_GREEN]         = { 0x60, 0xb4, 0x8a }, /* green */
	[TSM_COLOR_YELLOW]        = { 0xdf, 0xaf, 0x8f }, /* yellow */
	[TSM_COLOR_BLUE]          = { 0x9a, 0xb8, 0xd7 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 0xdc, 0x8c, 0xc3 }, /* magenta */
	[TSM_COLOR_CYAN]          = { 0x8c, 0xd0, 0xd3 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 0xff, 0xff, 0xff }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = { 0x70, 0x90, 0x80 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 0xdc, 0xa3, 0xa3 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = { 0x72, 0xd5, 0xa3 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 0xf0, 0xdf, 0xaf }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = { 0x94, 0xbf, 0xf3 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 0xec, 0x93, 0xd3 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = { 0x93, 0xe0, 0xe3 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 0xdc, 0xdc, 0xcc }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 0xdc, 0xdc, 0xcc }, /* white */
	[TSM_COLOR_BACKGROUND]    = { 0x2c, 0x2c, 0x2c }, /* light grey */
};

static uint8_t color_palette_base16_dark[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = { 0x00, 0x00, 0x00 }, /* black */
	[TSM_COLOR_RED]           = { 0xab, 0x46, 0x42 }, /* red, base08 */
	[TSM_COLOR_GREEN]         = { 0xa1, 0xb5, 0x6c }, /* green, base0B */
	[TSM_COLOR_YELLOW]        = { 0xf7, 0xca, 0x88 }, /* yellow, base0A */
	[TSM_COLOR_BLUE]          = { 0x7c, 0xaf, 0xc2 }, /* blue, base0D */
	[TSM_COLOR_MAGENTA]       = { 0xba, 0x8b, 0xaf }, /* magenta, base0E */
	[TSM_COLOR_CYAN]          = { 0x86, 0xc1, 0xb9 }, /* cyan, base0C */
	[TSM_COLOR_LIGHT_GREY]    = { 0xaa, 0xaa, 0xaa }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = { 0x55, 0x55, 0x55 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 0xab, 0x46, 0x42 }, /* red, base08 */
	[TSM_COLOR_LIGHT_GREEN]   = { 0xa1, 0xb5, 0x6c }, /* green, base0B */
	[TSM_COLOR_LIGHT_YELLOW]  = { 0xf7, 0xca, 0x88 }, /* yellow, base0A */
	[TSM_COLOR_LIGHT_BLUE]    = { 0x7c, 0xaf, 0xc2 }, /* blue, base0D */
	[TSM_COLOR_LIGHT_MAGENTA] = { 0xba, 0x8b, 0xaf }, /* magenta, base0E */
	[TSM_COLOR_LIGHT_CYAN]    = { 0x86, 0xc1, 0xb9 }, /* cyan, base0C */
	[TSM_COLOR_WHITE]         = { 0xff, 0xff, 0xff }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 0xd8, 0xd8, 0xd8 }, /* light grey */
	[TSM_COLOR_BACKGROUND]    = { 0x18, 0x18, 0x18 }, /* dark grey */
};

static uint8_t color_palette_base16_light[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = { 0x00, 0x00, 0x00 }, /* black */
	[TSM_COLOR_RED]           = { 0xab, 0x46, 0x42 }, /* red, base08 */
	[TSM_COLOR_GREEN]         = { 0xa1, 0xb5, 0x6c }, /* green, base0B */
	[TSM_COLOR_YELLOW]        = { 0xf7, 0xca, 0x88 }, /* yellow, base0A */
	[TSM_COLOR_BLUE]          = { 0x7c, 0xaf, 0xc2 }, /* blue, base0D */
	[TSM_COLOR_MAGENTA]       = { 0xba, 0x8b, 0xaf }, /* magenta, base0E */
	[TSM_COLOR_CYAN]          = { 0x86, 0xc1, 0xb9 }, /* cyan, base0C */
	[TSM_COLOR_LIGHT_GREY]    = { 0xaa, 0xaa, 0xaa }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = { 0x55, 0x55, 0x55 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 0xab, 0x46, 0x42 }, /* red, base08 */
	[TSM_COLOR_LIGHT_GREEN]   = { 0xa1, 0xb5, 0x6c }, /* green, base0B */
	[TSM_COLOR_LIGHT_YELLOW]  = { 0xf7, 0xca, 0x88 }, /* yellow, base0A */
	[TSM_COLOR_LIGHT_BLUE]    = { 0x7c, 0xaf, 0xc2 }, /* blue, base0D */
	[TSM_COLOR_LIGHT_MAGENTA] = { 0xba, 0x8b, 0xaf }, /* magenta, base0E */
	[TSM_COLOR_LIGHT_CYAN]    = { 0x86, 0xc1, 0xb9 }, /* cyan, base0C */
	[TSM_COLOR_WHITE]         = { 0xff, 0xff, 0xff }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 0x18, 0x18, 0x18 }, /* dark grey */
	[TSM_COLOR_BACKGROUND]    = { 0xd8, 0xd8, 0xd8 }, /* light grey */
};

static uint8_t (*get_palette(struct tsm_vte *vte))[3]
{
	if (!vte->palette_name)
		return color_palette;

	if (!strcmp(vte->palette_name, "custom") && vte->custom_palette_storage)
		return vte->custom_palette_storage;
	if (!strcmp(vte->palette_name, "solarized"))
		return color_palette_solarized;
	if (!strcmp(vte->palette_name, "solarized-black"))
		return color_palette_solarized_black;
	if (!strcmp(vte->palette_name, "solarized-white"))
		return color_palette_solarized_white;
	if (!strcmp(vte->palette_name, "soft-black"))
		return color_palette_soft_black;
	if (!strcmp(vte->palette_name, "base16-dark"))
		return color_palette_base16_dark;
	if (!strcmp(vte->palette_name, "base16-light"))
		return color_palette_base16_light;

	return color_palette;
}

/* Several effects may occur when non-RGB colors are used. For instance, if bold
 * is enabled, then a dark color code is always converted to a light color to
 * simulate bold (even though bold may actually be supported!). To support this,
 * we need to differentiate between a set color-code and a set rgb-color.
 * This function actually converts a set color-code into an RGB color. This must
 * be called before passing the attribute to the console layer so the console
 * layer can always work with RGB values and does not have to care for color
 * codes. */
static void to_rgb(struct tsm_vte *vte, struct tsm_screen_attr *attr)
{
	int8_t code;

	code = attr->fccode;
	if (code >= 0) {
		/* bold causes light colors */
		if (attr->bold && code < 8)
			code += 8;
		if (code >= TSM_COLOR_NUM)
			code = TSM_COLOR_FOREGROUND;

		attr->fr = vte->palette[code][0];
		attr->fg = vte->palette[code][1];
		attr->fb = vte->palette[code][2];
	}

	code = attr->bccode;
	if (code >= 0) {
		if (code >= TSM_COLOR_NUM)
			code = TSM_COLOR_BACKGROUND;

		attr->br = vte->palette[code][0];
		attr->bg = vte->palette[code][1];
		attr->bb = vte->palette[code][2];
	}
}

static void copy_fcolor(struct tsm_screen_attr *dest,
			const struct tsm_screen_attr *src)
{
	dest->fccode = src->fccode;
	dest->fr = src->fr;
	dest->fg = src->fg;
	dest->fb = src->fb;
}

static void copy_bcolor(struct tsm_screen_attr *dest,
			const struct tsm_screen_attr *src)
{
	dest->bccode = src->bccode;
	dest->br = src->br;
	dest->bg = src->bg;
	dest->bb = src->bb;
}

SHL_EXPORT
int tsm_vte_new(struct tsm_vte **out, struct tsm_screen *con,
		tsm_vte_write_cb write_cb, void *data,
		tsm_log_t log, void *log_data)
{
	struct tsm_vte *vte;

	if (!out || !con || !write_cb)
		return -EINVAL;

	vte = malloc(sizeof(*vte));
	if (!vte)
		return -ENOMEM;

	memset(vte, 0, sizeof(*vte));
	vte->ref = 1;
	vte->llog = log;
	vte->llog_data = log_data;
	vte->con = con;
	vte->write_cb = write_cb;
	vte->data = data;
	vte->osc_cb = NULL;
	vte->osc_data = NULL;
	vte->custom_palette_storage = NULL;
	vte->palette = get_palette(vte);
	vte->def_attr.fccode = TSM_COLOR_FOREGROUND;
	vte->def_attr.bccode = TSM_COLOR_BACKGROUND;
	to_rgb(vte, &vte->def_attr);

	tsm_utf8_mach_init(&vte->mach);

	tsm_vte_reset(vte);
	tsm_screen_erase_screen(vte->con, false);

	llog_debug(vte, "new vte object");
	tsm_screen_ref(vte->con);
	*out = vte;
	return 0;
}

SHL_EXPORT
void tsm_vte_ref(struct tsm_vte *vte)
{
	if (!vte)
		return;

	vte->ref++;
}

SHL_EXPORT
void tsm_vte_unref(struct tsm_vte *vte)
{
	if (!vte || !vte->ref)
		return;

	if (--vte->ref)
		return;

	llog_debug(vte, "destroying vte object");
	free(vte->palette_name);
	tsm_screen_unref(vte->con);
	free(vte->custom_palette_storage);
	free(vte);
}

SHL_EXPORT
void tsm_vte_set_bell_cb(struct tsm_vte *vte, tsm_vte_bell_cb bell_cb, void *bell_data)
{
	if (!vte)
		return;

	vte->bell_cb = bell_cb;
	vte->bell_data = bell_data;
}

SHL_EXPORT
void tsm_vte_set_osc_cb(struct tsm_vte *vte, tsm_vte_osc_cb osc_cb, void *osc_data)
{
	if (!vte)
		return;

	vte->osc_cb = osc_cb;
	vte->osc_data = osc_data;
}

static int vte_update_palette(struct tsm_vte *vte)
{
	vte->palette = get_palette(vte);
	vte->def_attr.fccode = TSM_COLOR_FOREGROUND;
	vte->def_attr.bccode = TSM_COLOR_BACKGROUND;

	to_rgb(vte, &vte->def_attr);
	memcpy(&vte->cattr, &vte->def_attr, sizeof(vte->cattr));

	tsm_screen_set_def_attr(vte->con, &vte->def_attr);
	tsm_screen_erase_screen(vte->con, false);

	return 0;
}

SHL_EXPORT
int tsm_vte_set_palette(struct tsm_vte *vte, const char *palette_name)
{
	char *tmp = NULL;

	if (!vte)
		return -EINVAL;

	if (palette_name) {
		tmp = strdup(palette_name);
		if (!tmp)
			return -ENOMEM;
	}

	free(vte->palette_name);
	vte->palette_name = tmp;

	return vte_update_palette(vte);
}

SHL_EXPORT
int tsm_vte_set_custom_palette(struct tsm_vte *vte, uint8_t (*palette)[3])
{
	const size_t palette_byte_size = sizeof(color_palette);
	uint8_t (*tmp)[3] = NULL;

	if (!vte)
		return -EINVAL;

	if (palette) {
		tmp = malloc(palette_byte_size);
		if (!tmp)
			return -ENOMEM;
		memcpy(tmp, palette, palette_byte_size);
	}

	free(vte->custom_palette_storage);
	vte->custom_palette_storage = tmp;

	return vte_update_palette(vte);
}

SHL_EXPORT
void tsm_vte_get_def_attr(struct tsm_vte *vte, struct tsm_screen_attr *out)
{
	if (!vte || !out)
		return;

	memcpy(out, &vte->def_attr, sizeof(*out));
}

/*
 * Write raw byte-stream to pty.
 * When writing data to the client we must make sure that we send the correct
 * encoding. For backwards-compatibility reasons we should always send 7bit
 * characters exclusively. However, when FLAG_7BIT_MODE is not set, then we can
 * also send raw 8bit characters. For instance, in FLAG_8BIT_MODE we can use the
 * GR characters as keyboard input and send them directly or even use the C1
 * escape characters. In unicode mode (default) we can send multi-byte utf-8
 * characters which are also 8bit. When sending these characters, set the \raw
 * flag to true so this function does not perform debug checks on data we send.
 * If debugging is disabled, these checks are also disabled and won't affect
 * performance.
 * For better debugging, we also use the __LINE__ and __FILE__ macros. Use the
 * vte_write() and vte_write_raw() macros below for more convenient use.
 *
 * As a rule of thumb do never send 8bit characters in escape sequences and also
 * avoid all 8bit escape codes including the C1 codes. This will guarantee that
 * all kind of clients are always compatible to us.
 *
 * If SEND_RECEIVE_MODE is off (that is, local echo is on) we have to send all
 * data directly to ourself again. However, we must avoid recursion when
 * tsm_vte_input() itself calls vte_write*(), therefore, we increase the
 * PARSER counter when entering tsm_vte_input() and reset it when leaving it
 * so we never echo data that origins from tsm_vte_input().
 * But note that SEND_RECEIVE_MODE is inherently broken for escape sequences
 * that request answers. That is, if we send a request to the client that awaits
 * a response and parse that request via local echo ourself, then we will also
 * send a response to the client even though he didn't request one. This
 * recursion fix does not avoid this but only prevents us from endless loops
 * here. Anyway, only few applications rely on local echo so we can safely
 * ignore this.
 */
static void vte_write_debug(struct tsm_vte *vte, const char *u8, size_t len,
			    bool raw, const char *file, int line)
{
#ifdef BUILD_ENABLE_DEBUG
	/* in debug mode we check that escape sequences are always <0x7f so they
	 * are correctly parsed by non-unicode and non-8bit-mode clients. */
	size_t i;

	if (!raw) {
		for (i = 0; i < len; ++i) {
			if (u8[i] & 0x80)
				llog_warning(vte, "sending 8bit character inline to client in %s:%d",
					     file, line);
		}
	}
#endif

	/* in local echo mode, directly parse the data again */
	if (!vte->parse_cnt && !(vte->flags & FLAG_SEND_RECEIVE_MODE)) {
		if (vte->flags & FLAG_PREPEND_ESCAPE)
			tsm_vte_input(vte, ESC, 1);
		tsm_vte_input(vte, u8, len);
	}

	if (vte->flags & FLAG_PREPEND_ESCAPE)
		vte->write_cb(vte, ESC, 1, vte->data);
	vte->write_cb(vte, u8, len, vte->data);

	vte->flags &= ~FLAG_PREPEND_ESCAPE;
}

#define vte_write(_vte, _u8, _len) \
	vte_write_debug((_vte), (_u8), (_len), false, __FILE__, __LINE__)
#define vte_write_raw(_vte, _u8, _len) \
	vte_write_debug((_vte), (_u8), (_len), true, __FILE__, __LINE__)

/* write to console */
static void write_console(struct tsm_vte *vte, tsm_symbol_t sym)
{
	to_rgb(vte, &vte->cattr);
	tsm_screen_write(vte->con, sym, &vte->cattr);
}

static void reset_state(struct tsm_vte *vte)
{
	vte->saved_state.cursor_x = 0;
	vte->saved_state.cursor_y = 0;
	vte->saved_state.origin_mode = false;
	vte->saved_state.wrap_mode = true;
	vte->saved_state.gl = &vte->g0;
	vte->saved_state.gr = &vte->g1;

	copy_fcolor(&vte->saved_state.cattr, &vte->def_attr);
	copy_bcolor(&vte->saved_state.cattr, &vte->def_attr);
	vte->saved_state.cattr.bold = 0;
	vte->saved_state.cattr.italic = 0;
	vte->saved_state.cattr.underline = 0;
	vte->saved_state.cattr.inverse = 0;
	vte->saved_state.cattr.protect = 0;
	vte->saved_state.cattr.blink = 0;
}

static void save_state(struct tsm_vte *vte)
{
	vte->saved_state.cursor_x = tsm_screen_get_cursor_x(vte->con);
	vte->saved_state.cursor_y = tsm_screen_get_cursor_y(vte->con);
	vte->saved_state.cattr = vte->cattr;
	vte->saved_state.gl = vte->gl;
	vte->saved_state.gr = vte->gr;
	vte->saved_state.wrap_mode = vte->flags & FLAG_AUTO_WRAP_MODE;
	vte->saved_state.origin_mode = vte->flags & FLAG_ORIGIN_MODE;
}

static void restore_state(struct tsm_vte *vte)
{
	tsm_screen_move_to(vte->con, vte->saved_state.cursor_x,
			       vte->saved_state.cursor_y);
	vte->cattr = vte->saved_state.cattr;
	to_rgb(vte, &vte->cattr);
	if (vte->flags & FLAG_BACKGROUND_COLOR_ERASE_MODE)
		tsm_screen_set_def_attr(vte->con, &vte->cattr);
	vte->gl = vte->saved_state.gl;
	vte->gr = vte->saved_state.gr;

	if (vte->saved_state.wrap_mode) {
		vte->flags |= FLAG_AUTO_WRAP_MODE;
		tsm_screen_set_flags(vte->con, TSM_SCREEN_AUTO_WRAP);
	} else {
		vte->flags &= ~FLAG_AUTO_WRAP_MODE;
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_AUTO_WRAP);
	}

	if (vte->saved_state.origin_mode) {
		vte->flags |= FLAG_ORIGIN_MODE;
		tsm_screen_set_flags(vte->con, TSM_SCREEN_REL_ORIGIN);
	} else {
		vte->flags &= ~FLAG_ORIGIN_MODE;
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_REL_ORIGIN);
	}
}

/*
 * Reset VTE state
 * This performs a soft reset of the VTE. That is, everything is reset to the
 * same state as when the VTE was created. This does not affect the console,
 * though.
 */
SHL_EXPORT
void tsm_vte_reset(struct tsm_vte *vte)
{
	if (!vte)
		return;

	vte->flags = 0;
	vte->flags |= FLAG_TEXT_CURSOR_MODE;
	vte->flags |= FLAG_AUTO_REPEAT_MODE;
	vte->flags |= FLAG_SEND_RECEIVE_MODE;
	vte->flags |= FLAG_AUTO_WRAP_MODE;
	vte->flags |= FLAG_BACKGROUND_COLOR_ERASE_MODE;
	tsm_screen_reset(vte->con);
	tsm_screen_set_flags(vte->con, TSM_SCREEN_AUTO_WRAP);

	tsm_utf8_mach_reset(&vte->mach);
	vte->state = STATE_GROUND;
	vte->gl = &vte->g0;
	vte->gr = &vte->g1;
	vte->glt = NULL;
	vte->grt = NULL;
	vte->g0 = &tsm_vte_unicode_lower;
	vte->g1 = &tsm_vte_unicode_upper;
	vte->g2 = &tsm_vte_unicode_lower;
	vte->g3 = &tsm_vte_unicode_upper;

	memcpy(&vte->cattr, &vte->def_attr, sizeof(vte->cattr));
	to_rgb(vte, &vte->cattr);
	tsm_screen_set_def_attr(vte->con, &vte->def_attr);

	reset_state(vte);
}

SHL_EXPORT
void tsm_vte_hard_reset(struct tsm_vte *vte)
{
	if (!vte)
		return;

	tsm_vte_reset(vte);
	tsm_screen_erase_screen(vte->con, false);
	tsm_screen_clear_sb(vte->con);
	tsm_screen_move_to(vte->con, 0, 0);
}

static void send_primary_da(struct tsm_vte *vte)
{
	const static char str[] = ESC "[?60;1;6;9;15c";
	vte_write(vte, str, sizeof(str) - 1);
}

/* execute control character (C0 or C1) */
static void do_execute(struct tsm_vte *vte, uint32_t ctrl)
{
	switch (ctrl) {
	case 0x00: /* NUL */
		/* Ignore on input */
		break;
	case 0x05: /* ENQ */
		/* Transmit answerback message */
		/* TODO: is there a better answer than ACK?  */
		vte_write(vte, "\x06", 1);
		break;
	case 0x07: /* BEL */
		/* Sound bell tone */
		if (vte->bell_cb)
			vte->bell_cb(vte, vte->bell_data);
		break;
	case 0x08: /* BS */
		/* Move cursor one position left */
		tsm_screen_move_left(vte->con, 1);
		break;
	case 0x09: /* HT */
		/* Move to next tab stop or end of line */
		tsm_screen_tab_right(vte->con, 1);
		break;
	case 0x0a: /* LF */
	case 0x0b: /* VT */
	case 0x0c: /* FF */
		/* Line feed or newline (CR/NL mode) */
		if (vte->flags & FLAG_LINE_FEED_NEW_LINE_MODE)
			tsm_screen_newline(vte->con);
		else
			tsm_screen_move_down(vte->con, 1, true);
		break;
	case 0x0d: /* CR */
		/* Move cursor to left margin */
		tsm_screen_move_line_home(vte->con);
		break;
	case 0x0e: /* SO */
		/* Map G1 character set into GL */
		vte->gl = &vte->g1;
		break;
	case 0x0f: /* SI */
		/* Map G0 character set into GL */
		vte->gl = &vte->g0;
		break;
	case 0x11: /* XON */
		/* Resume transmission */
		/* TODO */
		break;
	case 0x13: /* XOFF */
		/* Stop transmission */
		/* TODO */
		break;
	case 0x18: /* CAN */
		/* Cancel escape sequence */
		/* nothing to do here */
		break;
	case 0x1a: /* SUB */
		/* Discard current escape sequence and show err-sym */
		write_console(vte, 0xbf);
		break;
	case 0x1b: /* ESC */
		/* Invokes an escape sequence */
		/* nothing to do here */
		break;
	case 0x1f: /* DEL */
		/* Ignored */
		break;
	case 0x84: /* IND */
		/* Move down one row, perform scroll-up if needed */
		tsm_screen_move_down(vte->con, 1, true);
		break;
	case 0x85: /* NEL */
		/* CR/NL with scroll-up if needed */
		tsm_screen_newline(vte->con);
		break;
	case 0x88: /* HTS */
		/* Set tab stop at current position */
		tsm_screen_set_tabstop(vte->con);
		break;
	case 0x8d: /* RI */
		/* Move up one row, perform scroll-down if needed */
		tsm_screen_move_up(vte->con, 1, true);
		break;
	case 0x8e: /* SS2 */
		/* Temporarily map G2 into GL for next char only */
		vte->glt = &vte->g2;
		break;
	case 0x8f: /* SS3 */
		/* Temporarily map G3 into GL for next char only */
		vte->glt = &vte->g3;
		break;
	case 0x9a: /* DECID */
		/* Send device attributes response like ANSI DA */
		send_primary_da(vte);
		break;
	case 0x9c: /* ST */
		/* End control string */
		/* nothing to do here */
		break;
	default:
		llog_debug(vte, "unhandled control char %u", ctrl);
	}
}

static void do_clear(struct tsm_vte *vte)
{
	int i;

	vte->csi_argc = 0;
	for (i = 0; i < CSI_ARG_MAX; ++i)
		vte->csi_argv[i] = -1;
	vte->csi_flags = 0;

	vte->osc_len = 0;
	memset(vte->osc_arg, 0, sizeof(vte->osc_arg));
}

static void do_collect(struct tsm_vte *vte, uint32_t data)
{
	switch (data) {
	case '!':
		vte->csi_flags |= CSI_BANG;
		break;
	case '$':
		vte->csi_flags |= CSI_CASH;
		break;
	case '?':
		vte->csi_flags |= CSI_WHAT;
		break;
	case '>':
		vte->csi_flags |= CSI_GT;
		break;
	case ' ':
		vte->csi_flags |= CSI_SPACE;
		break;
	case '\'':
		vte->csi_flags |= CSI_SQUOTE;
		break;
	case '"':
		vte->csi_flags |= CSI_DQUOTE;
		break;
	case '*':
		vte->csi_flags |= CSI_MULT;
		break;
	case '+':
		vte->csi_flags |= CSI_PLUS;
		break;
	case '(':
		vte->csi_flags |= CSI_POPEN;
		break;
	case ')':
		vte->csi_flags |= CSI_PCLOSE;
		break;
	}
}

static void do_param(struct tsm_vte *vte, uint32_t data)
{
	int new;

	if (data == ';') {
		if (vte->csi_argc < CSI_ARG_MAX)
			vte->csi_argc++;
		return;
	}

	if (vte->csi_argc >= CSI_ARG_MAX)
		return;

	/* avoid integer overflows; max allowed value is 16384 anyway */
	if (vte->csi_argv[vte->csi_argc] > 0xffff)
		return;

	if (data >= '0' && data <= '9') {
		new = vte->csi_argv[vte->csi_argc];
		if (new <= 0)
			new = data - '0';
		else
			new = new * 10 + data - '0';
		vte->csi_argv[vte->csi_argc] = new;
	}
}

static bool set_charset(struct tsm_vte *vte, tsm_vte_charset *set)
{
	if (vte->csi_flags & CSI_POPEN)
		vte->g0 = set;
	else if (vte->csi_flags & CSI_PCLOSE)
		vte->g1 = set;
	else if (vte->csi_flags & CSI_MULT)
		vte->g2 = set;
	else if (vte->csi_flags & CSI_PLUS)
		vte->g3 = set;
	else
		return false;

	return true;
}

static void do_esc(struct tsm_vte *vte, uint32_t data)
{
	switch (data) {
	case 'B': /* map ASCII into G0-G3 */
		if (set_charset(vte, &tsm_vte_unicode_lower))
			return;
		break;
	case '<': /* map DEC supplemental into G0-G3 */
		if (set_charset(vte, &tsm_vte_dec_supplemental_graphics))
			return;
		break;
	case '0': /* map DEC special into G0-G3 */
		if (set_charset(vte, &tsm_vte_dec_special_graphics))
			return;
		break;
	case 'A': /* map British into G0-G3 */
		/* TODO: create British charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case '4': /* map Dutch into G0-G3 */
		/* TODO: create Dutch charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'C':
	case '5': /* map Finnish into G0-G3 */
		/* TODO: create Finnish charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'R': /* map French into G0-G3 */
		/* TODO: create French charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'Q': /* map French-Canadian into G0-G3 */
		/* TODO: create French-Canadian charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'K': /* map German into G0-G3 */
		/* TODO: create German charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'Y': /* map Italian into G0-G3 */
		/* TODO: create Italian charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'E':
	case '6': /* map Norwegian/Danish into G0-G3 */
		/* TODO: create Norwegian/Danish charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'Z': /* map Spanish into G0-G3 */
		/* TODO: create Spanish charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'H':
	case '7': /* map Swedish into G0-G3 */
		/* TODO: create Swedish charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case '=': /* map Swiss into G0-G3 */
		/* TODO: create Swiss charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'F':
		if (vte->csi_flags & CSI_SPACE) {
			/* S7C1T */
			/* Disable 8bit C1 mode */
			vte->flags &= ~FLAG_USE_C1;
			return;
		}
		break;
	case 'G':
		if (vte->csi_flags & CSI_SPACE) {
			/* S8C1T */
			/* Enable 8bit C1 mode */
			vte->flags |= FLAG_USE_C1;
			return;
		}
		break;
	}

	/* everything below is only valid without CSI flags */
	if (vte->csi_flags) {
		llog_debug(vte, "unhandled escape seq %u", data);
		return;
	}

	switch (data) {
	case 'D': /* IND */
		/* Move down one row, perform scroll-up if needed */
		tsm_screen_move_down(vte->con, 1, true);
		break;
	case 'E': /* NEL */
		/* CR/NL with scroll-up if needed */
		tsm_screen_newline(vte->con);
		break;
	case 'H': /* HTS */
		/* Set tab stop at current position */
		tsm_screen_set_tabstop(vte->con);
		break;
	case 'M': /* RI */
		/* Move up one row, perform scroll-down if needed */
		tsm_screen_move_up(vte->con, 1, true);
		break;
	case 'N': /* SS2 */
		/* Temporarily map G2 into GL for next char only */
		vte->glt = &vte->g2;
		break;
	case 'O': /* SS3 */
		/* Temporarily map G3 into GL for next char only */
		vte->glt = &vte->g3;
		break;
	case 'Z': /* DECID */
		/* Send device attributes response like ANSI DA */
		send_primary_da(vte);
		break;
	case '\\': /* ST */
		/* End control string */
		/* nothing to do here */
		break;
	case '~': /* LS1R */
		/* Invoke G1 into GR */
		vte->gr = &vte->g1;
		break;
	case 'n': /* LS2 */
		/* Invoke G2 into GL */
		vte->gl = &vte->g2;
		break;
	case '}': /* LS2R */
		/* Invoke G2 into GR */
		vte->gr = &vte->g2;
		break;
	case 'o': /* LS3 */
		/* Invoke G3 into GL */
		vte->gl = &vte->g3;
		break;
	case '|': /* LS3R */
		/* Invoke G3 into GR */
		vte->gr = &vte->g3;
		break;
	case '=': /* DECKPAM */
		/* Set application keypad mode */
		vte->flags |= FLAG_KEYPAD_APPLICATION_MODE;
		break;
	case '>': /* DECKPNM */
		/* Set numeric keypad mode */
		vte->flags &= ~FLAG_KEYPAD_APPLICATION_MODE;
		break;
	case 'c': /* RIS */
		/* hard reset */
		tsm_vte_hard_reset(vte);
		break;
	case '7': /* DECSC */
		/* save console state */
		save_state(vte);
		break;
	case '8': /* DECRC */
		/* restore console state */
		restore_state(vte);
		break;
	default:
		llog_debug(vte, "unhandled escape seq %u", data);
	}
}

static void csi_attribute(struct tsm_vte *vte)
{
	static const uint8_t bval[6] = { 0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff };
	unsigned int i, code, val;
	uint8_t cr, cg, cb;

	if (vte->csi_argc <= 1 && vte->csi_argv[0] == -1) {
		vte->csi_argc = 1;
		vte->csi_argv[0] = 0;
	}

	for (i = 0; i < vte->csi_argc; ++i) {
		switch (vte->csi_argv[i]) {
		case -1:
			break;
		case 0:
			copy_fcolor(&vte->cattr, &vte->def_attr);
			copy_bcolor(&vte->cattr, &vte->def_attr);
			vte->cattr.bold = 0;
			vte->cattr.italic = 0;
			vte->cattr.underline = 0;
			vte->cattr.inverse = 0;
			vte->cattr.blink = 0;
			break;
		case 1:
			vte->cattr.bold = 1;
			break;
		case 3:
			vte->cattr.italic = 1;
			break;
		case 4:
			vte->cattr.underline = 1;
			break;
		case 5:
			vte->cattr.blink = 1;
			break;
		case 7:
			vte->cattr.inverse = 1;
			break;
		case 22:
			vte->cattr.bold = 0;
			break;
		case 23:
			vte->cattr.italic = 0;
			break;
		case 24:
			vte->cattr.underline = 0;
			break;
		case 25:
			vte->cattr.blink = 0;
			break;
		case 27:
			vte->cattr.inverse = 0;
			break;
		case 30:
			vte->cattr.fccode = TSM_COLOR_BLACK;
			break;
		case 31:
			vte->cattr.fccode = TSM_COLOR_RED;
			break;
		case 32:
			vte->cattr.fccode = TSM_COLOR_GREEN;
			break;
		case 33:
			vte->cattr.fccode = TSM_COLOR_YELLOW;
			break;
		case 34:
			vte->cattr.fccode = TSM_COLOR_BLUE;
			break;
		case 35:
			vte->cattr.fccode = TSM_COLOR_MAGENTA;
			break;
		case 36:
			vte->cattr.fccode = TSM_COLOR_CYAN;
			break;
		case 37:
			vte->cattr.fccode = TSM_COLOR_LIGHT_GREY;
			break;
		case 39:
			copy_fcolor(&vte->cattr, &vte->def_attr);
			break;
		case 40:
			vte->cattr.bccode = TSM_COLOR_BLACK;
			break;
		case 41:
			vte->cattr.bccode = TSM_COLOR_RED;
			break;
		case 42:
			vte->cattr.bccode = TSM_COLOR_GREEN;
			break;
		case 43:
			vte->cattr.bccode = TSM_COLOR_YELLOW;
			break;
		case 44:
			vte->cattr.bccode = TSM_COLOR_BLUE;
			break;
		case 45:
			vte->cattr.bccode = TSM_COLOR_MAGENTA;
			break;
		case 46:
			vte->cattr.bccode = TSM_COLOR_CYAN;
			break;
		case 47:
			vte->cattr.bccode = TSM_COLOR_LIGHT_GREY;
			break;
		case 49:
			copy_bcolor(&vte->cattr, &vte->def_attr);
			break;
		case 90:
			vte->cattr.fccode = TSM_COLOR_DARK_GREY;
			break;
		case 91:
			vte->cattr.fccode = TSM_COLOR_LIGHT_RED;
			break;
		case 92:
			vte->cattr.fccode = TSM_COLOR_LIGHT_GREEN;
			break;
		case 93:
			vte->cattr.fccode = TSM_COLOR_LIGHT_YELLOW;
			break;
		case 94:
			vte->cattr.fccode = TSM_COLOR_LIGHT_BLUE;
			break;
		case 95:
			vte->cattr.fccode = TSM_COLOR_LIGHT_MAGENTA;
			break;
		case 96:
			vte->cattr.fccode = TSM_COLOR_LIGHT_CYAN;
			break;
		case 97:
			vte->cattr.fccode = TSM_COLOR_WHITE;
			break;
		case 100:
			vte->cattr.bccode = TSM_COLOR_DARK_GREY;
			break;
		case 101:
			vte->cattr.bccode = TSM_COLOR_LIGHT_RED;
			break;
		case 102:
			vte->cattr.bccode = TSM_COLOR_LIGHT_GREEN;
			break;
		case 103:
			vte->cattr.bccode = TSM_COLOR_LIGHT_YELLOW;
			break;
		case 104:
			vte->cattr.bccode = TSM_COLOR_LIGHT_BLUE;
			break;
		case 105:
			vte->cattr.bccode = TSM_COLOR_LIGHT_MAGENTA;
			break;
		case 106:
			vte->cattr.bccode = TSM_COLOR_LIGHT_CYAN;
			break;
		case 107:
			vte->cattr.bccode = TSM_COLOR_WHITE;
			break;
		case 38:
			/* fallthrough */
		case 48:
			val = vte->csi_argv[i];
			if (vte->csi_argv[i + 1] == 5) { // 256color mode
				if (i + 2 >= vte->csi_argc ||
					vte->csi_argv[i + 2] < 0) {
					llog_debug(vte, "invalid 256color SGR");
					break;
				}
				code = vte->csi_argv[i + 2];
				if (code < 16) {
					//no change
					cb = cg = cr = 0;
				} else if (code < 232) {
					code -= 16;
					cb = bval[code % 6];
					code /= 6;
					cg = bval[code % 6];
					code /= 6;
					cr = bval[code % 6];
					code = -1;
				} else {
					code = (code - 232) * 10 + 8;
					cr = code;
					cg = code;
					cb = code;
					code = -1;
				}
				i += 2;
			} else if (vte->csi_argv[i + 1] == 2) {  // true color mode
				if (i + 4 >= vte->csi_argc ||
					vte->csi_argv[i + 2] < 0 ||
					vte->csi_argv[i + 3] < 0 ||
					vte->csi_argv[i + 4] < 0) {
						llog_debug(vte, "invalid true color SGR");
						break;
					}
				cr = vte->csi_argv[i + 2];
				cg = vte->csi_argv[i + 3];
				cb = vte->csi_argv[i + 4];
				code = -1;
				i += 4;
			} else {
				llog_debug(vte, "invalid SGR");
				break;
			}
			if (val == 38) {
				vte->cattr.fccode = code;
				vte->cattr.fr = cr;
				vte->cattr.fg = cg;
				vte->cattr.fb = cb;
			} else {
				vte->cattr.bccode = code;
				vte->cattr.br = cr;
				vte->cattr.bg = cg;
				vte->cattr.bb = cb;
			}

			break;
		default:
			llog_debug(vte, "unhandled SGR attr %i",
				   vte->csi_argv[i]);
		}
	}

	to_rgb(vte, &vte->cattr);
	if (vte->flags & FLAG_BACKGROUND_COLOR_ERASE_MODE)
		tsm_screen_set_def_attr(vte->con, &vte->cattr);
}

static void csi_soft_reset(struct tsm_vte *vte)
{
	tsm_vte_reset(vte);
}

static void csi_compat_mode(struct tsm_vte *vte)
{
	/* always perform soft reset */
	csi_soft_reset(vte);

	if (vte->csi_argv[0] == 61) {
		/* Switching to VT100 compatibility mode. We do
		 * not support this mode, so ignore it. In fact,
		 * we are almost compatible to it, anyway, so
		 * there is no need to explicitly select it.
		 * However, we enable 7bit mode to avoid
		 * character-table problems */
		vte->flags |= FLAG_7BIT_MODE;
		vte->g0 = &tsm_vte_unicode_lower;
		vte->g1 = &tsm_vte_dec_supplemental_graphics;
	} else if (vte->csi_argv[0] == 62 ||
		   vte->csi_argv[0] == 63 ||
		   vte->csi_argv[0] == 64) {
		/* Switching to VT2/3/4 compatibility mode. We
		 * are always compatible with this so ignore it.
		 * We always send 7bit controls so we also do
		 * not care for the parameter value here that
		 * select the control-mode.
		 * VT220 defines argument 2 as 7bit mode but
		 * VT3xx up to VT5xx use it as 8bit mode. We
		 * choose to conform with the latter here.
		 * We also enable 8bit mode when VT220
		 * compatibility is requested explicitly. */
		if (vte->csi_argv[1] == 1 ||
		    vte->csi_argv[1] == 2)
			vte->flags |= FLAG_USE_C1;

		vte->flags |= FLAG_8BIT_MODE;
		vte->g0 = &tsm_vte_unicode_lower;
		vte->g1 = &tsm_vte_dec_supplemental_graphics;
	} else {
		llog_debug(vte, "unhandled DECSCL 'p' CSI %i, switching to utf-8 mode again",
			   vte->csi_argv[0]);
	}
}

static inline void set_reset_flag(struct tsm_vte *vte, bool set,
				  unsigned int flag)
{
	if (set)
		vte->flags |= flag;
	else
		vte->flags &= ~flag;
}

static void csi_mode(struct tsm_vte *vte, bool set)
{
	unsigned int i;

	for (i = 0; i < vte->csi_argc; ++i) {
		if (!(vte->csi_flags & CSI_WHAT)) {
			switch (vte->csi_argv[i]) {
			case -1:
				continue;
			case 2: /* KAM */
				set_reset_flag(vte, set,
					       FLAG_KEYBOARD_ACTION_MODE);
				continue;
			case 4: /* IRM */
				set_reset_flag(vte, set,
					       FLAG_INSERT_REPLACE_MODE);
				if (set)
					tsm_screen_set_flags(vte->con,
						TSM_SCREEN_INSERT_MODE);
				else
					tsm_screen_reset_flags(vte->con,
						TSM_SCREEN_INSERT_MODE);
				continue;
			case 12: /* SRM */
				set_reset_flag(vte, set,
					       FLAG_SEND_RECEIVE_MODE);
				continue;
			case 20: /* LNM */
				set_reset_flag(vte, set,
					       FLAG_LINE_FEED_NEW_LINE_MODE);
				continue;
			default:
				llog_debug(vte, "unknown non-DEC (Re)Set-Mode %d",
					   vte->csi_argv[i]);
				continue;
			}
		}

		switch (vte->csi_argv[i]) {
		case -1:
			continue;
		case 1: /* DECCKM */
			set_reset_flag(vte, set, FLAG_CURSOR_KEY_MODE);
			continue;
		case 2: /* DECANM */
			/* Select VT52 mode */
			/* We do not support VT52 mode. Is there any reason why
			 * we should support it? We ignore it here and do not
			 * mark it as to-do item unless someone has strong
			 * arguments to support it. */
			continue;
		case 3: /* DECCOLM */
			/* If set, select 132 column mode, otherwise use 80
			 * column mode. If neither is selected explicitly, we
			 * use dynamic mode, that is, we send SIGWCH when the
			 * size changes and we allow arbitrary buffer
			 * dimensions. On soft-reset, we automatically fall back
			 * to the default, that is, dynamic mode.
			 * Dynamic-mode can be forced to a static mode in the
			 * config. That is, every time dynamic-mode becomes
			 * active, the terminal will be set to the dimensions
			 * that were selected in the config. This allows setting
			 * a fixed size for the terminal regardless of the
			 * display size.
			 * TODO: Implement this */
			continue;
		case 4: /* DECSCLM */
			/* Select smooth scrolling. We do not support the
			 * classic smooth scrolling because we have a scrollback
			 * buffer. There is no need to implement smooth
			 * scrolling so ignore this here. */
			continue;
		case 5: /* DECSCNM */
			set_reset_flag(vte, set, FLAG_INVERSE_SCREEN_MODE);
			if (set)
				tsm_screen_set_flags(vte->con,
						TSM_SCREEN_INVERSE);
			else
				tsm_screen_reset_flags(vte->con,
						TSM_SCREEN_INVERSE);
			continue;
		case 6: /* DECOM */
			set_reset_flag(vte, set, FLAG_ORIGIN_MODE);
			if (set)
				tsm_screen_set_flags(vte->con,
						TSM_SCREEN_REL_ORIGIN);
			else
				tsm_screen_reset_flags(vte->con,
						TSM_SCREEN_REL_ORIGIN);
			continue;
		case 7: /* DECAWN */
			set_reset_flag(vte, set, FLAG_AUTO_WRAP_MODE);
			if (set)
				tsm_screen_set_flags(vte->con,
						TSM_SCREEN_AUTO_WRAP);
			else
				tsm_screen_reset_flags(vte->con,
						TSM_SCREEN_AUTO_WRAP);
			continue;
		case 8: /* DECARM */
			set_reset_flag(vte, set, FLAG_AUTO_REPEAT_MODE);
			continue;
		case 12: /* blinking cursor */
			/* TODO: implement */
			continue;
		case 18: /* DECPFF */
			/* If set, a form feed (FF) is sent to the printer after
			 * every screen that is printed. We don't have printers
			 * these days directly attached to terminals so we
			 * ignore this here. */
			continue;
		case 19: /* DECPEX */
			/* If set, the full screen is printed instead of
			 * scrolling region only. We have no printer so ignore
			 * this mode. */
			continue;
		case 25: /* DECTCEM */
			set_reset_flag(vte, set, FLAG_TEXT_CURSOR_MODE);
			if (set)
				tsm_screen_reset_flags(vte->con,
						TSM_SCREEN_HIDE_CURSOR);
			else
				tsm_screen_set_flags(vte->con,
						TSM_SCREEN_HIDE_CURSOR);
			continue;
		case 42: /* DECNRCM */
			set_reset_flag(vte, set, FLAG_NATIONAL_CHARSET_MODE);
			continue;
		case 47: /* Alternate screen buffer */
			if (vte->flags & FLAG_TITE_INHIBIT_MODE)
				continue;

			if (set)
				tsm_screen_set_flags(vte->con,
						     TSM_SCREEN_ALTERNATE);
			else
				tsm_screen_reset_flags(vte->con,
						       TSM_SCREEN_ALTERNATE);
			continue;
		case 1047: /* Alternate screen buffer with post-erase */
			if (vte->flags & FLAG_TITE_INHIBIT_MODE)
				continue;

			if (set) {
				tsm_screen_set_flags(vte->con,
						     TSM_SCREEN_ALTERNATE);
			} else {
				tsm_screen_erase_screen(vte->con, false);
				tsm_screen_reset_flags(vte->con,
						       TSM_SCREEN_ALTERNATE);
			}
			continue;
		case 1048: /* Set/Reset alternate-screen buffer cursor */
			if (vte->flags & FLAG_TITE_INHIBIT_MODE)
				continue;

			if (set) {
				vte->alt_cursor_x =
					tsm_screen_get_cursor_x(vte->con);
				vte->alt_cursor_y =
					tsm_screen_get_cursor_y(vte->con);
			} else {
				tsm_screen_move_to(vte->con, vte->alt_cursor_x,
						   vte->alt_cursor_y);
			}
			continue;
		case 1049: /* Alternate screen buffer with pre-erase+cursor */
			if (vte->flags & FLAG_TITE_INHIBIT_MODE)
				continue;

			if (set) {
				vte->alt_cursor_x =
					tsm_screen_get_cursor_x(vte->con);
				vte->alt_cursor_y =
					tsm_screen_get_cursor_y(vte->con);
				tsm_screen_set_flags(vte->con,
						     TSM_SCREEN_ALTERNATE);
				tsm_screen_erase_screen(vte->con, false);
			} else {
				tsm_screen_reset_flags(vte->con,
						       TSM_SCREEN_ALTERNATE);
				tsm_screen_move_to(vte->con, vte->alt_cursor_x,
						   vte->alt_cursor_y);
			}
			continue;
		default:
			llog_debug(vte, "unknown DEC %set-Mode %d",
				   set?"S":"Res", vte->csi_argv[i]);
			continue;
		}
	}
}

static void csi_dev_attr(struct tsm_vte *vte)
{
	if (vte->csi_argc <= 1 && vte->csi_argv[0] <= 0) {
		if (vte->csi_flags == 0) {
			send_primary_da(vte);
			return;
		} else if (vte->csi_flags & CSI_GT) {
			vte_write(vte, ESC "[>1;1;0c", 9);
			return;
		}
	}

	llog_debug(vte, "unhandled DA: %x %d %d %d...", vte->csi_flags,
		   vte->csi_argv[0], vte->csi_argv[1], vte->csi_argv[2]);
}

static void csi_dsr(struct tsm_vte *vte)
{
	char buf[64];
	unsigned int x, y, len;

	if (vte->csi_argv[0] == 5) {
		vte_write(vte, ESC "[0n", 4);
	} else if (vte->csi_argv[0] == 6) {
		x = tsm_screen_get_cursor_x(vte->con);
		y = tsm_screen_get_cursor_y(vte->con);
		len = snprintf(buf, sizeof(buf), ESC "[%u;%uR", y + 1, x + 1);
		if (len >= sizeof(buf))
			vte_write(vte, ESC "[0;0R", 6);
		else
			vte_write(vte, buf, len);
	}
}

static void do_csi(struct tsm_vte *vte, uint32_t data)
{
	int num, x, y, upper, lower;
	bool protect;

	if (vte->csi_argc < CSI_ARG_MAX)
		vte->csi_argc++;

	switch (data) {
	case 'A': /* CUU */
		/* move cursor up */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_move_up(vte->con, num, false);
		break;
	case 'B': /* CUD */
		/* move cursor down */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_move_down(vte->con, num, false);
		break;
	case 'C': /* CUF */
		/* move cursor forward */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_move_right(vte->con, num);
		break;
	case 'D': /* CUB */
		/* move cursor backward */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_move_left(vte->con, num);
		break;
	case 'd': /* VPA */
		/* Vertical Line Position Absolute */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		x = tsm_screen_get_cursor_x(vte->con);
		tsm_screen_move_to(vte->con, x, num - 1);
		break;
	case 'e': /* VPR */
		/* Vertical Line Position Relative */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		x = tsm_screen_get_cursor_x(vte->con);
		y = tsm_screen_get_cursor_y(vte->con);
		tsm_screen_move_to(vte->con, x, y + num);
		break;
	case 'H': /* CUP */
	case 'f': /* HVP */
		/* position cursor */
		x = vte->csi_argv[0];
		if (x <= 0)
			x = 1;
		y = vte->csi_argv[1];
		if (y <= 0)
			y = 1;
		tsm_screen_move_to(vte->con, y - 1, x - 1);
		break;
	case 'G': /* CHA */
		/* Cursor Character Absolute */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		y = tsm_screen_get_cursor_y(vte->con);
		tsm_screen_move_to(vte->con, num - 1, y);
		break;
	case 'J':
		if (vte->csi_flags & CSI_WHAT)
			protect = true;
		else
			protect = false;

		if (vte->csi_argv[0] <= 0)
			tsm_screen_erase_cursor_to_screen(vte->con,
							      protect);
		else if (vte->csi_argv[0] == 1)
			tsm_screen_erase_screen_to_cursor(vte->con,
							      protect);
		else if (vte->csi_argv[0] == 2)
			tsm_screen_erase_screen(vte->con, protect);
		else
			llog_debug(vte, "unknown parameter to CSI-J: %d",
				   vte->csi_argv[0]);
		break;
	case 'K':
		if (vte->csi_flags & CSI_WHAT)
			protect = true;
		else
			protect = false;

		if (vte->csi_argv[0] <= 0)
			tsm_screen_erase_cursor_to_end(vte->con, protect);
		else if (vte->csi_argv[0] == 1)
			tsm_screen_erase_home_to_cursor(vte->con, protect);
		else if (vte->csi_argv[0] == 2)
			tsm_screen_erase_current_line(vte->con, protect);
		else
			llog_debug(vte, "unknown parameter to CSI-K: %d",
				   vte->csi_argv[0]);
		break;
	case 'X': /* ECH */
		/* erase characters */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_erase_chars(vte->con, num);
		break;
	case 'm':
		if (vte->csi_flags & CSI_GT) {
			/* xterm: set/reset key modifier options (XTMODKEYS) */
			/* ignore */
		} else {
			csi_attribute(vte);
		}
		break;
	case 'p':
		if (vte->csi_flags & CSI_GT) {
			/* xterm: select X11 visual cursor mode */
			csi_soft_reset(vte);
		} else if (vte->csi_flags & CSI_BANG) {
			/* DECSTR: Soft Reset */
			csi_soft_reset(vte);
		} else if (vte->csi_flags & CSI_CASH) {
			/* DECRQM: Request DEC Private Mode */
			/* If CSI_WHAT is set, then enable,
			 * otherwise disable */
			if (vte->csi_flags & CSI_WHAT)
			{
				/* ignore */
			} else {
				csi_soft_reset(vte);
			}
		} else {
			/* DECSCL: Compatibility Level */
			/* Sometimes CSI_DQUOTE is set here, too */
			csi_compat_mode(vte);
		}
		break;
	case 'h': /* SM: Set Mode */
		csi_mode(vte, true);
		break;
	case 'l': /* RM: Reset Mode */
		csi_mode(vte, false);
		break;
	case 'r': /* DECSTBM */
		/* set margin size */
		upper = vte->csi_argv[0];
		if (upper < 0)
			upper = 0;
		lower = vte->csi_argv[1];
		if (lower < 0)
			lower = 0;
		tsm_screen_set_margins(vte->con, upper, lower);
		break;
	case 'c': /* DA */
		/* device attributes */
		csi_dev_attr(vte);
		break;
	case 'L': /* IL */
		/* insert lines */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_insert_lines(vte->con, num);
		break;
	case 'M': /* DL */
		/* delete lines */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_delete_lines(vte->con, num);
		break;
	case 'g': /* TBC */
		/* tabulation clear */
		num = vte->csi_argv[0];
		if (num <= 0)
			tsm_screen_reset_tabstop(vte->con);
		else if (num == 3)
			tsm_screen_reset_all_tabstops(vte->con);
		else
			llog_debug(vte, "invalid parameter %d to TBC CSI", num);
		break;
	case '@': /* ICH */
		/* insert characters */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_insert_chars(vte->con, num);
		break;
	case 'P': /* DCH */
		/* delete characters */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_delete_chars(vte->con, num);
		break;
	case 'Z': /* CBT */
		/* cursor horizontal backwards tab */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_tab_left(vte->con, num);
		break;
	case 'I': /* CHT */
		/* cursor horizontal forward tab */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_tab_right(vte->con, num);
		break;
	case 'n': /* DSR */
		/* device status reports */
		csi_dsr(vte);
		break;
	case 'S': /* SU */
		/* scroll up */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_scroll_up(vte->con, num);
		break;
	case 'T': /* SD */
		/* scroll down */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_scroll_down(vte->con, num);
		break;
	default:
		llog_debug(vte, "unhandled CSI sequence %c", data);
	}
}

/* map a character according to current GL and GR maps */
static uint32_t vte_map(struct tsm_vte *vte, uint32_t val)
{
	/* 32, 127, 160 and 255 map to identity like all values >255 */
	switch (val) {
	// case 33 ... 126:
	case 33: case 34: case 35: case 36: case 37: case 38: case 39: case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 59: case 60: case 61: case 62: case 63: case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 72: case 73: case 74: case 75: case 76: case 77: case 78: case 79: case 80: case 81: case 82: case 83: case 84: case 85: case 86: case 87: case 88: case 89: case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97: case 98: case 99: case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107: case 108: case 109: case 110: case 111: case 112: case 113: case 114: case 115: case 116: case 117: case 118: case 119: case 120: case 121: case 122: case 123: case 124: case 125: case 126:
		if (vte->glt) {
			val = (**vte->glt)[val - 32];
			vte->glt = NULL;
		} else {
			val = (**vte->gl)[val - 32];
		}
		break;
	// case 161 ... 254:
	case 161: case 162: case 163: case 164: case 165: case 166: case 167: case 168: case 169: case 170: case 171: case 172: case 173: case 174: case 175: case 176: case 177: case 178: case 179: case 180: case 181: case 182: case 183: case 184: case 185: case 186: case 187: case 188: case 189: case 190: case 191: case 192: case 193: case 194: case 195: case 196: case 197: case 198: case 199: case 200: case 201: case 202: case 203: case 204: case 205: case 206: case 207: case 208: case 209: case 210: case 211: case 212: case 213: case 214: case 215: case 216: case 217: case 218: case 219: case 220: case 221: case 222: case 223: case 224: case 225: case 226: case 227: case 228: case 229: case 230: case 231: case 232: case 233: case 234: case 235: case 236: case 237: case 238: case 239: case 240: case 241: case 242: case 243: case 244: case 245: case 246: case 247: case 248: case 249: case 250: case 251: case 252: case 253: case 254:
		if (vte->grt) {
			val = (**vte->grt)[val - 160];
			vte->grt = NULL;
		} else {
			val = (**vte->gr)[val - 160];
		}
		break;
	}

	return val;
}

static void do_osc_collect(struct tsm_vte *vte, uint32_t val) {
	char buf[4];
	int len = tsm_ucs4_to_utf8(val, buf);
	if (vte->osc_len + len > sizeof(vte->osc_arg) - 1) {
		return;
	}

	memcpy(vte->osc_arg + vte->osc_len, buf, len);
	vte->osc_len += len;
}

static void do_osc_end(struct tsm_vte *vte) {
	if (!vte->osc_cb) {
		return;
	}

	vte->osc_arg[vte->osc_len] = 0;
	vte->osc_cb(vte, vte->osc_arg, vte->osc_len, vte->osc_data);
}

/* perform parser action */
static void do_action(struct tsm_vte *vte, uint32_t data, int action)
{
	tsm_symbol_t sym;

	switch (action) {
		case ACTION_NONE:
			/* do nothing */
			return;
		case ACTION_IGNORE:
			/* ignore character */
			break;
		case ACTION_PRINT:
			sym = tsm_symbol_make(vte_map(vte, data));
			write_console(vte, sym);
			break;
		case ACTION_EXECUTE:
			do_execute(vte, data);
			break;
		case ACTION_CLEAR:
			do_clear(vte);
			break;
		case ACTION_COLLECT:
			do_collect(vte, data);
			break;
		case ACTION_PARAM:
			do_param(vte, data);
			break;
		case ACTION_ESC_DISPATCH:
			do_esc(vte, data);
			break;
		case ACTION_CSI_DISPATCH:
			do_csi(vte, data);
			break;
		case ACTION_DCS_START:
			break;
		case ACTION_DCS_COLLECT:
			break;
		case ACTION_DCS_END:
			break;
		case ACTION_OSC_START:
			do_clear(vte);
			break;
		case ACTION_OSC_COLLECT:
			do_osc_collect(vte, data);
			break;
		case ACTION_OSC_END:
			do_osc_end(vte);
			break;
		default:
			llog_warning(vte, "invalid action %d", action);
	}
}

/* entry actions to be performed when entering the selected state */
static const int entry_action[] = {
	[STATE_CSI_ENTRY] = ACTION_CLEAR,
	[STATE_DCS_ENTRY] = ACTION_CLEAR,
	[STATE_DCS_PASS] = ACTION_DCS_START,
	[STATE_ESC] = ACTION_CLEAR,
	[STATE_OSC_STRING] = ACTION_OSC_START,
	[STATE_NUM] = ACTION_NONE,
};

/* exit actions to be performed when leaving the selected state */
static const int exit_action[] = {
	[STATE_DCS_PASS] = ACTION_DCS_END,
	[STATE_OSC_STRING] = ACTION_OSC_END,
	[STATE_NUM] = ACTION_NONE,
};

/* perform state transition and dispatch related actions */
static void do_trans(struct tsm_vte *vte, uint32_t data, int state, int act)
{
	if (state != STATE_NONE) {
		/* A state transition occurs. Perform exit-action,
		 * transition-action and entry-action. Even when performing a
		 * transition to the same state as the current state we do this.
		 * Use STATE_NONE if this is not the desired behavior.
		 */
		do_action(vte, data, exit_action[vte->state]);
		do_action(vte, data, act);
		do_action(vte, data, entry_action[state]);
		vte->state = state;
	} else {
		do_action(vte, data, act);
	}
}

/*
 * Escape sequence parser
 * This parses the new input character \data. It performs state transition and
 * calls the right callbacks for each action.
 */
static void parse_data(struct tsm_vte *vte, uint32_t raw)
{
	/* events that may occur in any state */
	switch (raw) {
		case 0x18:
		case 0x1a:
		// case 0x80 ... 0x8f:
		case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
		// case 0x91 ... 0x97:
		case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
		case 0x99:
		case 0x9a:
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_EXECUTE);
			return;
		case 0x1b:
			do_trans(vte, raw, STATE_ESC, ACTION_NONE);
			return;
		case 0x98:
		case 0x9e:
		case 0x9f:
			do_trans(vte, raw, STATE_ST_IGNORE, ACTION_NONE);
			return;
		case 0x90:
			do_trans(vte, raw, STATE_DCS_ENTRY, ACTION_NONE);
			return;
		case 0x9d:
			do_trans(vte, raw, STATE_OSC_STRING, ACTION_NONE);
			return;
		case 0x9b:
			do_trans(vte, raw, STATE_CSI_ENTRY, ACTION_NONE);
			return;
	}

	/* events that depend on the current state */
	switch (vte->state) {
	case STATE_GROUND:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		// case 0x80 ... 0x8f:
		case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
		// case 0x91 ... 0x9a:
		case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9A:
		case 0x9c:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		// case 0x20 ... 0x7f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
			do_trans(vte, raw, STATE_NONE, ACTION_PRINT);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_PRINT);
		return;
	case STATE_ESC:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		// case 0x20 ... 0x2f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			do_trans(vte, raw, STATE_ESC_INT, ACTION_COLLECT);
			return;
		// case 0x30 ... 0x4f:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
		// case 0x51 ... 0x57:
		case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
		case 0x59:
		case 0x5a:
		case 0x5c:
		// case 0x60 ... 0x7e:
		case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
			do_trans(vte, raw, STATE_GROUND, ACTION_ESC_DISPATCH);
			return;
		case 0x5b:
			do_trans(vte, raw, STATE_CSI_ENTRY, ACTION_NONE);
			return;
		case 0x5d:
			do_trans(vte, raw, STATE_OSC_STRING, ACTION_NONE);
			return;
		case 0x50:
			do_trans(vte, raw, STATE_DCS_ENTRY, ACTION_NONE);
			return;
		case 0x58:
		case 0x5e:
		case 0x5f:
			do_trans(vte, raw, STATE_ST_IGNORE, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_ESC_INT, ACTION_COLLECT);
		return;
	case STATE_ESC_INT:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		// case 0x20 ... 0x2f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		// case 0x30 ... 0x7e:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
			do_trans(vte, raw, STATE_GROUND, ACTION_ESC_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
		return;
	case STATE_CSI_ENTRY:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		// case 0x20 ... 0x2f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			do_trans(vte, raw, STATE_CSI_INT, ACTION_COLLECT);
			return;
		case 0x3a:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		// case 0x30 ... 0x39:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_CSI_PARAM, ACTION_PARAM);
			return;
		// case 0x3c ... 0x3f:
		case 0x3C: case 0x3D: case 0x3E: case 0x3F:
			do_trans(vte, raw, STATE_CSI_PARAM, ACTION_COLLECT);
			return;
		// case 0x40 ... 0x7e:
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_PARAM:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		// case 0x30 ... 0x39:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_NONE, ACTION_PARAM);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x3a:
		// case 0x3c ... 0x3f:
		case 0x3C: case 0x3D: case 0x3E: case 0x3F:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		// case 0x20 ... 0x2f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			do_trans(vte, raw, STATE_CSI_INT, ACTION_COLLECT);
			return;
		// case 0x40 ... 0x7e:
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_INT:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		// case 0x20 ... 0x2f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		// case 0x30 ... 0x3f:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		// case 0x40 ... 0x7e:
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_IGNORE:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		// case 0x20 ... 0x3f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		// case 0x40 ... 0x7e:
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	case STATE_DCS_ENTRY:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x3a:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		// case 0x20 ... 0x2f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			do_trans(vte, raw, STATE_DCS_INT, ACTION_COLLECT);
			return;
		// case 0x30 ... 0x39:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_DCS_PARAM, ACTION_PARAM);
			return;
		// case 0x3c ... 0x3f:
		case 0x3C: case 0x3D: case 0x3E: case 0x3F:
			do_trans(vte, raw, STATE_DCS_PARAM, ACTION_COLLECT);
			return;
		// case 0x40 ... 0x7e:
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_PARAM:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		// case 0x30 ... 0x39:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_NONE, ACTION_PARAM);
			return;
		case 0x3a:
		// case 0x3c ... 0x3f:
		case 0x3C: case 0x3D: case 0x3E: case 0x3F:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		// case 0x20 ... 0x2f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			do_trans(vte, raw, STATE_DCS_INT, ACTION_COLLECT);
			return;
		// case 0x40 ... 0x7e:
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_INT:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		// case 0x20 ... 0x2f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		// case 0x30 ... 0x3f:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		// case 0x40 ... 0x7e:
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_PASS:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		// case 0x20 ... 0x7e:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
			do_trans(vte, raw, STATE_NONE, ACTION_DCS_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_DCS_COLLECT);
		return;
	case STATE_DCS_IGNORE:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		// case 0x20 ... 0x7f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	case STATE_OSC_STRING:
		switch (raw) {
		// case 0x00 ... 0x06:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
		// case 0x08 ... 0x17:
		case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		// case 0x20 ... 0x7f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
			do_trans(vte, raw, STATE_NONE, ACTION_OSC_COLLECT);
			return;
		case 0x07:
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_OSC_COLLECT);
		return;
	case STATE_ST_IGNORE:
		switch (raw) {
		// case 0x00 ... 0x17:
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x19:
		// case 0x1c ... 0x1f:
		case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		// case 0x20 ... 0x7f:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	}

	llog_warning(vte, "unhandled input %u in state %d", raw, vte->state);
}

SHL_EXPORT
void tsm_vte_input(struct tsm_vte *vte, const char *u8, size_t len)
{
	int state;
	uint32_t ucs4;
	size_t i;

	if (!vte || !vte->con)
		return;

	++vte->parse_cnt;
	for (i = 0; i < len; ++i) {
		if (vte->flags & FLAG_7BIT_MODE) {
			if (u8[i] & 0x80)
				llog_debug(vte, "receiving 8bit character U+%d from pty while in 7bit mode",
					   (int)u8[i]);
			parse_data(vte, u8[i] & 0x7f);
		} else if (vte->flags & FLAG_8BIT_MODE) {
			parse_data(vte, u8[i]);
		} else {
			state = tsm_utf8_mach_feed(&vte->mach, u8[i]);
			if (state == TSM_UTF8_ACCEPT ||
			    state == TSM_UTF8_REJECT) {
				ucs4 = tsm_utf8_mach_get(&vte->mach);
				parse_data(vte, ucs4);
			}
		}
	}
	--vte->parse_cnt;
}

SHL_EXPORT
bool tsm_vte_handle_keyboard(struct tsm_vte *vte, uint32_t keysym,
			     uint32_t ascii, unsigned int mods,
			     uint32_t unicode)
{
	char val, u8[4];
	size_t len;
	uint32_t sym;

	if (!vte) {
		return false;
	}

	/* MOD1 (mostly labeled 'Alt') prepends an escape character to every
	 * input that is sent by a key.
	 * TODO: Transform this huge handler into a lookup table to save a lot
	 * of code and make such modifiers easier to implement.
	 * Also check whether altSendsEscape should be the default (xterm
	 * disables this by default, why?) and whether we should implement the
	 * fallback shifting that xterm does. */
	if (mods & TSM_ALT_MASK)
		vte->flags |= FLAG_PREPEND_ESCAPE;

	/* A user might actually use multiple layouts for keyboard input. The
	 * @keysym variable contains the actual keysym that the user used. But
	 * if this keysym is not in the ascii range, the input handler does
	 * check all other layouts that the user specified whether one of them
	 * maps the key to some ASCII keysym and provides this via @ascii.
	 * We always use the real keysym except when handling CTRL+<XY>
	 * shortcuts we use the ascii keysym. This is for compatibility to xterm
	 * et. al. so ctrl+c always works regardless of the currently active
	 * keyboard layout.
	 * But if no ascii-sym is found, we still use the real keysym. */
	sym = ascii;
	if (sym == XKB_KEY_NoSymbol)
		sym = keysym;

	if (mods & TSM_CONTROL_MASK) {
		switch (sym) {
		case XKB_KEY_2:
		case XKB_KEY_space:
			vte_write(vte, "\x00", 1);
			return true;
		case XKB_KEY_a:
		case XKB_KEY_A:
			vte_write(vte, "\x01", 1);
			return true;
		case XKB_KEY_b:
		case XKB_KEY_B:
			vte_write(vte, "\x02", 1);
			return true;
		case XKB_KEY_c:
		case XKB_KEY_C:
			vte_write(vte, "\x03", 1);
			return true;
		case XKB_KEY_d:
		case XKB_KEY_D:
			vte_write(vte, "\x04", 1);
			return true;
		case XKB_KEY_e:
		case XKB_KEY_E:
			vte_write(vte, "\x05", 1);
			return true;
		case XKB_KEY_f:
		case XKB_KEY_F:
			vte_write(vte, "\x06", 1);
			return true;
		case XKB_KEY_g:
		case XKB_KEY_G:
			vte_write(vte, "\x07", 1);
			return true;
		case XKB_KEY_h:
		case XKB_KEY_H:
			vte_write(vte, "\x08", 1);
			return true;
		case XKB_KEY_i:
		case XKB_KEY_I:
			vte_write(vte, "\x09", 1);
			return true;
		case XKB_KEY_j:
		case XKB_KEY_J:
			vte_write(vte, "\x0a", 1);
			return true;
		case XKB_KEY_k:
		case XKB_KEY_K:
			vte_write(vte, "\x0b", 1);
			return true;
		case XKB_KEY_l:
		case XKB_KEY_L:
			vte_write(vte, "\x0c", 1);
			return true;
		case XKB_KEY_m:
		case XKB_KEY_M:
			vte_write(vte, "\x0d", 1);
			return true;
		case XKB_KEY_n:
		case XKB_KEY_N:
			vte_write(vte, "\x0e", 1);
			return true;
		case XKB_KEY_o:
		case XKB_KEY_O:
			vte_write(vte, "\x0f", 1);
			return true;
		case XKB_KEY_p:
		case XKB_KEY_P:
			vte_write(vte, "\x10", 1);
			return true;
		case XKB_KEY_q:
		case XKB_KEY_Q:
			vte_write(vte, "\x11", 1);
			return true;
		case XKB_KEY_r:
		case XKB_KEY_R:
			vte_write(vte, "\x12", 1);
			return true;
		case XKB_KEY_s:
		case XKB_KEY_S:
			vte_write(vte, "\x13", 1);
			return true;
		case XKB_KEY_t:
		case XKB_KEY_T:
			vte_write(vte, "\x14", 1);
			return true;
		case XKB_KEY_u:
		case XKB_KEY_U:
			vte_write(vte, "\x15", 1);
			return true;
		case XKB_KEY_v:
		case XKB_KEY_V:
			vte_write(vte, "\x16", 1);
			return true;
		case XKB_KEY_w:
		case XKB_KEY_W:
			vte_write(vte, "\x17", 1);
			return true;
		case XKB_KEY_x:
		case XKB_KEY_X:
			vte_write(vte, "\x18", 1);
			return true;
		case XKB_KEY_y:
		case XKB_KEY_Y:
			vte_write(vte, "\x19", 1);
			return true;
		case XKB_KEY_z:
		case XKB_KEY_Z:
			vte_write(vte, "\x1a", 1);
			return true;
		case XKB_KEY_3:
		case XKB_KEY_bracketleft:
		case XKB_KEY_braceleft:
			vte_write(vte, "\x1b", 1);
			return true;
		case XKB_KEY_4:
		case XKB_KEY_backslash:
		case XKB_KEY_bar:
			vte_write(vte, "\x1c", 1);
			return true;
		case XKB_KEY_5:
		case XKB_KEY_bracketright:
		case XKB_KEY_braceright:
			vte_write(vte, "\x1d", 1);
			return true;
		case XKB_KEY_6:
		case XKB_KEY_grave:
		case XKB_KEY_asciitilde:
			vte_write(vte, "\x1e", 1);
			return true;
		case XKB_KEY_7:
		case XKB_KEY_slash:
		case XKB_KEY_question:
			vte_write(vte, "\x1f", 1);
			return true;
		case XKB_KEY_8:
			vte_write(vte, "\x7f", 1);
			return true;
		}
	}

	switch (keysym) {
		case XKB_KEY_BackSpace:
			vte_write(vte, "\x08", 1);
			return true;
		case XKB_KEY_Tab:
		case XKB_KEY_KP_Tab:
			vte_write(vte, "\x09", 1);
			return true;
		case XKB_KEY_ISO_Left_Tab:
			vte_write(vte, ESC "[Z", 3);
			return true;
		case XKB_KEY_Linefeed:
			vte_write(vte, "\x0a", 1);
			return true;
		case XKB_KEY_Clear:
			vte_write(vte, "\x0b", 1);
			return true;
		/*
		 TODO: What should we do with this key? Sending XOFF is awful as
		       there is no simple way on modern keyboards to send XON
		       again. If someone wants this, we can re-eanble it and set
		       some flag.
		case XKB_KEY_Pause:
			vte_write(vte, "\x13", 1);
			return true;
		*/
		/*
		 TODO: What should we do on scroll-lock? Sending 0x14 is what
		       the specs say but it is not used today the way most
		       users would expect so we disable it. If someone wants
		       this, we can re-enable it and set some flag.
		case XKB_KEY_Scroll_Lock:
			vte_write(vte, "\x14", 1);
			return true;
		*/
		case XKB_KEY_Sys_Req:
			vte_write(vte, "\x15", 1);
			return true;
		case XKB_KEY_Escape:
			vte_write(vte, "\x1b", 1);
			return true;
		case XKB_KEY_KP_Enter:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				vte_write(vte, ESC "OM", 3);
				return true;
			}
			/* fallthrough */
		case XKB_KEY_Return:
			if (vte->flags & FLAG_LINE_FEED_NEW_LINE_MODE)
				vte_write(vte, "\x0d\x0a", 2);
			else
				vte_write(vte, "\x0d", 1);
			return true;
		case XKB_KEY_Find:
			vte_write(vte, ESC "[1~", 4);
			return true;
		case XKB_KEY_Insert:
			vte_write(vte, ESC "[2~", 4);
			return true;
		case XKB_KEY_Delete:
			vte_write(vte, ESC "[3~", 4);
			return true;
		case XKB_KEY_Select:
			vte_write(vte, ESC "[4~", 4);
			return true;
		case XKB_KEY_Page_Up:
		case XKB_KEY_KP_Page_Up:
			vte_write(vte, ESC "[5~", 4);
			return true;
		case XKB_KEY_KP_Page_Down:
		case XKB_KEY_Page_Down:
			vte_write(vte, ESC "[6~", 4);
			return true;
		case XKB_KEY_Up:
		case XKB_KEY_KP_Up:
			if (mods & TSM_CONTROL_MASK) {
				vte_write(vte, ESC "[1;5A", 6);
			} else if (vte->flags & FLAG_CURSOR_KEY_MODE) {
				vte_write(vte, ESC "OA", 3);
			} else {
				vte_write(vte, ESC "[A", 3);
			}
			return true;
		case XKB_KEY_Down:
		case XKB_KEY_KP_Down:
			if (mods & TSM_CONTROL_MASK) {
				vte_write(vte, ESC "[1;5B", 6);
			} else if (vte->flags & FLAG_CURSOR_KEY_MODE) {
				vte_write(vte, ESC "OB", 3);
			} else {
				vte_write(vte, ESC "[B", 3);
			}
			return true;
		case XKB_KEY_Right:
		case XKB_KEY_KP_Right:
			if (mods & TSM_CONTROL_MASK) {
				vte_write(vte, ESC "[1;5C", 6);
			} else if (vte->flags & FLAG_CURSOR_KEY_MODE) {
				vte_write(vte, ESC "OC", 3);
			} else {
				vte_write(vte, ESC "[C", 3);
			}
			return true;
		case XKB_KEY_Left:
		case XKB_KEY_KP_Left:
			if (mods & TSM_CONTROL_MASK) {
				vte_write(vte, ESC "[1;5D", 6);
			} else if (vte->flags & FLAG_CURSOR_KEY_MODE) {
				vte_write(vte, ESC "OD", 3);
			} else {
				vte_write(vte, ESC "[D", 3);
			}
			return true;
		case XKB_KEY_KP_Insert:
		case XKB_KEY_KP_0:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Op", 3);
			else
				vte_write(vte, "0", 1);
			return true;
		case XKB_KEY_KP_1:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Oq", 3);
			else
				vte_write(vte, "1", 1);
			return true;
		case XKB_KEY_KP_2:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Or", 3);
			else
				vte_write(vte, "2", 1);
			return true;
		case XKB_KEY_KP_3:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Os", 3);
			else
				vte_write(vte, "3", 1);
			return true;
		case XKB_KEY_KP_4:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Ot", 3);
			else
				vte_write(vte, "4", 1);
			return true;
		case XKB_KEY_KP_5:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Ou", 3);
			else
				vte_write(vte, "5", 1);
			return true;
		case XKB_KEY_KP_6:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Ov", 3);
			else
				vte_write(vte, "6", 1);
			return true;
		case XKB_KEY_KP_7:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Ow", 3);
			else
				vte_write(vte, "7", 1);
			return true;
		case XKB_KEY_KP_8:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Ox", 3);
			else
				vte_write(vte, "8", 1);
			return true;
		case XKB_KEY_KP_9:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Oy", 3);
			else
				vte_write(vte, "9", 1);
			return true;
		case XKB_KEY_KP_Subtract:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Om", 3);
			else
				vte_write(vte, "-", 1);
			return true;
		case XKB_KEY_KP_Separator:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Ol", 3);
			else
				vte_write(vte, ",", 1);
			return true;
		case XKB_KEY_KP_Delete:
		case XKB_KEY_KP_Decimal:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "On", 3);
			else
				vte_write(vte, ".", 1);
			return true;
		case XKB_KEY_KP_Equal:
		case XKB_KEY_KP_Divide:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Oj", 3);
			else
				vte_write(vte, "/", 1);
			return true;
		case XKB_KEY_KP_Multiply:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Oo", 3);
			else
				vte_write(vte, "*", 1);
			return true;
		case XKB_KEY_KP_Add:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, ESC "Ok", 3);
			else
				vte_write(vte, "+", 1);
			return true;
		case XKB_KEY_Home:
		case XKB_KEY_KP_Home:
			if (mods & TSM_CONTROL_MASK)
				vte_write(vte, ESC "[1;5H", 6);
			else if (vte->flags & FLAG_CURSOR_KEY_MODE)
				vte_write(vte, ESC "OH", 3);
			else
				vte_write(vte, ESC "[H", 3);
			return true;
		case XKB_KEY_End:
		case XKB_KEY_KP_End:
			if (mods & TSM_CONTROL_MASK)
				vte_write(vte, ESC "[1;5F", 6);
			else if (vte->flags & FLAG_CURSOR_KEY_MODE)
				vte_write(vte, ESC "OF", 3);
			else
				vte_write(vte, ESC "[F", 3);
			return true;
		case XKB_KEY_KP_Space:
			vte_write(vte, " ", 1);
			return true;
		/* TODO: check what to transmit for functions keys when
		 * shift/ctrl etc. are pressed. Every terminal behaves
		 * differently here which is really weird.
		 * We now map F4 to F14 if shift is pressed and so on for all
		 * keys. However, such mappings should rather be done via
		 * xkb-configurations and we should instead add a flags argument
		 * to the CSIs as some of the keys here already do. */
		case XKB_KEY_F1:
		case XKB_KEY_KP_F1:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[23~", 5);
			else
				vte_write(vte, ESC "OP", 3);
			return true;
		case XKB_KEY_F2:
		case XKB_KEY_KP_F2:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[24~", 5);
			else
				vte_write(vte, ESC "OQ", 3);
			return true;
		case XKB_KEY_F3:
		case XKB_KEY_KP_F3:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[25~", 5);
			else
				vte_write(vte, ESC "OR", 3);
			return true;
		case XKB_KEY_F4:
		case XKB_KEY_KP_F4:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, ESC "[1;2S", 6);
				vte_write(vte, ESC "[26~", 5);
			else
				vte_write(vte, ESC "OS", 3);
			return true;
		case XKB_KEY_F5:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, ESC "[15;2~", 7);
				vte_write(vte, ESC "[28~", 5);
			else
				vte_write(vte, ESC "[15~", 5);
			return true;
		case XKB_KEY_F6:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, ESC "[17;2~", 7);
				vte_write(vte, ESC "[29~", 5);
			else
				vte_write(vte, ESC "[17~", 5);
			return true;
		case XKB_KEY_F7:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, ESC "[18;2~", 7);
				vte_write(vte, ESC "[31~", 5);
			else
				vte_write(vte, ESC "[18~", 5);
			return true;
		case XKB_KEY_F8:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, ESC "[19;2~", 7);
				vte_write(vte, ESC "[32~", 5);
			else
				vte_write(vte, ESC "[19~", 5);
			return true;
		case XKB_KEY_F9:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, ESC "[20;2~", 7);
				vte_write(vte, ESC "[33~", 5);
			else
				vte_write(vte, ESC "[20~", 5);
			return true;
		case XKB_KEY_F10:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, ESC "[21;2~", 7);
				vte_write(vte, ESC "[34~", 5);
			else
				vte_write(vte, ESC "[21~", 5);
			return true;
		case XKB_KEY_F11:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[23;2~", 7);
			else
				vte_write(vte, ESC "[23~", 5);
			return true;
		case XKB_KEY_F12:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[24;2~", 7);
			else
				vte_write(vte, ESC "[24~", 5);
			return true;
		case XKB_KEY_F13:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[25;2~", 7);
			else
				vte_write(vte, ESC "[25~", 5);
			return true;
		case XKB_KEY_F14:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[26;2~", 7);
			else
				vte_write(vte, ESC "[26~", 5);
			return true;
		case XKB_KEY_F15:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[28;2~", 7);
			else
				vte_write(vte, ESC "[28~", 5);
			return true;
		case XKB_KEY_F16:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[29;2~", 7);
			else
				vte_write(vte, ESC "[29~", 5);
			return true;
		case XKB_KEY_F17:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[31;2~", 7);
			else
				vte_write(vte, ESC "[31~", 5);
			return true;
		case XKB_KEY_F18:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[32;2~", 7);
			else
				vte_write(vte, ESC "[32~", 5);
			return true;
		case XKB_KEY_F19:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[33;2~", 7);
			else
				vte_write(vte, ESC "[33~", 5);
			return true;
		case XKB_KEY_F20:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, ESC "[34;2~", 7);
			else
				vte_write(vte, ESC "[34~", 5);
			return true;
	}

	if (unicode != TSM_VTE_INVALID) {
		if (vte->flags & FLAG_7BIT_MODE) {
			val = unicode;
			if (unicode & 0x80) {
				llog_debug(vte, "invalid keyboard input in 7bit mode U+%x; mapping to '?'",
					   unicode);
				val = '?';
			}
			vte_write(vte, &val, 1);
		} else if (vte->flags & FLAG_8BIT_MODE) {
			val = unicode;
			if (unicode > 0xff) {
				llog_debug(vte, "invalid keyboard input in 8bit mode U+%x; mapping to '?'",
					   unicode);
				val = '?';
			}
			vte_write_raw(vte, &val, 1);
		} else {
			len = tsm_ucs4_to_utf8(tsm_symbol_make(unicode), u8);
			vte_write_raw(vte, u8, len);
		}
		return true;
	}

	vte->flags &= ~FLAG_PREPEND_ESCAPE;
	return false;
}
