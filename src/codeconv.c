/*
 * Sylpheed -- a GTK+ based, lightweight, and fast e-mail client
 * Copyright (C) 1999-2005 Hiroyuki Yamamoto
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "defs.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>

#if HAVE_LOCALE_H
#  include <locale.h>
#endif

#include <iconv.h>

#include "codeconv.h"
#include "unmime.h"
#include "base64.h"
#include "quoted-printable.h"
#include "utils.h"
#include "prefs_common.h"

typedef enum
{
	JIS_ASCII,
	JIS_KANJI,
	JIS_HWKANA,
	JIS_AUXKANJI
} JISState;

#define SUBST_CHAR	'_'
#define ESC		'\033'

#define iseuckanji(c) \
	(((c) & 0xff) >= 0xa1 && ((c) & 0xff) <= 0xfe)
#define iseuchwkana1(c) \
	(((c) & 0xff) == 0x8e)
#define iseuchwkana2(c) \
	(((c) & 0xff) >= 0xa1 && ((c) & 0xff) <= 0xdf)
#define iseucaux(c) \
	(((c) & 0xff) == 0x8f)
#define issjiskanji1(c) \
	((((c) & 0xff) >= 0x81 && ((c) & 0xff) <= 0x9f) || \
	 (((c) & 0xff) >= 0xe0 && ((c) & 0xff) <= 0xfc))
#define issjiskanji2(c) \
	((((c) & 0xff) >= 0x40 && ((c) & 0xff) <= 0x7e) || \
	 (((c) & 0xff) >= 0x80 && ((c) & 0xff) <= 0xfc))
#define issjishwkana(c) \
	(((c) & 0xff) >= 0xa1 && ((c) & 0xff) <= 0xdf)

#define K_IN()				\
	if (state != JIS_KANJI) {	\
		*out++ = ESC;		\
		*out++ = '$';		\
		*out++ = 'B';		\
		state = JIS_KANJI;	\
	}

#define K_OUT()				\
	if (state != JIS_ASCII) {	\
		*out++ = ESC;		\
		*out++ = '(';		\
		*out++ = 'B';		\
		state = JIS_ASCII;	\
	}

#define HW_IN()				\
	if (state != JIS_HWKANA) {	\
		*out++ = ESC;		\
		*out++ = '(';		\
		*out++ = 'I';		\
		state = JIS_HWKANA;	\
	}

#define AUX_IN()			\
	if (state != JIS_AUXKANJI) {	\
		*out++ = ESC;		\
		*out++ = '$';		\
		*out++ = '(';		\
		*out++ = 'D';		\
		state = JIS_AUXKANJI;	\
	}

static void conv_jistoeuc(gchar *outbuf, gint outlen, const gchar *inbuf);
static void conv_euctojis(gchar *outbuf, gint outlen, const gchar *inbuf);
static void conv_sjistoeuc(gchar *outbuf, gint outlen, const gchar *inbuf);

static void conv_jistoutf8(gchar *outbuf, gint outlen, const gchar *inbuf);
static void conv_sjistoutf8(gchar *outbuf, gint outlen, const gchar *inbuf);
static void conv_euctoutf8(gchar *outbuf, gint outlen, const gchar *inbuf);
static void conv_anytoutf8(gchar *outbuf, gint outlen, const gchar *inbuf);

static void conv_utf8toeuc(gchar *outbuf, gint outlen, const gchar *inbuf);
static void conv_utf8tojis(gchar *outbuf, gint outlen, const gchar *inbuf);

static void conv_unreadable_eucjp(gchar *str);
static void conv_unreadable_8bit(gchar *str);
static void conv_unreadable_latin(gchar *str);

static void conv_jistodisp(gchar *outbuf, gint outlen, const gchar *inbuf);
static void conv_sjistodisp(gchar *outbuf, gint outlen, const gchar *inbuf);
static void conv_euctodisp(gchar *outbuf, gint outlen, const gchar *inbuf);

static void conv_anytodisp(gchar *outbuf, gint outlen, const gchar *inbuf);
static void conv_ustodisp(gchar *outbuf, gint outlen, const gchar *inbuf);
static void conv_noconv(gchar *outbuf, gint outlen, const gchar *inbuf);

static void conv_jistoeuc(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	const guchar *in = inbuf;
	guchar *out = outbuf;
	JISState state = JIS_ASCII;

	while (*in != '\0') {
		if (*in == ESC) {
			in++;
			if (*in == '$') {
				if (*(in + 1) == '@' || *(in + 1) == 'B') {
					state = JIS_KANJI;
					in += 2;
				} else if (*(in + 1) == '(' &&
					   *(in + 2) == 'D') {
					state = JIS_AUXKANJI;
					in += 3;
				} else {
					/* unknown escape sequence */
					state = JIS_ASCII;
				}
			} else if (*in == '(') {
				if (*(in + 1) == 'B' || *(in + 1) == 'J') {
					state = JIS_ASCII;
					in += 2;
				} else if (*(in + 1) == 'I') {
					state = JIS_HWKANA;
					in += 2;
				} else {
					/* unknown escape sequence */
					state = JIS_ASCII;
				}
			} else {
				/* unknown escape sequence */
				state = JIS_ASCII;
			}
		} else if (*in == 0x0e) {
			state = JIS_HWKANA;
			in++;
		} else if (*in == 0x0f) {
			state = JIS_ASCII;
			in++;
		} else {
			switch (state) {
			case JIS_ASCII:
				*out++ = *in++;
				break;
			case JIS_KANJI:
				*out++ = *in++ | 0x80;
				if (*in == '\0') break;
				*out++ = *in++ | 0x80;
				break;
			case JIS_HWKANA:
				*out++ = 0x8e;
				*out++ = *in++ | 0x80;
				break;
			case JIS_AUXKANJI:
				*out++ = 0x8f;
				*out++ = *in++ | 0x80;
				if (*in == '\0') break;
				*out++ = *in++ | 0x80;
				break;
			}
		}
	}

	*out = '\0';
}

#define JIS_HWDAKUTEN		0x5e
#define JIS_HWHANDAKUTEN	0x5f

static gint conv_jis_hantozen(guchar *outbuf, guchar jis_code, guchar sound_sym)
{
	static guint16 h2z_tbl[] = {
		/* 0x20 - 0x2f */
		0x0000, 0x2123, 0x2156, 0x2157, 0x2122, 0x2126, 0x2572, 0x2521,
		0x2523, 0x2525, 0x2527, 0x2529, 0x2563, 0x2565, 0x2567, 0x2543,
		/* 0x30 - 0x3f */
		0x213c, 0x2522, 0x2524, 0x2526, 0x2528, 0x252a, 0x252b, 0x252d,
		0x252f, 0x2531, 0x2533, 0x2535, 0x2537, 0x2539, 0x253b, 0x253d,
		/* 0x40 - 0x4f */
		0x253f, 0x2541, 0x2544, 0x2546, 0x2548, 0x254a, 0x254b, 0x254c,
		0x254d, 0x254e, 0x254f, 0x2552, 0x2555, 0x2558, 0x255b, 0x255e,
		/* 0x50 - 0x5f */
		0x255f, 0x2560, 0x2561, 0x2562, 0x2564, 0x2566, 0x2568, 0x2569,
		0x256a, 0x256b, 0x256c, 0x256d, 0x256f, 0x2573, 0x212b, 0x212c
	};

	static guint16 dakuten_tbl[] = {
		/* 0x30 - 0x3f */
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x252c, 0x252e,
		0x2530, 0x2532, 0x2534, 0x2536, 0x2538, 0x253a, 0x253c, 0x253e,
		/* 0x40 - 0x4f */
		0x2540, 0x2542, 0x2545, 0x2547, 0x2549, 0x0000, 0x0000, 0x0000,
		0x0000, 0x0000, 0x2550, 0x2553, 0x2556, 0x2559, 0x255c, 0x0000
	};

	static guint16 handakuten_tbl[] = {
		/* 0x4a - 0x4e */
		0x2551, 0x2554, 0x2557, 0x255a, 0x255d
	};

	guint16 out_code;

	jis_code &= 0x7f;
	sound_sym &= 0x7f;

	if (jis_code < 0x21 || jis_code > 0x5f)
		return 0;

	if (sound_sym == JIS_HWDAKUTEN &&
	    jis_code >= 0x36 && jis_code <= 0x4e) {
		out_code = dakuten_tbl[jis_code - 0x30];
		if (out_code != 0) {
			*outbuf = out_code >> 8;
			*(outbuf + 1) = out_code & 0xff;
			return 2;
		}
	}

	if (sound_sym == JIS_HWHANDAKUTEN &&
	    jis_code >= 0x4a && jis_code <= 0x4e) {
		out_code = handakuten_tbl[jis_code - 0x4a];
		*outbuf = out_code >> 8;
		*(outbuf + 1) = out_code & 0xff;
		return 2;
	}

	out_code = h2z_tbl[jis_code - 0x20];
	*outbuf = out_code >> 8;
	*(outbuf + 1) = out_code & 0xff;
	return 1;
}

static void conv_euctojis(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	const guchar *in = inbuf;
	guchar *out = outbuf;
	JISState state = JIS_ASCII;

	while (*in != '\0') {
		if (isascii(*in)) {
			K_OUT();
			*out++ = *in++;
		} else if (iseuckanji(*in)) {
			if (iseuckanji(*(in + 1))) {
				K_IN();
				*out++ = *in++ & 0x7f;
				*out++ = *in++ & 0x7f;
			} else {
				K_OUT();
				*out++ = SUBST_CHAR;
				in++;
				if (*in != '\0' && !isascii(*in)) {
					*out++ = SUBST_CHAR;
					in++;
				}
			}
		} else if (iseuchwkana1(*in)) {
			if (iseuchwkana2(*(in + 1))) {
				if (prefs_common.allow_jisx0201_kana) {
					HW_IN();
					in++;
					*out++ = *in++ & 0x7f;
				} else {
					guchar jis_ch[2];
					gint len;

					if (iseuchwkana1(*(in + 2)) &&
					    iseuchwkana2(*(in + 3)))
						len = conv_jis_hantozen
							(jis_ch,
							 *(in + 1), *(in + 3));
					else
						len = conv_jis_hantozen
							(jis_ch,
							 *(in + 1), '\0');
					if (len == 0)
						in += 2;
					else {
						K_IN();
						in += len * 2;
						*out++ = jis_ch[0];
						*out++ = jis_ch[1];
					}
				}
			} else {
				K_OUT();
				in++;
				if (*in != '\0' && !isascii(*in)) {
					*out++ = SUBST_CHAR;
					in++;
				}
			}
		} else if (iseucaux(*in)) {
			in++;
			if (iseuckanji(*in) && iseuckanji(*(in + 1))) {
				AUX_IN();
				*out++ = *in++ & 0x7f;
				*out++ = *in++ & 0x7f;
			} else {
				K_OUT();
				if (*in != '\0' && !isascii(*in)) {
					*out++ = SUBST_CHAR;
					in++;
					if (*in != '\0' && !isascii(*in)) {
						*out++ = SUBST_CHAR;
						in++;
					}
				}
			}
		} else {
			K_OUT();
			*out++ = SUBST_CHAR;
			in++;
		}
	}

	K_OUT();
	*out = '\0';
}

static void conv_sjistoeuc(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	const guchar *in = inbuf;
	guchar *out = outbuf;

	while (*in != '\0') {
		if (isascii(*in)) {
			*out++ = *in++;
		} else if (issjiskanji1(*in)) {
			if (issjiskanji2(*(in + 1))) {
				guchar out1 = *in;
				guchar out2 = *(in + 1);
				guchar row;

				row = out1 < 0xa0 ? 0x70 : 0xb0;
				if (out2 < 0x9f) {
					out1 = (out1 - row) * 2 - 1;
					out2 -= out2 > 0x7f ? 0x20 : 0x1f;
				} else {
					out1 = (out1 - row) * 2;
					out2 -= 0x7e;
				}

				*out++ = out1 | 0x80;
				*out++ = out2 | 0x80;
				in += 2;
			} else {
				*out++ = SUBST_CHAR;
				in++;
				if (*in != '\0' && !isascii(*in)) {
					*out++ = SUBST_CHAR;
					in++;
				}
			}
		} else if (issjishwkana(*in)) {
			*out++ = 0x8e;
			*out++ = *in++;
		} else {
			*out++ = SUBST_CHAR;
			in++;
		}
	}

	*out = '\0';
}

static void conv_jistoutf8(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	gchar *eucstr;

	Xalloca(eucstr, outlen, return);

	conv_jistoeuc(eucstr, outlen, inbuf);
	conv_euctoutf8(outbuf, outlen, eucstr);
}

static void conv_sjistoutf8(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	gchar *tmpstr;

	tmpstr = conv_iconv_strdup(inbuf, CS_SHIFT_JIS, CS_UTF_8);
	if (tmpstr) {
		strncpy2(outbuf, tmpstr, outlen);
		g_free(tmpstr);
	} else
		strncpy2(outbuf, inbuf, outlen);
}

static void conv_euctoutf8(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	static iconv_t cd = (iconv_t)-1;
	static gboolean iconv_ok = TRUE;
	gchar *tmpstr;

	if (cd == (iconv_t)-1) {
		if (!iconv_ok) {
			strncpy2(outbuf, inbuf, outlen);
			return;
		}
		cd = iconv_open(CS_UTF_8, CS_EUC_JP_MS);
		if (cd == (iconv_t)-1) {
			cd = iconv_open(CS_UTF_8, CS_EUC_JP);
			if (cd == (iconv_t)-1) {
				g_warning("conv_euctoutf8(): %s\n",
					  g_strerror(errno));
				iconv_ok = FALSE;
				strncpy2(outbuf, inbuf, outlen);
				return;
			}
		}
	}

	tmpstr = conv_iconv_strdup_with_cd(inbuf, cd);
	if (tmpstr) {
		strncpy2(outbuf, tmpstr, outlen);
		g_free(tmpstr);
	} else
		strncpy2(outbuf, inbuf, outlen);
}

static void conv_anytoutf8(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	switch (conv_guess_ja_encoding(inbuf)) {
	case C_ISO_2022_JP:
		conv_jistoutf8(outbuf, outlen, inbuf);
		break;
	case C_SHIFT_JIS:
		conv_sjistoutf8(outbuf, outlen, inbuf);
		break;
	case C_EUC_JP:
		conv_euctoutf8(outbuf, outlen, inbuf);
		break;
	default:
		strncpy2(outbuf, inbuf, outlen);
		break;
	}
}

static void conv_utf8toeuc(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	static iconv_t cd = (iconv_t)-1;
	static gboolean iconv_ok = TRUE;
	gchar *tmpstr;

	if (cd == (iconv_t)-1) {
		if (!iconv_ok) {
			strncpy2(outbuf, inbuf, outlen);
			return;
		}
		cd = iconv_open(CS_EUC_JP_MS, CS_UTF_8);
		if (cd == (iconv_t)-1) {
			cd = iconv_open(CS_EUC_JP, CS_UTF_8);
			if (cd == (iconv_t)-1) {
				g_warning("conv_utf8toeuc(): %s\n",
					  g_strerror(errno));
				iconv_ok = FALSE;
				strncpy2(outbuf, inbuf, outlen);
				return;
			}
		}
	}

	tmpstr = conv_iconv_strdup_with_cd(inbuf, cd);
	if (tmpstr) {
		strncpy2(outbuf, tmpstr, outlen);
		g_free(tmpstr);
	} else
		strncpy2(outbuf, inbuf, outlen);
}

static void conv_utf8tojis(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	gchar *eucstr;

	Xalloca(eucstr, outlen, return);

	conv_utf8toeuc(eucstr, outlen, inbuf);
	conv_euctojis(outbuf, outlen, eucstr);
}

static gchar valid_eucjp_tbl[][96] = {
	/* 0xa2a0 - 0xa2ff */
	{ 0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 0,
	  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 1, 1, 1, 1, 1, 1,
	  1, 1, 0, 0, 0, 0, 0, 0,  0, 0, 1, 1, 1, 1, 1, 1,
	  1, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 0, 0, 0, 0, 0,
	  0, 0, 1, 1, 1, 1, 1, 1,  1, 1, 0, 0, 0, 0, 1, 0 },

	/* 0xa3a0 - 0xa3ff */
	{ 0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 0, 0, 0, 0, 0, 0,
	  0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 0, 0, 0, 0, 0,
	  0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 0, 0, 0, 0, 0 },

	/* 0xa4a0 - 0xa4ff */
	{ 0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0 },

	/* 0xa5a0 - 0xa5ff */
	{ 0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 0,  0, 0, 0, 0, 0, 0, 0, 0 },

	/* 0xa6a0 - 0xa6ff */
	{ 0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 0, 0, 0, 0, 0, 0, 0,
	  0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 0, 0, 0, 0, 0, 0, 0,
	  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
	  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0 },

	/* 0xa7a0 - 0xa7ff */
	{ 0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
	  0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0 },

	/* 0xa8a0 - 0xa8ff */
	{ 0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,
	  1, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
	  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
	  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
	  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0 }
};

static gboolean isprintableeuckanji(guchar c1, guchar c2)
{
	if (c1 <= 0xa0 || c1 >= 0xf5)
		return FALSE;
	if (c2 <= 0xa0 || c2 == 0xff)
		return FALSE;

	if (c1 >= 0xa9 && c1 <= 0xaf)
		return FALSE;

	if (c1 >= 0xa2 && c1 <= 0xa8)
		return (gboolean)valid_eucjp_tbl[c1 - 0xa2][c2 - 0xa0];

	if (c1 == 0xcf) {
		if (c2 >= 0xd4 && c2 <= 0xfe)
			return FALSE;
	} else if (c1 == 0xf4) {
		if (c2 >= 0xa7 && c2 <= 0xfe)
			return FALSE;
	}

	return TRUE;
}

static void conv_unreadable_eucjp(gchar *str)
{
	register guchar *p = str;

	while (*p != '\0') {
		if (isascii(*p)) {
			/* convert CR+LF -> LF */
			if (*p == '\r' && *(p + 1) == '\n')
				memmove(p, p + 1, strlen(p));
			/* printable 7 bit code */
			p++;
		} else if (iseuckanji(*p)) {
			if (isprintableeuckanji(*p, *(p + 1))) {
				/* printable euc-jp code */
				p += 2;
			} else {
				/* substitute unprintable code */
				*p++ = SUBST_CHAR;
				if (*p != '\0') {
					if (isascii(*p))
						p++;
					else
						*p++ = SUBST_CHAR;
				}
			}
		} else if (iseuchwkana1(*p)) {
			if (iseuchwkana2(*(p + 1)))
				/* euc-jp hankaku kana */
				p += 2;
			else
				*p++ = SUBST_CHAR;
		} else if (iseucaux(*p)) {
			if (iseuckanji(*(p + 1)) && iseuckanji(*(p + 2))) {
				/* auxiliary kanji */
				p += 3;
			} else
				*p++ = SUBST_CHAR;
		} else
			/* substitute unprintable 1 byte code */
			*p++ = SUBST_CHAR;
	}
}

static void conv_unreadable_8bit(gchar *str)
{
	register guchar *p = str;

	while (*p != '\0') {
		/* convert CR+LF -> LF */
		if (*p == '\r' && *(p + 1) == '\n')
			memmove(p, p + 1, strlen(p));
		else if (!isascii(*p)) *p = SUBST_CHAR;
		p++;
	}
}

static void conv_unreadable_latin(gchar *str)
{
	register guchar *p = str;

	while (*p != '\0') {
		/* convert CR+LF -> LF */
		if (*p == '\r' && *(p + 1) == '\n')
			memmove(p, p + 1, strlen(p));
		else if ((*p & 0xff) >= 0x7f && (*p & 0xff) <= 0x9f)
			*p = SUBST_CHAR;
		p++;
	}
}

#define NCV	'\0'

void conv_mb_alnum(gchar *str)
{
	static guchar char_tbl[] = {
		/* 0xa0 - 0xaf */
		NCV, ' ', NCV, NCV, ',', '.', NCV, ':',
		';', '?', '!', NCV, NCV, NCV, NCV, NCV,
		/* 0xb0 - 0xbf */
		NCV, NCV, NCV, NCV, NCV, NCV, NCV, NCV,
		NCV, NCV, NCV, NCV, NCV, NCV, NCV, NCV,
		/* 0xc0 - 0xcf */
		NCV, NCV, NCV, NCV, NCV, NCV, NCV, NCV,
		NCV, NCV, '(', ')', NCV, NCV, '[', ']',
		/* 0xd0 - 0xdf */
		'{', '}', NCV, NCV, NCV, NCV, NCV, NCV,
		NCV, NCV, NCV, NCV, '+', '-', NCV, NCV,
		/* 0xe0 - 0xef */
		NCV, '=', NCV, '<', '>', NCV, NCV, NCV,
		NCV, NCV, NCV, NCV, NCV, NCV, NCV, NCV
	};

	register guchar *p = str;
	register gint len;

	len = strlen(str);

	while (len > 1) {
		if (*p == 0xa3) {
			register guchar ch = *(p + 1);

			if (ch >= 0xb0 && ch <= 0xfa) {
				/* [a-zA-Z] */
				*p = ch & 0x7f;
				p++;
				len--;
				memmove(p, p + 1, len);
				len--;
			} else  {
				p += 2;
				len -= 2;
			}
		} else if (*p == 0xa1) {
			register guchar ch = *(p + 1);

			if (ch >= 0xa0 && ch <= 0xef &&
			    NCV != char_tbl[ch - 0xa0]) {
				*p = char_tbl[ch - 0xa0];
				p++;
				len--;
				memmove(p, p + 1, len);
				len--;
			} else {
				p += 2;
				len -= 2;
			}
		} else if (iseuckanji(*p)) {
			p += 2;
			len -= 2;
		} else {
			p++;
			len--;
		}
	}
}

CharSet conv_guess_ja_encoding(const gchar *str)
{
	const guchar *p = str;
	CharSet guessed = C_US_ASCII;

	while (*p != '\0') {
		if (*p == ESC && (*(p + 1) == '$' || *(p + 1) == '(')) {
			if (guessed == C_US_ASCII)
				return C_ISO_2022_JP;
			p += 2;
		} else if (isascii(*p)) {
			p++;
		} else if (iseuckanji(*p) && iseuckanji(*(p + 1))) {
			if (*p >= 0xfd && *p <= 0xfe)
				return C_EUC_JP;
			else if (guessed == C_SHIFT_JIS) {
				if ((issjiskanji1(*p) &&
				     issjiskanji2(*(p + 1))) ||
				    issjishwkana(*p))
					guessed = C_SHIFT_JIS;
				else
					guessed = C_EUC_JP;
			} else
				guessed = C_EUC_JP;
			p += 2;
		} else if (issjiskanji1(*p) && issjiskanji2(*(p + 1))) {
			if (iseuchwkana1(*p) && iseuchwkana2(*(p + 1)))
				guessed = C_SHIFT_JIS;
			else
				return C_SHIFT_JIS;
			p += 2;
		} else if (issjishwkana(*p)) {
			guessed = C_SHIFT_JIS;
			p++;
		} else {
			p++;
		}
	}

	return guessed;
}

static void conv_jistodisp(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	conv_jistoutf8(outbuf, outlen, inbuf);
}

static void conv_sjistodisp(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	conv_sjistoutf8(outbuf, outlen, inbuf);
}

static void conv_euctodisp(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	conv_euctoutf8(outbuf, outlen, inbuf);
}

void conv_utf8todisp(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	if (g_utf8_validate(inbuf, -1, NULL) == TRUE)
		strncpy2(outbuf, inbuf, outlen);
	else
		conv_ustodisp(outbuf, outlen, inbuf);
}

static void conv_anytodisp(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	conv_anytoutf8(outbuf, outlen, inbuf);
	if (g_utf8_validate(outbuf, -1, NULL) != TRUE)
		conv_unreadable_8bit(outbuf);
}

static void conv_ustodisp(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	strncpy2(outbuf, inbuf, outlen);
	conv_unreadable_8bit(outbuf);
}

void conv_localetodisp(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	gchar *tmpstr;

	tmpstr = conv_iconv_strdup(inbuf, conv_get_locale_charset_str(),
				   CS_INTERNAL);
	if (tmpstr) {
		strncpy2(outbuf, tmpstr, outlen);
		g_free(tmpstr);
	} else
		conv_utf8todisp(outbuf, outlen, inbuf);
}

static void conv_noconv(gchar *outbuf, gint outlen, const gchar *inbuf)
{
	strncpy2(outbuf, inbuf, outlen);
}

CodeConverter *conv_code_converter_new(const gchar *src_encoding,
				       const gchar *dest_encoding)
{
	CodeConverter *conv;

	conv = g_new0(CodeConverter, 1);
	conv->code_conv_func =
		conv_get_code_conv_func(src_encoding, dest_encoding);
	conv->src_encoding = g_strdup(src_encoding);
	conv->dest_encoding = g_strdup(dest_encoding);

	return conv;
}

void conv_code_converter_destroy(CodeConverter *conv)
{
	g_free(conv->src_encoding);
	g_free(conv->dest_encoding);
	g_free(conv);
}

gint conv_convert(CodeConverter *conv, gchar *outbuf, gint outlen,
		  const gchar *inbuf)
{
	if (conv->code_conv_func != conv_noconv)
		conv->code_conv_func(outbuf, outlen, inbuf);
	else {
		gchar *str;

		str = conv_iconv_strdup
			(inbuf, conv->src_encoding, conv->dest_encoding);
		if (!str)
			return -1;
		else {
			strncpy2(outbuf, str, outlen);
			g_free(str);
		}
	}

	return 0;
}

gchar *conv_codeset_strdup(const gchar *inbuf,
			   const gchar *src_code, const gchar *dest_code)
{
	gchar *buf;
	size_t len;
	CodeConvFunc conv_func;

	conv_func = conv_get_code_conv_func(src_code, dest_code);
	if (conv_func != conv_noconv) {
		len = (strlen(inbuf) + 1) * 3;
		buf = g_malloc(len);
		if (!buf) return NULL;

		conv_func(buf, len, inbuf);
		return g_realloc(buf, strlen(buf) + 1);
	}

	return conv_iconv_strdup(inbuf, src_code, dest_code);
}

CodeConvFunc conv_get_code_conv_func(const gchar *src_charset_str,
				     const gchar *dest_charset_str)
{
	CodeConvFunc code_conv = conv_noconv;
	CharSet src_charset;
	CharSet dest_charset;

	if (!src_charset_str)
		src_charset = conv_get_locale_charset();
	else
		src_charset = conv_get_charset_from_str(src_charset_str);

	/* auto detection mode */
	if (!src_charset_str && !dest_charset_str) {
		if (src_charset == C_EUC_JP || src_charset == C_SHIFT_JIS)
			return conv_anytodisp;
		else
			return conv_noconv;
	}

	dest_charset = conv_get_charset_from_str(dest_charset_str);

	if (dest_charset == C_US_ASCII)
		return conv_ustodisp;

	switch (src_charset) {
	case C_US_ASCII:
	case C_ISO_8859_1:
	case C_ISO_8859_2:
	case C_ISO_8859_3:
	case C_ISO_8859_4:
	case C_ISO_8859_5:
	case C_ISO_8859_6:
	case C_ISO_8859_7:
	case C_ISO_8859_8:
	case C_ISO_8859_9:
	case C_ISO_8859_10:
	case C_ISO_8859_11:
	case C_ISO_8859_13:
	case C_ISO_8859_14:
	case C_ISO_8859_15:
		break;
	case C_ISO_2022_JP:
	case C_ISO_2022_JP_2:
	case C_ISO_2022_JP_3:
		if (dest_charset == C_AUTO)
			code_conv = conv_jistodisp;
		else if (dest_charset == C_EUC_JP)
			code_conv = conv_jistoeuc;
		else if (dest_charset == C_UTF_8)
			code_conv = conv_jistoutf8;
		break;
	case C_SHIFT_JIS:
		if (dest_charset == C_AUTO)
			code_conv = conv_sjistodisp;
		else if (dest_charset == C_EUC_JP)
			code_conv = conv_sjistoeuc;
		else if (dest_charset == C_UTF_8)
			code_conv = conv_sjistoutf8;
		break;
	case C_EUC_JP:
		if (dest_charset == C_AUTO)
			code_conv = conv_euctodisp;
		else if (dest_charset == C_ISO_2022_JP   ||
			 dest_charset == C_ISO_2022_JP_2 ||
			 dest_charset == C_ISO_2022_JP_3)
			code_conv = conv_euctojis;
		else if (dest_charset == C_UTF_8)
			code_conv = conv_euctoutf8;
		break;
	case C_UTF_8:
		if (dest_charset == C_EUC_JP)
			code_conv = conv_utf8toeuc;
		else if (dest_charset == C_ISO_2022_JP   ||
			 dest_charset == C_ISO_2022_JP_2 ||
			 dest_charset == C_ISO_2022_JP_3)
			code_conv = conv_utf8tojis;
		break;
	default:
		break;
	}

	return code_conv;
}

gchar *conv_iconv_strdup(const gchar *inbuf,
			 const gchar *src_code, const gchar *dest_code)
{
	iconv_t cd;
	gchar *outbuf;

	if (!src_code)
		src_code = conv_get_locale_charset_str();
	if (!dest_code)
		dest_code = CS_INTERNAL;

	cd = iconv_open(dest_code, src_code);
	if (cd == (iconv_t)-1)
		return NULL;

	outbuf = conv_iconv_strdup_with_cd(inbuf, cd);

	iconv_close(cd);

	return outbuf;
}

gchar *conv_iconv_strdup_with_cd(const gchar *inbuf, iconv_t cd)
{
	const gchar *inbuf_p;
	gchar *outbuf;
	gchar *outbuf_p;
	size_t in_size;
	size_t in_left;
	size_t out_size;
	size_t out_left;
	size_t n_conv;
	size_t len;

	inbuf_p = inbuf;
	in_size = strlen(inbuf);
	in_left = in_size;
	out_size = (in_size + 1) * 2;
	outbuf = g_malloc(out_size);
	outbuf_p = outbuf;
	out_left = out_size;

#define EXPAND_BUF()				\
{						\
	len = outbuf_p - outbuf;		\
	out_size *= 2;				\
	outbuf = g_realloc(outbuf, out_size);	\
	outbuf_p = outbuf + len;		\
	out_left = out_size - len;		\
}

	while ((n_conv = iconv(cd, (ICONV_CONST gchar **)&inbuf_p, &in_left,
			       &outbuf_p, &out_left)) == (size_t)-1) {
		if (EILSEQ == errno) {
			//g_print("iconv(): at %d: %s\n", in_size - in_left, g_strerror(errno));
			inbuf_p++;
			in_left--;
			if (out_left == 0) {
				EXPAND_BUF();
			}
			*outbuf_p++ = SUBST_CHAR;
			out_left--;
		} else if (EINVAL == errno) {
			break;
		} else if (E2BIG == errno) {
			EXPAND_BUF();
		} else {
			g_warning("conv_iconv_strdup(): %s\n",
				  g_strerror(errno));
			break;
		}
	}

	while ((n_conv = iconv(cd, NULL, NULL, &outbuf_p, &out_left)) ==
	       (size_t)-1) {
		if (E2BIG == errno) {
			EXPAND_BUF();
		} else {
			g_warning("conv_iconv_strdup(): %s\n",
				  g_strerror(errno));
			break;
		}
	}

#undef EXPAND_BUF

	len = outbuf_p - outbuf;
	outbuf = g_realloc(outbuf, len + 1);
	outbuf[len] = '\0';

	return outbuf;
}

static const struct {
	CharSet charset;
	gchar *const name;
} charsets[] = {
	{C_US_ASCII,		CS_US_ASCII},
	{C_US_ASCII,		CS_ANSI_X3_4_1968},
	{C_UTF_8,		CS_UTF_8},
	{C_UTF_7,		CS_UTF_7},
	{C_ISO_8859_1,		CS_ISO_8859_1},
	{C_ISO_8859_2,		CS_ISO_8859_2},
	{C_ISO_8859_3,		CS_ISO_8859_3},
	{C_ISO_8859_4,		CS_ISO_8859_4},
	{C_ISO_8859_5,		CS_ISO_8859_5},
	{C_ISO_8859_6,		CS_ISO_8859_6},
	{C_ISO_8859_7,		CS_ISO_8859_7},
	{C_ISO_8859_8,		CS_ISO_8859_8},
	{C_ISO_8859_9,		CS_ISO_8859_9},
	{C_ISO_8859_10,		CS_ISO_8859_10},
	{C_ISO_8859_11,		CS_ISO_8859_11},
	{C_ISO_8859_13,		CS_ISO_8859_13},
	{C_ISO_8859_14,		CS_ISO_8859_14},
	{C_ISO_8859_15,		CS_ISO_8859_15},
	{C_BALTIC,		CS_BALTIC},
	{C_CP1250,		CS_CP1250},
	{C_CP1251,		CS_CP1251},
	{C_CP1252,		CS_CP1252},
	{C_CP1253,		CS_CP1253},
	{C_CP1254,		CS_CP1254},
	{C_CP1255,		CS_CP1255},
	{C_CP1256,		CS_CP1256},
	{C_CP1257,		CS_CP1257},
	{C_CP1258,		CS_CP1258},
	{C_WINDOWS_1250,	CS_WINDOWS_1250},
	{C_WINDOWS_1251,	CS_WINDOWS_1251},
	{C_WINDOWS_1252,	CS_WINDOWS_1252},
	{C_WINDOWS_1253,	CS_WINDOWS_1253},
	{C_WINDOWS_1254,	CS_WINDOWS_1254},
	{C_WINDOWS_1255,	CS_WINDOWS_1255},
	{C_WINDOWS_1256,	CS_WINDOWS_1256},
	{C_WINDOWS_1257,	CS_WINDOWS_1257},
	{C_WINDOWS_1258,	CS_WINDOWS_1258},
	{C_KOI8_R,		CS_KOI8_R},
	{C_KOI8_T,		CS_KOI8_T},
	{C_KOI8_U,		CS_KOI8_U},
	{C_ISO_2022_JP,		CS_ISO_2022_JP},
	{C_ISO_2022_JP_2,	CS_ISO_2022_JP_2},
	{C_ISO_2022_JP_3,	CS_ISO_2022_JP_3},
	{C_EUC_JP,		CS_EUC_JP},
	{C_EUC_JP,		CS_EUCJP},
	{C_EUC_JP_MS,		CS_EUC_JP_MS},
	{C_SHIFT_JIS,		CS_SHIFT_JIS},
	{C_SHIFT_JIS,		CS_SHIFT__JIS},
	{C_SHIFT_JIS,		CS_SJIS},
	{C_ISO_2022_KR,		CS_ISO_2022_KR},
	{C_EUC_KR,		CS_EUC_KR},
	{C_ISO_2022_CN,		CS_ISO_2022_CN},
	{C_EUC_CN,		CS_EUC_CN},
	{C_GB2312,		CS_GB2312},
	{C_GBK,			CS_GBK},
	{C_EUC_TW,		CS_EUC_TW},
	{C_BIG5,		CS_BIG5},
	{C_BIG5_HKSCS,		CS_BIG5_HKSCS},
	{C_TIS_620,		CS_TIS_620},
	{C_WINDOWS_874,		CS_WINDOWS_874},
	{C_GEORGIAN_PS,		CS_GEORGIAN_PS},
	{C_TCVN5712_1,		CS_TCVN5712_1},
};

static const struct {
	gchar *const locale;
	CharSet charset;
	CharSet out_charset;
} locale_table[] = {
	{"ja_JP.eucJP"	, C_EUC_JP	, C_ISO_2022_JP},
	{"ja_JP.EUC-JP"	, C_EUC_JP	, C_ISO_2022_JP},
	{"ja_JP.EUC"	, C_EUC_JP	, C_ISO_2022_JP},
	{"ja_JP.ujis"	, C_EUC_JP	, C_ISO_2022_JP},
	{"ja_JP.SJIS"	, C_SHIFT_JIS	, C_ISO_2022_JP},
	{"ja_JP.JIS"	, C_ISO_2022_JP	, C_ISO_2022_JP},
	{"ja_JP"	, C_EUC_JP	, C_ISO_2022_JP},
	{"ko_KR.EUC-KR"	, C_EUC_KR	, C_EUC_KR},
	{"ko_KR"	, C_EUC_KR	, C_EUC_KR},
	{"zh_CN.GB2312"	, C_GB2312	, C_GB2312},
	{"zh_CN.GBK"	, C_GBK		, C_GB2312},
	{"zh_CN"	, C_GB2312	, C_GB2312},
	{"zh_HK"	, C_BIG5_HKSCS	, C_BIG5_HKSCS},
	{"zh_TW.eucTW"	, C_EUC_TW	, C_BIG5},
	{"zh_TW.EUC-TW"	, C_EUC_TW	, C_BIG5},
	{"zh_TW.Big5"	, C_BIG5	, C_BIG5},
	{"zh_TW"	, C_BIG5	, C_BIG5},

	{"ru_RU.KOI8-R"	, C_KOI8_R	, C_KOI8_R},
	{"ru_RU.KOI8R"	, C_KOI8_R	, C_KOI8_R},
	{"ru_RU.CP1251"	, C_WINDOWS_1251, C_KOI8_R},
	{"ru_RU"	, C_ISO_8859_5	, C_KOI8_R},
	{"tg_TJ"	, C_KOI8_T	, C_KOI8_T},
	{"ru_UA"	, C_KOI8_U	, C_KOI8_U},
	{"uk_UA.CP1251"	, C_WINDOWS_1251, C_KOI8_U},
	{"uk_UA"	, C_KOI8_U	, C_KOI8_U},

	{"be_BY"	, C_WINDOWS_1251, C_WINDOWS_1251},
	{"bg_BG"	, C_WINDOWS_1251, C_WINDOWS_1251},

	{"yi_US"	, C_WINDOWS_1255, C_WINDOWS_1255},

	{"af_ZA"	, C_ISO_8859_1  , C_ISO_8859_1},
	{"br_FR"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"ca_ES"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"da_DK"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"de_AT"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"de_BE"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"de_CH"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"de_DE"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"de_LU"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_AU"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_BW"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_CA"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_DK"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_GB"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_HK"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_IE"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_NZ"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_PH"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_SG"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_US"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_ZA"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"en_ZW"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_AR"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_BO"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_CL"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_CO"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_CR"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_DO"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_EC"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_ES"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_GT"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_HN"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_MX"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_NI"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_PA"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_PE"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_PR"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_PY"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_SV"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_US"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_UY"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"es_VE"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"et_EE"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"eu_ES"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"fi_FI"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"fo_FO"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"fr_BE"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"fr_CA"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"fr_CH"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"fr_FR"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"fr_LU"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"ga_IE"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"gl_ES"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"gv_GB"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"id_ID"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"is_IS"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"it_CH"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"it_IT"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"kl_GL"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"kw_GB"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"ms_MY"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"nl_BE"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"nl_NL"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"nn_NO"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"no_NO"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"oc_FR"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"pt_BR"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"pt_PT"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"sq_AL"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"sv_FI"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"sv_SE"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"tl_PH"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"uz_UZ"	, C_ISO_8859_1	, C_ISO_8859_1},
	{"wa_BE"	, C_ISO_8859_1	, C_ISO_8859_1},

	{"bs_BA"	, C_ISO_8859_2	, C_ISO_8859_2},
	{"cs_CZ"	, C_ISO_8859_2	, C_ISO_8859_2},
	{"hr_HR"	, C_ISO_8859_2	, C_ISO_8859_2},
	{"hu_HU"	, C_ISO_8859_2	, C_ISO_8859_2},
	{"pl_PL"	, C_ISO_8859_2	, C_ISO_8859_2},
	{"ro_RO"	, C_ISO_8859_2	, C_ISO_8859_2},
	{"sk_SK"	, C_ISO_8859_2	, C_ISO_8859_2},
	{"sl_SI"	, C_ISO_8859_2	, C_ISO_8859_2},

	{"sr_YU@cyrillic"	, C_ISO_8859_5	, C_ISO_8859_5},
	{"sr_YU"		, C_ISO_8859_2	, C_ISO_8859_2},

	{"mt_MT"		, C_ISO_8859_3	, C_ISO_8859_3},

	{"lt_LT.iso88594"	, C_ISO_8859_4	, C_ISO_8859_4},
	{"lt_LT.ISO8859-4"	, C_ISO_8859_4	, C_ISO_8859_4},
	{"lt_LT.ISO_8859-4"	, C_ISO_8859_4	, C_ISO_8859_4},
	{"lt_LT"		, C_ISO_8859_13	, C_ISO_8859_13},

	{"mk_MK"	, C_ISO_8859_5	, C_ISO_8859_5},

	{"ar_AE"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_BH"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_DZ"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_EG"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_IQ"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_JO"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_KW"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_LB"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_LY"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_MA"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_OM"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_QA"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_SA"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_SD"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_SY"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_TN"	, C_ISO_8859_6	, C_ISO_8859_6},
	{"ar_YE"	, C_ISO_8859_6	, C_ISO_8859_6},

	{"el_GR"	, C_ISO_8859_7	, C_ISO_8859_7},
	{"he_IL"	, C_ISO_8859_8	, C_ISO_8859_8},
	{"iw_IL"	, C_ISO_8859_8	, C_ISO_8859_8},
	{"tr_TR"	, C_ISO_8859_9	, C_ISO_8859_9},

	{"lv_LV"	, C_ISO_8859_13	, C_ISO_8859_13},
	{"mi_NZ"	, C_ISO_8859_13	, C_ISO_8859_13},

	{"cy_GB"	, C_ISO_8859_14	, C_ISO_8859_14},

	{"ar_IN"	, C_UTF_8	, C_UTF_8},
	{"en_IN"	, C_UTF_8	, C_UTF_8},
	{"se_NO"	, C_UTF_8	, C_UTF_8},
	{"ta_IN"	, C_UTF_8	, C_UTF_8},
	{"te_IN"	, C_UTF_8	, C_UTF_8},
	{"ur_PK"	, C_UTF_8	, C_UTF_8},

	{"th_TH"	, C_TIS_620	, C_TIS_620},
	/* {"th_TH"	, C_WINDOWS_874}, */
	/* {"th_TH"	, C_ISO_8859_11}, */

	{"ka_GE"	, C_GEORGIAN_PS	, C_GEORGIAN_PS},
	{"vi_VN.TCVN"	, C_TCVN5712_1	, C_TCVN5712_1},

	{"C"			, C_US_ASCII	, C_US_ASCII},
	{"POSIX"		, C_US_ASCII	, C_US_ASCII},
	{"ANSI_X3.4-1968"	, C_US_ASCII	, C_US_ASCII},
};

static GHashTable *conv_get_charset_to_str_table(void)
{
	static GHashTable *table;
	gint i;

	if (table)
		return table;

	table = g_hash_table_new(NULL, g_direct_equal);

	for (i = 0; i < sizeof(charsets) / sizeof(charsets[0]); i++) {
		if (g_hash_table_lookup(table, GUINT_TO_POINTER(charsets[i].charset))
		    == NULL) {
			g_hash_table_insert
				(table, GUINT_TO_POINTER(charsets[i].charset),
				 charsets[i].name);
		}
	}

	return table;
}

static GHashTable *conv_get_charset_from_str_table(void)
{
	static GHashTable *table;
	gint i;

	if (table)
		return table;

	table = g_hash_table_new(str_case_hash, str_case_equal);

	for (i = 0; i < sizeof(charsets) / sizeof(charsets[0]); i++) {
		g_hash_table_insert(table, charsets[i].name,
				    GUINT_TO_POINTER(charsets[i].charset));
	}

	return table;
}

const gchar *conv_get_charset_str(CharSet charset)
{
	GHashTable *table;

	table = conv_get_charset_to_str_table();
	return g_hash_table_lookup(table, GUINT_TO_POINTER(charset));
}

CharSet conv_get_charset_from_str(const gchar *charset)
{
	GHashTable *table;

	if (!charset) return C_AUTO;

	table = conv_get_charset_from_str_table();
	return GPOINTER_TO_UINT(g_hash_table_lookup(table, charset));
}

CharSet conv_get_locale_charset(void)
{
	static CharSet cur_charset = -1;
	const gchar *cur_locale;
	const gchar *p;
	gint i;

	if (cur_charset != -1)
		return cur_charset;

	cur_locale = conv_get_current_locale();
	if (!cur_locale) {
		cur_charset = C_US_ASCII;
		return cur_charset;
	}

	if (strcasestr(cur_locale, "UTF-8")) {
		cur_charset = C_UTF_8;
		return cur_charset;
	}

	if ((p = strcasestr(cur_locale, "@euro")) && p[5] == '\0') {
		cur_charset = C_ISO_8859_15;
		return cur_charset;
	}

	for (i = 0; i < sizeof(locale_table) / sizeof(locale_table[0]); i++) {
		const gchar *p;

		/* "ja_JP.EUC" matches with "ja_JP.eucJP", "ja_JP.EUC" and
		   "ja_JP". "ja_JP" matches with "ja_JP.xxxx" and "ja" */
		if (!strncasecmp(cur_locale, locale_table[i].locale,
				 strlen(locale_table[i].locale))) {
			cur_charset = locale_table[i].charset;
			return cur_charset;
		} else if ((p = strchr(locale_table[i].locale, '_')) &&
			 !strchr(p + 1, '.')) {
			if (strlen(cur_locale) == 2 &&
			    !strncasecmp(cur_locale, locale_table[i].locale, 2)) {
				cur_charset = locale_table[i].charset;
				return cur_charset;
			}
		}
	}

	cur_charset = C_AUTO;
	return cur_charset;
}

const gchar *conv_get_locale_charset_str(void)
{
	static const gchar *codeset = NULL;

	if (!codeset)
		codeset = conv_get_charset_str(conv_get_locale_charset());

	return codeset ? codeset : CS_INTERNAL;
}

CharSet conv_get_internal_charset(void)
{
	return C_INTERNAL;
}

const gchar *conv_get_internal_charset_str(void)
{
	return CS_INTERNAL;
}

CharSet conv_get_outgoing_charset(void)
{
	static CharSet out_charset = -1;
	const gchar *cur_locale;
	const gchar *p;
	gint i;

	if (out_charset != -1)
		return out_charset;

	cur_locale = conv_get_current_locale();
	if (!cur_locale) {
		out_charset = C_AUTO;
		return out_charset;
	}

	if ((p = strcasestr(cur_locale, "@euro")) && p[5] == '\0') {
		out_charset = C_ISO_8859_15;
		return out_charset;
	}

	for (i = 0; i < sizeof(locale_table) / sizeof(locale_table[0]); i++) {
		const gchar *p;

		if (!strncasecmp(cur_locale, locale_table[i].locale,
				 strlen(locale_table[i].locale))) {
			out_charset = locale_table[i].out_charset;
			break;
		} else if ((p = strchr(locale_table[i].locale, '_')) &&
			 !strchr(p + 1, '.')) {
			if (strlen(cur_locale) == 2 &&
			    !strncasecmp(cur_locale, locale_table[i].locale, 2)) {
				out_charset = locale_table[i].out_charset;
				break;
			}
		}
	}

	return out_charset;
}

const gchar *conv_get_outgoing_charset_str(void)
{
	CharSet out_charset;
	const gchar *str;

	if (prefs_common.outgoing_charset) {
		if (!isalpha((guchar)prefs_common.outgoing_charset[0])) {
			g_free(prefs_common.outgoing_charset);
			prefs_common.outgoing_charset = g_strdup(CS_AUTO);
		} else if (strcmp(prefs_common.outgoing_charset, CS_AUTO) != 0)
			return prefs_common.outgoing_charset;
	}

	out_charset = conv_get_outgoing_charset();
	str = conv_get_charset_str(out_charset);

	return str ? str : CS_UTF_8;
}

gboolean conv_is_multibyte_encoding(CharSet encoding)
{
	switch (encoding) {
	case C_EUC_JP:
	case C_EUC_JP_MS:
	case C_EUC_KR:
	case C_EUC_TW:
	case C_EUC_CN:
	case C_ISO_2022_JP:
	case C_ISO_2022_JP_2:
	case C_ISO_2022_JP_3:
	case C_ISO_2022_KR:
	case C_ISO_2022_CN:
	case C_SHIFT_JIS:
	case C_GB2312:
	case C_BIG5:
	case C_UTF_8:
	case C_UTF_7:
		return TRUE;
	default:
		return FALSE;
	}
}

const gchar *conv_get_current_locale(void)
{
	static const gchar *cur_locale;

	if (!cur_locale) {
		cur_locale = g_getenv("LC_ALL");
		if (!cur_locale) cur_locale = g_getenv("LC_CTYPE");
		if (!cur_locale) cur_locale = g_getenv("LANG");
		if (!cur_locale) cur_locale = setlocale(LC_CTYPE, NULL);

		debug_print("current locale: %s\n",
			    cur_locale ? cur_locale : "(none)");
	}

	return cur_locale;
}

gchar *conv_unmime_header(const gchar *str, const gchar *default_encoding)
{
	gchar buf[BUFFSIZE];

	if (is_ascii_str(str))
		return unmime_header(str);

	if (default_encoding) {
		gchar *utf8_buf;

		utf8_buf = conv_codeset_strdup
			(str, default_encoding, CS_INTERNAL);
		if (utf8_buf) {
			gchar *decoded_str;

			decoded_str = unmime_header(utf8_buf);
			g_free(utf8_buf);
			return decoded_str;
		}
	}

	if (conv_get_locale_charset() == C_EUC_JP)
		conv_anytodisp(buf, sizeof(buf), str);
	else
		conv_localetodisp(buf, sizeof(buf), str);

	return unmime_header(buf);
}

#define MAX_LINELEN		76
#define MAX_HARD_LINELEN	996
#define MIMESEP_BEGIN		"=?"
#define MIMESEP_END		"?="

#define B64LEN(len)	((len) / 3 * 4 + ((len) % 3 ? 4 : 0))

#define LBREAK_IF_REQUIRED(cond, is_plain_text)				\
{									\
	if (len - (destp - (guchar *)dest) < MAX_LINELEN + 2) {		\
		*destp = '\0';						\
		return;							\
	}								\
									\
	if ((cond) && *srcp) {						\
		if (destp > (guchar *)dest && left < MAX_LINELEN - 1) {	\
			if (isspace(*(destp - 1)))			\
				destp--;				\
			else if (is_plain_text && isspace(*srcp))	\
				srcp++;					\
			if (*srcp) {					\
				*destp++ = '\n';			\
				*destp++ = ' ';				\
				left = MAX_LINELEN - 1;			\
			}						\
		}							\
	}								\
}

void conv_encode_header(gchar *dest, gint len, const gchar *src,
			gint header_len, gboolean addr_field)
{
	const gchar *cur_encoding;
	const gchar *out_encoding;
	gint mimestr_len;
	gchar *mimesep_enc;
	gint left;
	const guchar *srcp = src;
	guchar *destp = dest;
	gboolean use_base64;

	g_return_if_fail(g_utf8_validate(src, -1, NULL) == TRUE);

	if (MB_CUR_MAX > 1) {
		use_base64 = TRUE;
		mimesep_enc = "?B?";
	} else {
		use_base64 = FALSE;
		mimesep_enc = "?Q?";
	}

	cur_encoding = CS_INTERNAL;
	out_encoding = conv_get_outgoing_charset_str();
	if (!strcmp(out_encoding, CS_US_ASCII))
		out_encoding = CS_ISO_8859_1;

	mimestr_len = strlen(MIMESEP_BEGIN) + strlen(out_encoding) +
		strlen(mimesep_enc) + strlen(MIMESEP_END);

	left = MAX_LINELEN - header_len;

	while (*srcp) {
		LBREAK_IF_REQUIRED(left <= 0, TRUE);

		while (isspace(*srcp)) {
			*destp++ = *srcp++;
			left--;
			LBREAK_IF_REQUIRED(left <= 0, TRUE);
		}

		/* output as it is if the next word is ASCII string */
		if (!is_next_nonascii(srcp)) {
			gint word_len;

			word_len = get_next_word_len(srcp);
			LBREAK_IF_REQUIRED(left < word_len, TRUE);
			while (word_len > 0) {
				LBREAK_IF_REQUIRED(left + (MAX_HARD_LINELEN - MAX_LINELEN) <= 0, TRUE)
				*destp++ = *srcp++;
				left--;
				word_len--;
			}

			continue;
		}

		/* don't include parentheses in encoded strings */
		if (addr_field && (*srcp == '(' || *srcp == ')')) {
			LBREAK_IF_REQUIRED(left < 2, FALSE);
			*destp++ = *srcp++;
			left--;
		}

		while (1) {
			gint mb_len = 0;
			gint cur_len = 0;
			gchar *part_str;
			gchar *out_str;
			gchar *enc_str;
			const guchar *p = srcp;
			gint out_str_len;
			gint out_enc_str_len;
			gint mime_block_len;
			gboolean cont = FALSE;

			while (*p != '\0') {
				if (isspace(*p) && !is_next_nonascii(p + 1))
					break;
				/* don't include parentheses in encoded
				   strings */
				if (addr_field && (*p == '(' || *p == ')'))
					break;

				mb_len = g_utf8_skip[*p];

				Xstrndup_a(part_str, srcp, cur_len + mb_len, );
				out_str = conv_codeset_strdup
					(part_str, cur_encoding, out_encoding);
				if (!out_str) {
					g_warning("conv_encode_header(): code conversion failed\n");
					conv_unreadable_8bit(part_str);
					out_str = g_strdup(part_str);
				}
				out_str_len = strlen(out_str);

				if (use_base64)
					out_enc_str_len = B64LEN(out_str_len);
				else
					out_enc_str_len =
						qp_get_q_encoding_len(out_str);

				g_free(out_str);

				if (mimestr_len + out_enc_str_len <= left) {
					cur_len += mb_len;
					p += mb_len;
				} else if (cur_len == 0) {
					LBREAK_IF_REQUIRED(1, FALSE);
					continue;
				} else {
					cont = TRUE;
					break;
				}
			}

			if (cur_len > 0) {
				Xstrndup_a(part_str, srcp, cur_len, );
				out_str = conv_codeset_strdup
					(part_str, cur_encoding, out_encoding);
				if (!out_str) {
					g_warning("conv_encode_header(): code conversion failed\n");
					conv_unreadable_8bit(part_str);
					out_str = g_strdup(part_str);
				}
				out_str_len = strlen(out_str);

				if (use_base64)
					out_enc_str_len = B64LEN(out_str_len);
				else
					out_enc_str_len =
						qp_get_q_encoding_len(out_str);

				Xalloca(enc_str, out_enc_str_len + 1, );
				if (use_base64)
					base64_encode(enc_str, out_str, out_str_len);
				else
					qp_q_encode(enc_str, out_str);

				g_free(out_str);

				/* output MIME-encoded string block */
				mime_block_len = mimestr_len + strlen(enc_str);
				g_snprintf(destp, mime_block_len + 1,
					   MIMESEP_BEGIN "%s%s%s" MIMESEP_END,
					   out_encoding, mimesep_enc, enc_str);
				destp += mime_block_len;
				srcp += cur_len;

				left -= mime_block_len;
			}

			LBREAK_IF_REQUIRED(cont, FALSE);

			if (cur_len == 0)
				break;
		}
	}

	*destp = '\0';
}

#undef LBREAK_IF_REQUIRED

gint conv_copy_file(const gchar *src, const gchar *dest, const gchar *encoding)
{
	FILE *src_fp, *dest_fp;
	gchar buf[BUFFSIZE], outbuf[BUFFSIZE];
	CodeConverter *conv;
	gboolean err = FALSE;

	if ((src_fp = fopen(src, "rb")) == NULL) {
		FILE_OP_ERROR(src, "fopen");
		return -1;
	}
	if ((dest_fp = fopen(dest, "wb")) == NULL) {
		FILE_OP_ERROR(dest, "fopen");
		fclose(src_fp);
		return -1;
	}

	if (change_file_mode_rw(dest_fp, dest) < 0) {
		FILE_OP_ERROR(dest, "chmod");
		g_warning("can't change file mode\n");
	}

	conv = conv_code_converter_new(encoding, NULL);

	while (fgets(buf, sizeof(buf), src_fp) != NULL) {
		if (conv_convert(conv, outbuf, sizeof(outbuf), buf) == 0)
			fputs(outbuf, dest_fp);
		else
			fputs(buf, dest_fp);
	}

	conv_code_converter_destroy(conv);

	if (ferror(src_fp)) {
		FILE_OP_ERROR(src, "fgets");
		err = TRUE;
	}
	fclose(src_fp);
	if (fclose(dest_fp) == EOF) {
		FILE_OP_ERROR(dest, "fclose");
		err = TRUE;
	}
	if (err) {
		unlink(dest);
		return -1;
	}

	return 0;
}

gint conv_copy_dir(const gchar *src, const gchar *dest, const gchar *encoding)
{
	DIR *dp;
	struct dirent *d;
	gchar *src_file;
	gchar *dest_file;

	if ((dp = opendir(src)) == NULL) {
		FILE_OP_ERROR(src, "opendir");
		return -1;
	}

	if (make_dir_hier(dest) < 0) {
		closedir(dp);
		return -1;
	}

	while ((d = readdir(dp)) != NULL) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

		src_file = g_strconcat(src, G_DIR_SEPARATOR_S, d->d_name, NULL);
		dest_file = g_strconcat(dest, G_DIR_SEPARATOR_S, d->d_name,
					NULL);
		if (is_file_exist(src_file))
			conv_copy_file(src_file, dest_file, encoding);
		g_free(dest_file);
		g_free(src_file);
	}

	closedir(dp);

	return 0;
}

gchar *conv_filename_from_utf8(const gchar *utf8_file)
{
	gchar *fs_file;
	GError *error = NULL;

	fs_file = g_filename_from_utf8(utf8_file, -1, NULL, NULL, &error);
	if (error) {
		g_warning("failed to convert encoding of file name: %s\n",
			  error->message);
		g_error_free(error);
	}
	if (!fs_file)
		fs_file = g_strdup(utf8_file);

	return fs_file;
}

gchar *conv_filename_to_utf8(const gchar *fs_file)
{
	gchar *utf8_file;
	GError *error = NULL;

	utf8_file = g_filename_to_utf8(fs_file, -1, NULL, NULL, &error);
	if (error) {
		g_warning("failed to convert encoding of file name: %s\n",
			  error->message);
		g_error_free(error);
	}
	if (!utf8_file)
		utf8_file = g_strdup(fs_file);

	return utf8_file;
}
