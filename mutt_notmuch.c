/*
 * Notmuch support for mutt
 *
 * Copyright (C) 2011 Karel Zak <kzak@redhat.com>
 */
#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "mx.h"
#include "rfc2047.h"
#include "sort.h"
#include "mailbox.h"
#include "copy.h"
#include "keymap.h"
#include "url.h"
#include "buffy.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <utime.h>

#include <notmuch.h>

#include "mutt_notmuch.h"
#include "mutt_curses.h"

/*
 * Parsed URI arguments
 */
struct uri_tag {
	char *name;
	char *value;
	struct uri_tag *next;
};

/*
 * HEADER->data
 */
struct nm_hdrdata {
	char *folder;
	char *tags;
	int magic;
};

/*
 * CONTEXT->data
 */
struct nm_data {
	notmuch_database_t *db;

	char *db_filename;
	char *db_query;
	int db_limit;

	struct uri_tag *query_items;
};


static void url_free_tags(struct uri_tag *tags)
{
	while (tags) {
		struct uri_tag *next = tags->next;
		FREE(&tags->name);
		FREE(&tags->value);
		FREE(&tags);
		tags = next;
	}
}

static int url_parse_query(char *url, char **filename, struct uri_tag **tags)
{
	char *p = strstr(url, "://");	/* remote unsupported */
	char *e;
	struct uri_tag *tag, *last = NULL;

	*filename = NULL;
	*tags = NULL;

	if (!p || !*(p + 3))
		return -1;

	p += 3;
	*filename = p;

	e = strchr(p, '?');

	*filename = e ? strndup(p, e - p) : safe_strdup(p);
	if (!*filename)
		return -1;

	if (!e)
		return 0;

	if (url_pct_decode(*filename) < 0)
		goto err;
	if (!e)
		return 0;	/* only filename */

	++e;	/* skip '?' */
	p = e;

	while (p && *p) {
		tag = safe_calloc(1, sizeof(struct uri_tag));
		if (!tag)
			goto err;

		if (!*tags)
			last = *tags = tag;
		else {
			last->next = tag;
			last = tag;
		}

		e = strchr(p, '=');
		if (!e)
			e = strchr(p, '&');
		tag->name = e ? strndup(p, e - p) : safe_strdup(p);
		if (!tag->name || url_pct_decode(tag->name) < 0)
			goto err;
		if (!e)
			break;

		p = e + 1;

		if (*e == '&')
			continue;

		e = strchr(p, '&');
		tag->value = e ? strndup(p, e - p) : safe_strdup(p);
		if (!tag->value || url_pct_decode(tag->value) < 0)
			goto err;
		if (!e)
			break;
		p = e + 1;
	}

	return 0;
err:
	FREE(&(*filename));
	url_free_tags(*tags);
	return -1;
}

static void free_hdrdata(HEADER *h)
{
	struct nm_hdrdata *data = h->data;

	if (data) {
		FREE(&data->folder);
		FREE(&data->tags);
		FREE(&data);
	}
	h->data = NULL;
}

static int free_data(CONTEXT *ctx)
{
	struct nm_data *data = ctx->data;
	int i;

	for (i = 0; i < ctx->msgcount; i++) {
		HEADER *h = ctx->hdrs[i];

		if (h)
			free_hdrdata(h);
	}

	if (data) {
		if (data->db)
			notmuch_database_close(data->db);
		FREE(&data->db_filename);
		FREE(&data->db_query);
		url_free_tags(data->query_items);
		FREE(&data);
	}

	ctx->data = NULL;
	return 0;
}

char *nm_header_get_folder(HEADER *h)
{
	return h && h->data ? ((struct nm_hdrdata *) h->data)->folder : NULL;
}

char *nm_header_get_tags(HEADER *h)
{
	return h && h->data ? ((struct nm_hdrdata *) h->data)->tags : NULL;
}

int nm_header_get_magic(HEADER *h)
{
	return h && h->data ? ((struct nm_hdrdata *) h->data)->magic : 0;
}

char *nm_header_get_fullpath(HEADER *h, char *buf, size_t bufsz)
{
	snprintf(buf, bufsz, "%s/%s", nm_header_get_folder(h), h->path);
	return buf;
}

static int init_data(CONTEXT *ctx)
{
	struct nm_data *data = ctx->data;

	if (data)
		return 0;

	data = safe_calloc(1, sizeof(struct nm_data));
	ctx->data = (void *) data;
	ctx->mx_close = free_data;

	if (url_parse_query(ctx->path, &data->db_filename, &data->query_items)) {
		mutt_error(_("failed to parse notmuch uri: %s"), ctx->path);
		data->db_filename = NULL;
		data->query_items = NULL;
		return -1;
	}

	return 0;
}

static struct nm_data *get_data(CONTEXT *ctx)
{
	return ctx->data;
}

static char *get_query(CONTEXT *ctx)
{
	struct nm_data *data = get_data(ctx);
	struct uri_tag *item;

	if (!data)
		return NULL;
	if (data->db_query)
		return data->db_query;

	for (item = data->query_items; item; item = item->next) {
		if (!item->value)
			continue;

		if (strcmp(item->name, "limit") == 0) {
			if (mutt_atoi(item->value, &data->db_limit))
				mutt_error (_("failed to parse notmuch limit: %s"), item->value);

		} else if (strcmp(item->name, "query") == 0)
			data->db_query = safe_strdup(item->value);
	}

	return data->db_query;
}

static int get_limit(CONTEXT *ctx)
{
	struct nm_data *data = get_data(ctx);
	return data->db_limit;
}

static notmuch_database_t *get_db(CONTEXT *ctx, int writable)
{
	struct nm_data *data = get_data(ctx);

	if (!data)
	       return NULL;
	if (!data->db) {
		data->db = notmuch_database_open(data->db_filename,
				writable ? NOTMUCH_DATABASE_MODE_READ_WRITE :
				NOTMUCH_DATABASE_MODE_READ_ONLY);
		if (!data->db)
			mutt_error (_("Cannot open notmuch database: %s"),
					data->db_filename);
	}

	return data->db;
}

static void release_db(CONTEXT *ctx)
{
	struct nm_data *data = get_data(ctx);

	if (data && data->db) {
		notmuch_database_close(data->db);
		data->db = NULL;
	}
}

static struct nm_hdrdata *create_hdrdata(HEADER *h, const char *path,
					      notmuch_message_t *msg)
{
	struct nm_hdrdata *data =
			safe_calloc(1, sizeof(struct nm_hdrdata));
	notmuch_tags_t *tags;
	char *tstr = NULL;
	size_t sz = 0;
	char *p;

	p = strrchr(path, '/');
	if (p && p - path > 3 &&
	    (strncmp(p - 3, "cur", 3) == 0 ||
	     strncmp(p - 3, "new", 3) == 0 ||
	     strncmp(p - 3, "tmp", 3) == 0)) {
			data->magic = M_MAILDIR;
			p -= 3;
			h->path = safe_strdup(p);
			data->folder = strndup(path, p - path - 1);
	}

	for (tags = notmuch_message_get_tags(msg);
	     tags && notmuch_tags_valid(tags);
	     notmuch_tags_move_to_next(tags)) {

		const char *t = notmuch_tags_get(tags);
		size_t xsz = t ? strlen(t) : 0;

		if (!xsz)
			continue;

		if (NotmuchHiddenTags) {
			p = strstr(NotmuchHiddenTags, t);

			if (p && (p == NotmuchHiddenTags
				  || *(p - 1) == ','
				  || *(p - 1) == ' ')
			    && (*(p + xsz) == '\0'
				  || *(p + xsz) == ','
				  || *(p + xsz) == ' '))
				continue;
		}

		safe_realloc(&tstr, sz + (sz ? 1 : 0) + xsz + 1);
		p = tstr + sz;
		if (sz) {
			*p++ = ' ';
			sz++;
		}
		memcpy(p, t, xsz + 1);
		sz += xsz;
	}

	data->tags = tstr;
	h->data = data;
	return data;
}

int nm_read_query(CONTEXT *ctx)
{
	notmuch_database_t *db;
	notmuch_query_t *q;
	notmuch_messages_t *msgs;
	int limit;

	if (!ctx->data && init_data(ctx))
		return -1;

	db = get_db(ctx, FALSE);
	if (!db)
		return -1;

	q = notmuch_query_create(db, get_query(ctx));
	notmuch_query_set_sort(q, NOTMUCH_SORT_NEWEST_FIRST);

	msgs = notmuch_query_search_messages(q);

	limit = get_limit(ctx);

	for (msgs = notmuch_query_search_messages(q);
	     notmuch_messages_valid(msgs) &&
		(limit == 0 || ctx->msgcount < limit);
	     notmuch_messages_move_to_next(msgs)) {

		notmuch_message_t *m = notmuch_messages_get(msgs);
		const char *path = notmuch_message_get_filename(m);
		HEADER *h;

		if (!path)
			continue;

		if (ctx->msgcount >= ctx->hdrmax)
			mx_alloc_memory(ctx);

		h = maildir_parse_message(M_MAILDIR, path, 0, NULL);
		if (!h)
			continue;

		create_hdrdata(h, path, m);

		ctx->size += h->content->length;
		ctx->hdrs[ctx->msgcount] = h;
		ctx->msgcount++;
	}

	notmuch_query_destroy(q);
	release_db(ctx);

	return 0;
}

char *nm_uri_from_query(CONTEXT *ctx, char *buf, size_t bufsz)
{
	struct nm_data *data;
	char uri[_POSIX_PATH_MAX];

	if (ctx && ctx->magic == M_NOTMUCH && (data = get_data(ctx)))
		snprintf(uri, sizeof(uri), "notmuch://%s?query=%s", data->db_filename, buf);
	else if (NotmuchDefaultUri)
		snprintf(uri, sizeof(uri), "%s?query=%s", NotmuchDefaultUri, buf);
	else
		return NULL;

	strncpy(buf, uri, bufsz);
	buf[bufsz - 1] = '\0';
	return buf;
}

static int _nm_update_filename(notmuch_database_t *db,
			const char *old, const char *new)
{
	notmuch_status_t st;

	if (!db)
		return -1;

	st = notmuch_database_add_message(db, new, NULL);
	if (st == NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID ||
	    st == NOTMUCH_STATUS_SUCCESS)
		notmuch_database_remove_message(db, old);

	return 0;
}

int nm_update_filename(CONTEXT *ctx, const char *old, const char *new)
{
	if (ctx->magic != M_NOTMUCH
	    || (!ctx->data && init_data(ctx))
	    || !new
	    || !old)
		return -1;

	return _nm_update_filename(get_db(ctx, TRUE), old, new);
}

int nm_sync(CONTEXT *ctx, int *index_hint)
{
	int i, rc = 0;
	char msgbuf[STRING];
	progress_t progress;
	char *uri = ctx->path;
	notmuch_database_t *db = NULL;

	if (!ctx->data && init_data(ctx))
		return -1;

	if (!ctx->quiet) {
		snprintf(msgbuf, sizeof (msgbuf), _("Writing %s..."), ctx->path);
		mutt_progress_init(&progress, msgbuf, M_PROGRESS_MSG,
				   WriteInc, ctx->msgcount);
	}

	for (i = 0; i < ctx->msgcount; i++) {
		char old[_POSIX_PATH_MAX], new[_POSIX_PATH_MAX];
		HEADER *h = ctx->hdrs[i];
		struct nm_hdrdata *data = h->data;

		if (!ctx->quiet)
			mutt_progress_update(&progress, i, -1);


		nm_header_get_fullpath(h, old, sizeof(old));

		ctx->path = data->folder;
		ctx->magic = data->magic;

#if USE_HCACHE
		rc = mh_sync_mailbox_message(ctx, i, NULL);
#else
		rc = mh_sync_mailbox_message(ctx, i);
#endif
		if (rc)
			break;

		nm_header_get_fullpath(h, new, sizeof(new));

		if (h->deleted || strcmp(old, new)) {
			/* email renamed or deleted -- update DB */
			if (!db)
				db = get_db(ctx, TRUE);

			if (h->deleted)
				notmuch_database_remove_message(db, old);
			else if (new && old)
				_nm_update_filename(db, old, new);
		}
	}

	ctx->path = uri;
	ctx->magic = M_NOTMUCH;
	release_db(ctx);

	return rc;
}

int nm_check_database(CONTEXT * ctx, int *index_hint)
{
	fprintf(stderr, "nm_check_database() not implemented yet");
	return 0;
}

static unsigned count_query(notmuch_database_t *db, const char *qstr)
{
	unsigned res = 0;
	notmuch_query_t *q = notmuch_query_create(db, qstr);
	if (q) {
		res = notmuch_query_count_messages(q);
		notmuch_query_destroy(q);
	}
	return res;
}

int nm_get_count(char *path, unsigned *all, unsigned *new)
{
	struct uri_tag *query_items = NULL, *item;
	char *db_filename = NULL, *db_query = NULL;
	notmuch_database_t *db = NULL;

	int rc = -1;

	if (url_parse_query(path, &db_filename, &query_items)) {
		mutt_error(_("failed to parse notmuch uri: %s"), path);
		goto done;
	}
	if (!db_filename || !query_items)
		goto done;

	for (item = query_items; item; item = item->next) {
		if (item->value && strcmp(item->name, "query") == 0) {
			db_query = item->value;
			break;
		}
	}

	if (!db_query)
		goto done;

	db = notmuch_database_open(db_filename,	NOTMUCH_DATABASE_MODE_READ_ONLY);
	if (!db) {
		mutt_error (_("Cannot open notmuch database: %s"), db_filename);
		goto done;
	}

	/* all emails */
	if (all)
		*all = count_query(db, db_query);

	/* new messages */
	if (new) {
		if (strstr(db_query, NotmuchUnreadTag))
			*new = all ? *all : count_query(db, db_query);
		else {
			size_t qsz = strlen(db_query)
					+ sizeof(" and ")
					+ strlen(NotmuchUnreadTag);
			char *qstr = safe_malloc(qsz + 10);

			if (!qstr)
				goto done;

			snprintf(qstr, qsz, "%s and %s", db_query, NotmuchUnreadTag);
			*new = count_query(db, qstr);
			FREE(&qstr);
		}
	}

	rc = 0;
done:
	if (db)
		notmuch_database_close(db);
	FREE(&db_filename);
	url_free_tags(query_items);
	return rc;
}