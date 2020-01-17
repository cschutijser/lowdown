/*	$Id$ */
/*
 * Copyright (c) 2020 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "config.h"

#if HAVE_SYS_QUEUE
# include <sys/queue.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <err.h> /* FIXME: debugging */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lowdown.h"
#include "extern.h"

#if 0
LOWDOWN_ROOT			-> done
LOWDOWN_BLOCKCODE		-> done
LOWDOWN_BLOCKQUOTE		-> done
LOWDOWN_HEADER			-> done
LOWDOWN_HRULE			-> done
LOWDOWN_LIST			-> done
LOWDOWN_LISTITEM		-> done
LOWDOWN_PARAGRAPH		-> done
LOWDOWN_TABLE_BLOCK		-> done
LOWDOWN_TABLE_HEADER		-> 
LOWDOWN_TABLE_BODY		-> 
LOWDOWN_TABLE_ROW		-> done
LOWDOWN_TABLE_CELL		-> 
LOWDOWN_FOOTNOTES_BLOCK		-> 
LOWDOWN_FOOTNOTE_DEF		-> 
LOWDOWN_BLOCKHTML		-> 
LOWDOWN_LINK_AUTO		-> done
LOWDOWN_CODESPAN		-> done
LOWDOWN_DOUBLE_EMPHASIS		-> done
LOWDOWN_EMPHASIS		-> done
LOWDOWN_HIGHLIGHT		-> 
LOWDOWN_IMAGE			-> 
LOWDOWN_LINEBREAK		-> done
LOWDOWN_LINK			-> done
LOWDOWN_TRIPLE_EMPHASIS		-> done
LOWDOWN_STRIKETHROUGH		-> done
LOWDOWN_SUPERSCRIPT		-> 
LOWDOWN_FOOTNOTE_REF		-> 
LOWDOWN_MATH_BLOCK		-> 
LOWDOWN_RAW_HTML		-> 
LOWDOWN_ENTITY			-> 
LOWDOWN_NORMAL_TEXT		-> done
LOWDOWN_DOC_HEADER		-> 
LOWDOWN_DOC_FOOTER		-> 
#endif

struct tstack {
	size_t	id; /* node identifier */
	size_t	lines; /* times emitted block prefix */
};

struct term {
	size_t		 col; /* output column from zero */
	ssize_t		 last_blank; /* line breaks or -1 (start) */
	struct tstack	 stack[128]; /* nodes being outputted */
	size_t		 stackpos; /* position in stack */
};

struct style {
	int	 italic;
	int	 strike;
	int	 bold;
	int	 under;
	size_t	 bcolour;
	size_t	 colour;
};

/*
 * Whether the style is not empty (i.e., has style attributes).
 */
#define	STYLE_NONEMPTY(_s) \
	((_s)->colour || (_s)->bold || (_s)->italic || \
	 (_s)->under || (_s)->strike || (_s)->bcolour)

static void
rndr_buf_style(struct hbuf *out, const struct style *s)
{

	if (!STYLE_NONEMPTY(s))
		return;
	HBUF_PUTSL(out, "\033[");
	if (s->bold) {
		HBUF_PUTSL(out, "1");
		if (s->under || s->colour || s->italic || s->strike ||
		    s->bcolour)
			HBUF_PUTSL(out, ";");
	}
	if (s->under) {
		HBUF_PUTSL(out, "4");
		if (s->colour || s->italic || s->strike || s->bcolour)
			HBUF_PUTSL(out, ";");
	}
	if (s->italic) {
		HBUF_PUTSL(out, "3");
		if (s->colour || s->strike || s->bcolour)
			HBUF_PUTSL(out, ";");
	}
	if (s->strike) {
		HBUF_PUTSL(out, "9");
		if (s->colour || s->bcolour)
			HBUF_PUTSL(out, ";");
	}
	if (s->bcolour) {
		hbuf_printf(out, "%zu", s->bcolour);
		if (s->colour)
			HBUF_PUTSL(out, ";");
	}
	if (s->colour)
		hbuf_printf(out, "%zu", s->colour);
	HBUF_PUTSL(out, "m");
}

/*
 * Set the style for the given node.
 */
static void
rndr_node_style(struct style *s, const struct lowdown_node *n)
{

	/* 
	 * Workaround: children of links don't inherit some of the
	 * values of their parent, specifically underlining.
	 */

	if (n->parent != NULL && n->parent->type == LOWDOWN_LINK) {
		s->colour = 92;
		s->bold = 1;
		s->under = 0;
	}

	switch (n->type) {
	case LOWDOWN_HRULE:
		s->colour = 37;
		break;
	case LOWDOWN_CODESPAN:
		s->bcolour = 47;
		s->colour = 31;
		break;
	case LOWDOWN_STRIKETHROUGH:
		s->strike = 1;
		break;
	case LOWDOWN_EMPHASIS:
		s->italic = 1;
		break;
	case LOWDOWN_DOUBLE_EMPHASIS:
		s->bold = 1;
		break;
	case LOWDOWN_TRIPLE_EMPHASIS:
		s->bold = s->italic = 1;
		break;
	case LOWDOWN_LINK:
	case LOWDOWN_AUTOLINK:
		s->colour = 32;
		s->under = 1;
		break;
	case LOWDOWN_HEADER:
		if (n->rndr_header.level > 1) {
			s->bold = 1;
			s->colour = 36;
		} else {
			s->bold = 1;
			s->colour = 37;
			s->bcolour = 104;
		}
		break;
	default:
		break;
	}
}

/*
 * Bookkeep that we've put "len" characters into the current line.
 */
static void
rndr_buf_advance(struct term *term, size_t len)
{

	term->col += len;
	if (term->col && term->last_blank != 0)
		term->last_blank = 0;
}

/*
 * Return non-zero if "n" or any of its ancestors require resetting the
 * output line mode, otherwise return zero.
 * This applies to both block and inline styles.
 */
static int
rndr_buf_endstyle(const struct lowdown_node *n)
{
	struct style	s;

	if (n->parent != NULL)
		if (rndr_buf_endstyle(n->parent))
			return 1;

	memset(&s, 0, sizeof(struct style));
	rndr_node_style(&s, n);
	return STYLE_NONEMPTY(&s);
}

/*
 * Unsets the current style context, if applies.
 */
static void
rndr_buf_endwords(struct term *term, hbuf *out,
	const struct lowdown_node *n)
{

	if (rndr_buf_endstyle(n))
		HBUF_PUTSL(out, "\033[0m");
}


/*
 * Like rndr_buf_endwords(), but also terminating the line itself.
 */
static void
rndr_buf_endline(struct term *term, hbuf *out,
	const struct lowdown_node *n)
{

	rndr_buf_endwords(term, out, n);
	assert(term->col > 0);
	assert(term->last_blank == 0);
	HBUF_PUTSL(out, "\n");
	term->col = 0;
	term->last_blank = 1;
}

/*
 * Output optional number of newlines before or after content.
 */
static void
rndr_buf_vspace(struct term *term, hbuf *out,
	const struct lowdown_node *n, size_t sz)
{

	if (term->last_blank == -1)
		return;
	while ((size_t)term->last_blank < sz) {
		HBUF_PUTSL(out, "\n");
		term->last_blank++;
	}
	term->col = 0;
}

/*
 * Output prefixes of the given node in the style further accumulated
 * from the parent nodes.
 */
static void
rndr_buf_startline_prefixes(struct term *term, struct style *s,
	const struct lowdown_node *n, hbuf *out)
{
	size_t	 i, emit;

	for (i = 0; i <= term->stackpos; i++)
		if (term->stack[i].id == n->id)
			break;
	assert(i <= term->stackpos);
	emit = term->stack[i].lines++;

	if (n->parent != NULL)
		rndr_buf_startline_prefixes(term, s, n->parent, out);

	rndr_node_style(s, n);

	switch (n->type) {
	case LOWDOWN_BLOCKCODE:
		HBUF_PUTSL(out, "    ");
		rndr_buf_advance(term, 4);
		break;
	case LOWDOWN_ROOT:
		HBUF_PUTSL(out, " ");
		rndr_buf_advance(term, 1);
		break;
	case LOWDOWN_BLOCKQUOTE:
		HBUF_PUTSL(out, "| ");
		rndr_buf_advance(term, 2);
		break;
	case LOWDOWN_HEADER:
		if (n->rndr_header.level == 1)
			break;
		rndr_buf_style(out, s);
		for (i = 0; i < n->rndr_header.level; i++)
			HBUF_PUTSL(out, "#");
		HBUF_PUTSL(out, " ");
		rndr_buf_advance(term, i + 1);
		if (STYLE_NONEMPTY(s))
			HBUF_PUTSL(out, "\033[0m");
		break;
	case LOWDOWN_LISTITEM:
		if (n->parent == NULL || 
		    n->parent->rndr_list.flags == 0) {
			if (emit == 0)
				HBUF_PUTSL(out, "- ");
			else
				HBUF_PUTSL(out, "  ");
			rndr_buf_advance(term, 3);
			break;
		}
		if (emit == 0)
			hbuf_printf(out, "%4u. ", n->rndr_listitem.num);
		else
			HBUF_PUTSL(out, "      ");
		rndr_buf_advance(term, 6);
		break;
	default:
		break;
	}
}

/*
 * Like rndr_buf_startwords(), but at the start of a line.
 * This also outputs all line prefixes of the block context.
 */
static void
rndr_buf_startline(struct term *term,
	hbuf *out, const struct lowdown_node *n)
{
	struct style	 s;

	assert(term->last_blank);
	assert(term->col == 0);

	memset(&s, 0, sizeof(struct style));
	rndr_buf_startline_prefixes(term, &s, n, out);
	rndr_buf_style(out, &s);
}

/*
 * Ascend to the root of the parse tree from rndr_buf_startwords(),
 * accumulating styles as we do so.
 */
static void
rndr_buf_startwords_style(const struct lowdown_node *n, struct style *s)
{

	if (n->parent != NULL)
		rndr_buf_startwords_style(n->parent, s);
	rndr_node_style(s, n);
}

/*
 * Accumulate and output the style at the start of one or more words.
 * Should *not* be called on the start of a new line, which calls for
 * rndr_buf_startline().
 */
static void
rndr_buf_startwords(struct term *term, hbuf *out,
	const struct lowdown_node *n)
{
	struct style	 s;

	assert(!term->last_blank);
	assert(term->col > 0);

	memset(&s, 0, sizeof(struct style));
	rndr_buf_startwords_style(n, &s);
	rndr_buf_style(out, &s);
}

static void
rndr_buf_literal(struct term *term, hbuf *out, 
	const struct lowdown_node *n, const hbuf *in)
{
	size_t		 i = 0, len;
	const char	*start;

	while (i < in->size) {
		start = &in->data[i];
		while (i < in->size && in->data[i] != '\n')
			i++;
		len = &in->data[i] - start;
		i++;
		rndr_buf_startline(term, out, n);
		hbuf_put(out, start, len);
		rndr_buf_advance(term, len);
		rndr_buf_endline(term, out, n);
	}
}

/*
 * Emit text in "in" the current line with output "out".
 * Use "n" and its ancestry to determine our context.
 */
static void
rndr_buf(struct term *term, hbuf *out, 
	const struct lowdown_node *n, const hbuf *in,
	int leading_space)
{
	size_t	 	 i = 0, len;
	int 		 needspace, begin = 1, end = 0;
	const char	*start;
	const struct lowdown_node *nn;

	for (nn = n; nn != NULL; nn = nn->parent)
		if (nn->type == LOWDOWN_BLOCKCODE) {
			rndr_buf_literal(term, out, n, in);
			return;
		}

	/* 
	 * Start each word by seeing if it has leading space.
	 * Allow this to be overriden by "leading_space" once.
	 */

	while (i < in->size) {
		if (leading_space) {
			needspace = 1;
			leading_space = 0;
		} else
			needspace = isspace
				((unsigned char)in->data[i]);

		while (i < in->size && 
		       isspace((unsigned char)in->data[i]))
			i++;

		/* See how long it the coming word (may be 0). */

		start = &in->data[i];
		while (i < in->size &&
		       !isspace((unsigned char)in->data[i]))
			i++;
		len = &in->data[i] - start;

		/* 
		 * If we cross our maximum width, then break.
		 * This will also unset the current style.
		 */

		if (term->col && term->col + len > 72) {
			rndr_buf_endline(term, out, n);
			end = 0;
		}

		/*
		 * Either emit our new line prefix (only if we have a
		 * word that will follow!) or, if we need space, emit
		 * the spacing.  In the first case, or if we have
		 * following text and are starting this node, emit our
		 * current style.
		 */

		if (term->last_blank && len) {
			rndr_buf_startline(term, out, n);
			begin = 0;
			end = 1;
		} else if (!term->last_blank) {
			if (needspace) {
				HBUF_PUTSL(out, " ");
				rndr_buf_advance(term, 1);
			}
			if (begin && len) {
				rndr_buf_startwords(term, out, n);
				begin = 0;
				end = 1;
			}
		}

		/* Emit the word itself. */

		hbuf_put(out, start, len);
		rndr_buf_advance(term, len);
	}

	if (end) {
		assert(begin == 0);
		rndr_buf_endwords(term, out, n);
	}
}

void
lowdown_term_rndr(hbuf *ob, void *arg, struct lowdown_node *n)
{
	struct lowdown_node	*child;
	struct term		*p = arg;
	struct hbuf		*tmp;
	
	/* Current nodes we're servicing. */

	memset(&p->stack[p->stackpos], 0, sizeof(struct tstack));
	p->stack[p->stackpos].id = n->id;

	/* Start with stuff to do *before* descent. */

	switch (n->type) {
	case LOWDOWN_ROOT:
		p->last_blank = -1;
		break;
	case LOWDOWN_BLOCKCODE:
	case LOWDOWN_BLOCKQUOTE:
	case LOWDOWN_HEADER:
	case LOWDOWN_LIST:
	case LOWDOWN_PARAGRAPH:
	case LOWDOWN_TABLE_BLOCK:
		rndr_buf_vspace(p, ob, n, 2);
		break;
	case LOWDOWN_HRULE:
	case LOWDOWN_LINEBREAK:
	case LOWDOWN_LISTITEM:
	case LOWDOWN_TABLE_ROW:
		rndr_buf_vspace(p, ob, n, 1);
		break;
	default:
		break;
	}

	/* Descend into children. */

	TAILQ_FOREACH(child, &n->children, entries) {
		p->stackpos++;
		assert(p->stackpos < 128);
		lowdown_term_rndr(ob, p, child);
		p->stackpos--;
	}

	/* Process content. */

	switch (n->type) {
	case LOWDOWN_HRULE:
		tmp = hbuf_new(32);
		HBUF_PUTSL(tmp, "~~~~~~~~");
		rndr_buf(p, ob, n, tmp, 0);
		hbuf_free(tmp);
		break;
	case LOWDOWN_BLOCKCODE:
		rndr_buf(p, ob, n, &n->rndr_blockcode.text, 0);
		break;
	case LOWDOWN_CODESPAN:
		rndr_buf(p, ob, n, &n->rndr_codespan.text, 0);
		break;
	case LOWDOWN_LINK_AUTO:
		rndr_buf(p, ob, n, &n->rndr_autolink.link, 0);
		break;
	case LOWDOWN_LINK:
		rndr_buf(p, ob, n, &n->rndr_link.link, 1);
		break;
	case LOWDOWN_NORMAL_TEXT:
		rndr_buf(p, ob, n, &n->rndr_normal_text.text, 0);
		break;
	default:
		break;
	}

	/* Process trailing block spaces and so on. */

	switch (n->type) {
	case LOWDOWN_BLOCKCODE:
	case LOWDOWN_BLOCKQUOTE:
	case LOWDOWN_HEADER:
	case LOWDOWN_LIST:
	case LOWDOWN_PARAGRAPH:
	case LOWDOWN_TABLE_BLOCK:
		rndr_buf_vspace(p, ob, n, 2);
		break;
	case LOWDOWN_HRULE:
	case LOWDOWN_LISTITEM:
	case LOWDOWN_ROOT:
	case LOWDOWN_TABLE_ROW:
		rndr_buf_vspace(p, ob, n, 1);
		break;
	default:
		break;
	}

	/* Snip trailing newlines except for one. */

	if (n->type == LOWDOWN_ROOT) {
		while (ob->size && ob->data[ob->size - 1] == '\n')
			ob->size--;
		HBUF_PUTSL(ob, "\n");
	}
}

void *
lowdown_term_new(const struct lowdown_opts *opts)
{

	return xcalloc(1, sizeof(struct term));
}

void
lowdown_term_free(void *arg)
{

	free(arg);
}