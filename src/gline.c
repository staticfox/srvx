/* gline.c - Gline database
 * Copyright 2000-2004 srvx Development Team
 *
 * This file is part of srvx.
 *
 * srvx is free software; you can redistribute it and/or modify
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
 * along with srvx; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */

#include "heap.h"
#include "helpfile.h"
#include "log.h"
#include "saxdb.h"
#include "timeq.h"
#include "gline.h"

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#define KEY_REASON "reason"
#define KEY_EXPIRES "expires"
#define KEY_LASTMOD "lastmod"
#define KEY_ISSUER "issuer"
#define KEY_ISSUED "issued"
#define KEY_LIFETIME "lifetime"

static heap_t gline_heap; /* key: expiry time, data: struct gline_entry* */
static dict_t gline_dict; /* key: target, data: struct gline_entry* */

static int
gline_comparator(const void *a, const void *b)
{
    const struct gline *ga=a, *gb=b;
    return ga->lifetime - gb->lifetime;
}

static void
free_gline_from_dict(void *data)
{
    struct gline *ent = data;
    free(ent->issuer);
    free(ent->target);
    free(ent->reason);
    free(ent);
}

static void
free_gline(struct gline *ent)
{
    dict_remove(gline_dict, ent->target);
}

static int
gline_equal_p(UNUSED_ARG(void *key), void *data, void *extra)
{
    return data == extra;
}

static void
gline_expire(UNUSED_ARG(void *data))
{
    unsigned long stopped;
    void *wraa;

    stopped = 0;
    while (heap_size(gline_heap)) {
        heap_peek(gline_heap, 0, &wraa);
        stopped = ((struct gline*)wraa)->lifetime;
        if (stopped > now)
            break;
        heap_pop(gline_heap);
        free_gline(wraa);
    }
    if (heap_size(gline_heap))
        timeq_add(stopped, gline_expire, NULL);
}

int
gline_remove(const char *target, int announce)
{
    struct gline *gl;

    gl = dict_find(gline_dict, target, NULL);
    if (gl != NULL)
        gl->expires = now;
    if (announce)
        irc_ungline(target);
    return gl != NULL;
}

struct gline *
gline_add(const char *issuer, const char *target, unsigned long duration, const char *reason, unsigned long issued, unsigned long lastmod, unsigned long lifetime, int announce)
{
    struct gline *ent;
    struct gline *prev_first;
    void *argh;
    unsigned long expires;

    heap_peek(gline_heap, 0, &argh);
    prev_first = argh;
    expires = now + duration;
    if (lifetime < expires)
        lifetime = expires;
    ent = dict_find(gline_dict, target, NULL);
    if (ent) {
        heap_remove_pred(gline_heap, gline_equal_p, ent);
        if (ent->issued > lastmod)
            ent->issued = lastmod;
        if (ent->lastmod < lastmod) {
            ent->lastmod = lastmod;
	    ent->expires = expires;
	    if (strcmp(ent->reason, reason)) {
		free(ent->reason);
		ent->reason = strdup(reason);
	    }
	}
        if (ent->lifetime < lifetime)
            ent->lifetime = lifetime;
    } else {
        ent = malloc(sizeof(*ent));
        ent->issued = issued;
        ent->lastmod = lastmod;
        ent->issuer = strdup(issuer);
        ent->target = strdup(target);
        ent->expires = expires;
        ent->lifetime = lifetime;
        ent->reason = strdup(reason);
        dict_insert(gline_dict, ent->target, ent);
    }
    heap_insert(gline_heap, ent, ent);
    if (!prev_first || (ent->lifetime < prev_first->lifetime)) {
        timeq_del(0, gline_expire, 0, TIMEQ_IGNORE_WHEN|TIMEQ_IGNORE_DATA);
        timeq_add(ent->lifetime, gline_expire, 0);
    }
    if (announce)
        irc_gline(NULL, ent);
    return ent;
}

static char *
gline_alternate_target(const char *target)
{
    const char *hostname;

    /* If no host part, bail. */
    if (!(hostname = strchr(target, '@')))
        return NULL;
    /* If host part contains wildcards, bail. */
    if (hostname[strcspn(hostname, "*?/")])
        return NULL;
    /* Get parsed address and canonical name for host. */
#if 0
    irc_in_addr_t in; /* move this to the right place */
    if (irc_pton(&in, NULL, hostname+1)) {
        if (getnameinfo(/*TODO*/))
              return NULL;
    } else if (!getaddrinfo(/*TODO*/)) {
    } else return NULL;
#else
    return NULL;
#endif
}

struct gline *
gline_find(const char *target)
{
    struct gline *res;
    dict_iterator_t it;
    char *alt_target;

    res = dict_find(gline_dict, target, NULL);
    if (res)
        return res;
    /* Stock ircu requires BADCHANs to match exactly. */
    if ((target[0] == '#') || (target[0] == '&'))
        return NULL;
    else if (target[strcspn(target, "*?")]) {
        /* Wildcard: do an obnoxiously long search. */
        for (it = dict_first(gline_dict); it; it = iter_next(it)) {
            res = iter_data(it);
            if (match_ircglob(target, res->target))
                return res;
        }
    }
    /* See if we can resolve the hostname part of the mask. */
    if ((alt_target = gline_alternate_target(target))) {
        res = gline_find(alt_target);
        free(alt_target);
        return res;
    }
    return NULL;
}

static int
gline_refresh_helper(UNUSED_ARG(void *key), void *data, void *extra)
{
    struct gline *ge = data;
    irc_gline(extra, ge);
    return 0;
}

void
gline_refresh_server(struct server *srv)
{
    heap_remove_pred(gline_heap, gline_refresh_helper, srv);
}

void
gline_refresh_all(void)
{
    heap_remove_pred(gline_heap, gline_refresh_helper, 0);
}

unsigned int
gline_count(void)
{
    return dict_size(gline_dict);
}

static int
gline_add_record(const char *key, void *data, UNUSED_ARG(void *extra))
{
    struct record_data *rd = data;
    const char *issuer, *reason, *dstr;
    unsigned long issued, expiration, lastmod, lifetime;

    if (!(reason = database_get_data(rd->d.object, KEY_REASON, RECDB_QSTRING))) {
        log_module(MAIN_LOG, LOG_ERROR, "Missing reason for gline %s", key);
        return 0;
    }
    if (!(dstr = database_get_data(rd->d.object, KEY_EXPIRES, RECDB_QSTRING))) {
        log_module(MAIN_LOG, LOG_ERROR, "Missing expiration for gline %s", key);
        return 0;
    }
    expiration = strtoul(dstr, NULL, 0);
    dstr = database_get_data(rd->d.object, KEY_LIFETIME, RECDB_QSTRING);
    lifetime = dstr ? strtoul(dstr, NULL, 0) : expiration;
    dstr = database_get_data(rd->d.object, KEY_LASTMOD, RECDB_QSTRING);
    lastmod = dstr ? strtoul(dstr, NULL, 0) : 0;
    dstr = database_get_data(rd->d.object, KEY_ISSUED, RECDB_QSTRING);
    issued = dstr ? strtoul(dstr, NULL, 0) : now;
    if (!(issuer = database_get_data(rd->d.object, KEY_ISSUER, RECDB_QSTRING)))
        issuer = "<unknown>";
    if (lifetime > now)
        gline_add(issuer, key, expiration - now, reason, issued, lastmod, lifetime, 0);
    return 0;
}

static int
gline_saxdb_read(struct dict *db)
{
    return dict_foreach(db, gline_add_record, 0) != NULL;
}

static int
gline_write_entry(UNUSED_ARG(void *key), void *data, void *extra)
{
    struct gline *ent = data;
    struct saxdb_context *ctx = extra;

    saxdb_start_record(ctx, ent->target, 0);
    saxdb_write_int(ctx, KEY_EXPIRES, ent->expires);
    saxdb_write_int(ctx, KEY_ISSUED, ent->issued);
    saxdb_write_int(ctx, KEY_LIFETIME, ent->lifetime);
    if (ent->lastmod)
        saxdb_write_int(ctx, KEY_LASTMOD, ent->lastmod);
    saxdb_write_string(ctx, KEY_REASON, ent->reason);
    saxdb_write_string(ctx, KEY_ISSUER, ent->issuer);
    saxdb_end_record(ctx);
    return 0;
}

static int
gline_saxdb_write(struct saxdb_context *ctx)
{
    heap_remove_pred(gline_heap, gline_write_entry, ctx);
    return 0;
}

static void
gline_db_cleanup(void)
{
    heap_delete(gline_heap);
    dict_delete(gline_dict);
}

void
gline_init(void)
{
    gline_heap = heap_new(gline_comparator);
    gline_dict = dict_new();
    dict_set_free_data(gline_dict, free_gline_from_dict);
    saxdb_register("gline", gline_saxdb_read, gline_saxdb_write);
    reg_exit_func(gline_db_cleanup);
}

struct gline_discrim *
gline_discrim_create(struct userNode *user, struct userNode *src, unsigned int argc, char *argv[])
{
    unsigned int i;
    struct gline_discrim *discrim;

    discrim = calloc(1, sizeof(*discrim));
    discrim->limit = 50;
    discrim->max_issued = ULONG_MAX;
    discrim->max_lastmod = ULONG_MAX;
    discrim->max_lifetime = ULONG_MAX;

    for (i=0; i<argc; i++) {
        if (i + 2 > argc) {
            send_message(user, src, "MSG_MISSING_PARAMS", argv[i]);
            goto fail;
        } else if (!irccasecmp(argv[i], "mask") || !irccasecmp(argv[i], "host")) {
            if (!irccasecmp(argv[++i], "exact"))
                discrim->target_mask_type = EXACT;
            else if (!irccasecmp(argv[i], "subset"))
                discrim->target_mask_type = SUBSET;
            else if (!irccasecmp(argv[i], "superset"))
                discrim->target_mask_type = SUPERSET;
            else
                discrim->target_mask_type = SUBSET, i--;
            if (++i == argc) {
                send_message(user, src, "MSG_MISSING_PARAMS", argv[i-1]);
                goto fail;
            }
            if (!is_gline(argv[i]) && !IsChannelName(argv[i])) {
                send_message(user, src, "MSG_INVALID_GLINE", argv[i]);
                goto fail;
            }
            discrim->target_mask = argv[i];
            discrim->alt_target_mask = gline_alternate_target(discrim->target_mask);
        } else if (!irccasecmp(argv[i], "limit"))
            discrim->limit = strtoul(argv[++i], NULL, 0);
        else if (!irccasecmp(argv[i], "reason"))
            discrim->reason_mask = argv[++i];
        else if (!irccasecmp(argv[i], "issuer"))
            discrim->issuer_mask = argv[++i];
        else if (!irccasecmp(argv[i], "after"))
            discrim->min_expire = now + ParseInterval(argv[++i]);
        else if (!irccasecmp(argv[i], "before"))
            discrim->max_issued = now - ParseInterval(argv[++i]);
        else if (!irccasecmp(argv[i], "lastmod")) {
            const char *cmp = argv[++i];
            if (cmp[0] == '<') {
                if (cmp[1] == '=') {
                    discrim->min_lastmod = now - ParseInterval(cmp + 2);
                } else {
                    discrim->min_lastmod = now - (ParseInterval(cmp + 1) - 1);
                }
            } else if (cmp[0] == '>') {
                if (cmp[1] == '=') {
                    discrim->max_lastmod = now - ParseInterval(cmp + 2);
                } else {
                    discrim->max_lastmod = now - (ParseInterval(cmp + 1) - 1);
                }
            } else {
                discrim->min_lastmod = now - ParseInterval(cmp + 2);
            }
        } else if (!irccasecmp(argv[i], "lifetime")) {
            const char *cmp = argv[++i];
            if (cmp[0] == '<') {
                if (cmp[1] == '=') {
                    discrim->min_lifetime = now - ParseInterval(cmp + 2);
                } else {
                    discrim->min_lifetime = now - (ParseInterval(cmp + 1) - 1);
                }
            } else if (cmp[0] == '>') {
                if (cmp[1] == '=') {
                    discrim->max_lifetime = now - ParseInterval(cmp + 2);
                } else {
                    discrim->max_lifetime = now - (ParseInterval(cmp + 1) - 1);
                }
            } else {
                discrim->min_lifetime = now - ParseInterval(cmp + 2);
            }
        } else {
            send_message(user, src, "MSG_INVALID_CRITERIA", argv[i]);
            goto fail;
        }
    }
    return discrim;
  fail:
    free(discrim->alt_target_mask);
    free(discrim);
    return NULL;
}

struct gline_search {
    struct gline_discrim *discrim;
    gline_search_func func;
    void *data;
    unsigned int hits;
};

static int
gline_discrim_match(struct gline *gline, struct gline_discrim *discrim)
{
    if ((discrim->issuer_mask && !match_ircglob(gline->issuer, discrim->issuer_mask))
        || (discrim->reason_mask && !match_ircglob(gline->reason, discrim->reason_mask))
        || (discrim->target_mask
            && (((discrim->target_mask_type == SUBSET)
                 && !match_ircglobs(discrim->target_mask, gline->target)
                 && (!discrim->alt_target_mask
                     || !match_ircglobs(discrim->alt_target_mask, gline->target)))
                || ((discrim->target_mask_type == EXACT)
                    && irccasecmp(discrim->target_mask, gline->target)
                    && (!discrim->alt_target_mask
                        || !irccasecmp(discrim->alt_target_mask, gline->target)))
                || ((discrim->target_mask_type == SUPERSET)
                    && !match_ircglobs(gline->target, discrim->target_mask)
                    && (!discrim->alt_target_mask
                        || !match_ircglobs(discrim->alt_target_mask, gline->target)))))
        || (discrim->max_issued < gline->issued)
        || (discrim->min_expire > gline->expires)
        || (discrim->min_lastmod > gline->lastmod)
        || (discrim->max_lastmod < gline->lastmod)
        || (discrim->min_lifetime > gline->lifetime)
        || (discrim->max_lifetime < gline->lifetime)) {
        return 0;
    }
    return 1;
}

static int
gline_search_helper(UNUSED_ARG(void *key), void *data, void *extra)
{
    struct gline *gline = data;
    struct gline_search *search = extra;

    if (gline_discrim_match(gline, search->discrim)
        && (search->hits++ < search->discrim->limit)) {
        search->func(gline, search->data);
    }
    return 0;
}

unsigned int
gline_discrim_search(struct gline_discrim *discrim, gline_search_func gsf, void *data)
{
    struct gline_search search;
    search.discrim = discrim;
    search.func = gsf;
    search.data = data;
    search.hits = 0;
    heap_remove_pred(gline_heap, gline_search_helper, &search);
    return search.hits;
}
