/*
 * Copyright(c) 2012 Tim Ruehsen
 *
 * This file is part of libmget.
 *
 * Libmget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Libmget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libmget.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * xml parsing routines
 *
 * Changelog
 * 22.06.2012  Tim Ruehsen  created, but needs definitely a rewrite
 *
 * This derives from an old source code that I wrote in 2001.
 * It is short, fast and has a low memory print, BUT it is a hack.
 * It has to be replaced by e.g. libxml2 or something better.
 *
 * HTML parsing is (very) different from XML parsing, see here:
 * http://www.whatwg.org/specs/web-apps/current-work/multipage/parsing.html
 * It is a PITA and should be handled by a specialized, external library !
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif

#include <libmget.h>
#include "private.h"

typedef struct XML_CONTEXT XML_CONTEXT;

struct XML_CONTEXT {
	const char
		*buf, // pointer to original start of buffer (0-terminated)
		*p, // pointer next char in buffer
		*token; // token buffer
	int
		hints; // XML_HINT...
	size_t
		token_size, // size of token buffer
		token_len; // used bytes of token buffer (not counting terminating 0 byte)
	void
		*user_ctx; // user context (not needed if we were using nested functions)
	mget_xml_callback_t
		*callback;
};

// append a char to token buffer

static const char *getToken(XML_CONTEXT *context)
{
	int c;
	const char *p;

	// skip leading spaces
	while ((c = *context->p++) && isspace(c));
	if (!c) return NULL;

	context->token = context->p - 1;

//	info_printf("a c=%c\n", c);

	if (isalpha(c) || c == '_') {
		while ((c = *context->p++) && !isspace(c) && c != '>' && c != '=');
		if (!c) return NULL;
		context->p--;
		context->token_len = context->p - context->token;
		return context->token;
	}

	if (c == '/') {
		if (!(c = *context->p++)) return NULL;
		if (c == '>') {
			context->token_len = 2;
			return context->token;
		} else return NULL; // syntax error
	}

	if (c == '\"' || c == '\'') { // read in quoted value
		int quote = c;

		context->token = context->p;

		if (!(p = strchr(context->p, quote)))
			return NULL;
		context->p = p + 1;

		context->token_len = context->p - context->token - 1;
		return context->token;
	}

	if (c == '<') { // fetch specials, e.g. start of comments '<!--'
		if (!(c = *context->p++)) return NULL;
		if (c == '?' || c == '/') {
			context->token_len = 2;
			return context->token;
		}

		if (c == '!') {
			// left: <!--, <![CDATA[ and <!WHATEVER
			if (!(c = *context->p++)) return NULL;
			if (c == '-') {
				if (!(c = *context->p++)) return NULL;
				if (c == '-') {
					context->token_len = 4;
					return context->token;
				} else {
					context->p -= 2;
					context->token_len = 2;
					return context->token;
				}
			} else {
				context->p--;
				context->token_len = 2;
				return context->token;
			}
		} else {
			context->p--;
			context->token_len = 1;
			return context->token;
		}
	}

	if (c == '>' || c == '=') {
		context->token_len = 1;
		return context->token;
	}

	if (c == '-') { // fetch specials, e.g. end of comments '-->'
		if (!(c = *context->p++)) return NULL;
		if (c != '-') {
			context->p--;
			c = '-';
		} else {
			if (!(c = *context->p++)) return NULL;
			if (c != '>') {
				context->p -= 2;
				c = '-';
			} else {
				context->token_len = 3;
				return context->token;
			}
		}
	}

	if (c == '?') { // fetch specials, e.g. '?>'
		if (!(c = *context->p++)) return NULL;
		if (c != '>') {
			context->p--;
			// c = '?';
		} else {
			context->token_len = 2;
			return context->token;
		}
	}

	while ((c = *context->p++) && !isspace(c));

	if (c) {
		context->p--;
		context->token_len = context->p - context->token;
		return context->token;
	}

	return NULL;
}

static int getValue(XML_CONTEXT *context)
{
	int c;

	context->token_len = 0;
	context->token = context->p;

	// remove leading spaces
	while ((c = *context->p++) && isspace(c));
	if (!c) return EOF;

	if (c == '=') {
		if (!getToken(context))
			return EOF;
		else
			return 1; // token valid
	}

	// attribute without value
	context->p--;
	context->token = context->p;
	return 1;
}

// special HTML <script> content parsing
// see http://www.whatwg.org/specs/web-apps/current-work/multipage/scripting-1.html#the-script-element
// 4.3.1.2 Restrictions for contents of script elements

static const char *getScriptContent(XML_CONTEXT *context)
{
	int comment = 0, length_valid = 0;
	const char *p;

	for (p = context->token = context->p; *p; p++) {
		if (comment) {
			if (*p == '-' && !memcmp(p, "-->", 3)) {
				p += 3 - 1;
				comment = 0;
			}
		} else {
			if (*p == '<' && !memcmp(p, "<!--", 4)) {
				p += 4 - 1;
				comment = 1;
			} else if (*p == '<' && !memcmp(p, "</script", 8)) {
				context->token_len = p - context->token;
				length_valid = 1;
				for (p += 8; isspace(*p); p++);
				if (*p == '>') {
					p++;
					break; // found end of <script>
				}
			}
		}
	}
	context->p = p;

	if (!length_valid)
		context->token_len = p - context->token;

	if (!*p && !context->token_len)
		return NULL;

	if (context->callback)
		context->callback(context->user_ctx, XML_FLG_CONTENT | XML_FLG_END, "script", NULL, context->token, context->token_len, context->token - context->buf);

	return context->token;
}

static const char *getUnparsed(XML_CONTEXT *context, int flags, const char *end, size_t len, const char *directory)
{
	int c;

	if (len == 1) {
		for (context->token = context->p; (c = *context->p) && c != *end; context->p++);
	} else {
		for (context->token = context->p; (c = *context->p); context->p++) {
			if (c == *end && context->p[1] == end[1] && (len == 2 || context->p[2] == end[2])) {
				break;
			}
		}
	}

	context->token_len = context->p - context->token;
	if (c) context->p += len;

	if (!c && !context->token_len)
		return NULL;
/*
	if (context->token && context->token_len && context->hints & XML_HINT_REMOVE_EMPTY_CONTENT) {
		int notempty = 0;
		char *p;

		for (p = context->token; *p; p++) {
			if (!isspace(*p)) {
				notempty = 1;
				break;
			}
		}

		if (notempty) {
			if (context->callback)
				context->callback(context->user_ctx, flags, directory, NULL, context->token, context->token_len, context->token - context->buf);
		} else {
			// ignore empty content
			context->token_len = 0;
			context->token[0] = 0;
		}
	} else {
*/
	if (context->callback)
		context->callback(context->user_ctx, flags, directory, NULL, context->token, context->token_len, context->token - context->buf);

//	}

	return context->token;
}

static const char *getComment(XML_CONTEXT *context)
{
	return getUnparsed(context, XML_FLG_COMMENT, "-->", 3, NULL);
}

static const char *getProcessing(XML_CONTEXT *context)
{
	return getUnparsed(context, XML_FLG_PROCESSING, "?>", 2, NULL);
}

static const char *getSpecial(XML_CONTEXT *context)
{
	return getUnparsed(context, XML_FLG_SPECIAL, ">", 1, NULL);
}

static const char *getContent(XML_CONTEXT *context, const char *directory)
{
	int c;

	for (context->token = context->p; (c = *context->p) && c != '<'; context->p++);

	context->token_len = context->p - context->token;

	if (!c && !context->token_len)
		return NULL;

	// debug_printf("content=%.*s\n", (int)context->token_len, context->token);
	if (context->callback && context->token_len)
		context->callback(context->user_ctx, XML_FLG_CONTENT, directory, NULL, context->token, context->token_len, context->token - context->buf);

	return context->token;
}

static void parseXML(const char *dir, XML_CONTEXT *context)
{
	const char *tok;
	char directory[256] = "";
	size_t pos = 0;

	if (!(context->hints & XML_HINT_HTML)) {
		pos = strlcpy(directory, dir, sizeof(directory));
		if (pos >= sizeof(directory)) pos = sizeof(directory) - 1;
	}

	do {
		getContent(context, directory);
		if (context->token_len)
			debug_printf("%s=%.*s\n", directory, (int)context->token_len, context->token);

		if (!(tok = getToken(context))) return;
		// debug_printf("A Token '%.*s'\n", (int)context->token_len, context->token);

		if (context->token_len == 1 && *tok == '<') {
			// get element name and add it to directory
			int flags = XML_FLG_BEGIN;

			if (!(tok = getToken(context))) return;
			// debug_printf("A2 Token '%.*s'\n", (int)context->token_len, context->token);

			if (!(context->hints & XML_HINT_HTML)) {
				if (!pos || directory[pos - 1] != '/')
					snprintf(&directory[pos], sizeof(directory) - pos, "/%.*s", (int)context->token_len, tok);
				else
					snprintf(&directory[pos], sizeof(directory) - pos, "%.*s", (int)context->token_len, tok);
			} else {
				// snprintf(directory, sizeof(directory), "%.*s", (int)context->token_len, tok);
				if (context->token_len < sizeof(directory)) {
					strncpy(directory, tok, context->token_len);
					directory[context->token_len] = 0;
				} else {
					strncpy(directory, tok, sizeof(directory) - 1);
					directory[sizeof(directory) - 1] = 0;
				}
			}

			while ((tok = getToken(context))) {
				// debug_printf("C Token %.*s\n", (int)context->token_len, context->token);
				if (context->token_len == 2 && !strncmp(tok, "/>", 2)) {
					if (context->callback)
						context->callback(context->user_ctx, flags | XML_FLG_END, directory, NULL, NULL, 0, 0);
					break; // stay in this level
				} else if (context->token_len == 1 && *tok == '>') {
					if (context->callback)
						context->callback(context->user_ctx, flags | XML_FLG_CLOSE, directory, NULL, NULL, 0, 0);
					if (context->hints & XML_HINT_HTML) {
						if (!strcasecmp(directory, "script")) {
							// special HTML <script> content parsing
							// see http://www.whatwg.org/specs/web-apps/current-work/multipage/scripting-1.html#the-script-element
							// 4.3.1.2 Restrictions for contents of script elements
							debug_printf("*** need special <script> handling\n");
							getScriptContent(context);
							if (context->token_len)
								debug_printf("%s=%.*s\n", directory, (int)context->token_len, context->token);
						}
					} else
						parseXML(directory, context); // descend one level
					break;
				} else {
//					snprintf(attribute, sizeof(attribute), "%.*s", (int)context->token_len, tok);
					char attribute[context->token_len + 1];
					strncpy(attribute, tok, context->token_len);
					attribute[context->token_len] = 0;

					if (getValue(context) == 0) return;
					if (context->token_len) {
						debug_printf("%s/@%s=%.*s\n", directory, attribute, (int)context->token_len, context->token);
						if (context->callback)
							context->callback(context->user_ctx, flags | XML_FLG_ATTRIBUTE, directory, attribute, context->token, context->token_len, context->token - context->buf);
					} else {
						debug_printf("%s/@%s\n", directory, attribute);
						if (context->callback)
							context->callback(context->user_ctx, flags | XML_FLG_ATTRIBUTE, directory, attribute, NULL, 0, 0);
					}
					flags = 0;
				}
			}
			directory[pos] = 0;
		} else if (context->token_len == 2) {
			if (!strncmp(tok, "</", 2)) {
				// ascend one level
				// cleanup - get name and '>'
				if (!(tok = getToken(context))) return;
				// debug_printf("X Token %s\n",tok);
				if (context->callback) {
					if (!(context->hints & XML_HINT_HTML))
						context->callback(context->user_ctx, XML_FLG_END, directory, NULL, NULL, 0, 0);
					else {
						char tag[context->token_len + 1]; // we need to \0 terminate tok
						memcpy(tag, tok, context->token_len);
						tag[context->token_len] = 0;
						context->callback(context->user_ctx, XML_FLG_END, tag, NULL, NULL, 0, 0);
					}
				}
				if (!(tok = getToken(context))) return;
				// debug_printf("Y Token %s\n",tok);
				if (!(context->hints & XML_HINT_HTML))
					return;
				else
					continue;
			} else if (!strncmp(tok, "<?", 2)) { // special info - ignore
				getProcessing(context);
				debug_printf("%s=<?%.*s?>\n", directory, (int)context->token_len, context->token);
				continue;
			} else if (!strncmp(tok, "<!", 2)) {
				getSpecial(context);
				debug_printf("%s=<!%.*s>\n", directory, (int)context->token_len, context->token);
			}
		} else if (context->token_len == 4 && !strncmp(tok, "<!--", 4)) { // comment - ignore
			getComment(context);
			debug_printf("%s=<!--%.*s-->\n", directory, (int)context->token_len, context->token);
			continue;
		}
	} while (tok);
}

void mget_xml_parse_buffer(
	const char *buf,
	mget_xml_callback_t *callback,
	void *user_ctx,
	int hints)
{
	XML_CONTEXT context;

	context.token = NULL;
	context.token_size = 0;
	context.token_len = 0;
	context.buf = buf;
	context.p = buf;
	context.user_ctx = user_ctx;
	context.callback = callback;
	context.hints = hints;

	parseXML("/", &context);
}

void mget_html_parse_buffer(
	const char *buf,
	mget_xml_callback_t *callback,
	void *user_ctx,
	int hints)
{
	mget_xml_parse_buffer(buf, callback, user_ctx, hints | XML_HINT_HTML);
}

void mget_xml_parse_file(
	const char *fname,
	mget_xml_callback_t *callback,
	void *user_ctx,
	int hints)
{
	int fd;

	if (strcmp(fname,"-")) {
		if ((fd = open(fname, O_RDONLY)) != -1) {
			struct stat st;
			if (fstat(fd, &st) == 0) {
#ifdef HAVE_MMAP
				size_t nread = st.st_size;
				char *buf = mmap(NULL, nread, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
#else
				char *buf=xmalloc(st.st_size + 1);
				size_t nread=read(fd, buf, st.st_size);
#endif

				if (nread > 0) {
					buf[nread] = 0; // PROT_WRITE allows this write, MAP_PRIVATE prevents changes in underlying file system
					mget_xml_parse_buffer(buf, callback, user_ctx, hints);
				}

#ifdef HAVE_MMAP
				munmap(buf, nread);
#else
				xfree(buf);
#endif
			}
			close(fd);
		} else
			error_printf(_("Failed to open %s\n"), fname);
	} else {
		// read data from STDIN.
		// maybe should use yy_scan_bytes instead of buffering into memory.
		char tmp[4096];
		ssize_t nbytes;
		mget_buffer_t *buf = mget_buffer_alloc(4096);

		while ((nbytes = read(STDIN_FILENO, tmp, sizeof(tmp))) > 0) {
			mget_buffer_memcat(buf, tmp, nbytes);
		}

		if (buf->length)
			mget_xml_parse_buffer(buf->data, callback, user_ctx, hints);

		mget_buffer_free(&buf);
	}
}

void mget_html_parse_file(
	const char *fname,
	mget_xml_callback_t *callback,
	void *user_ctx,
	int hints)
{
	mget_xml_parse_file(fname, callback, user_ctx, hints | XML_HINT_HTML);
}
