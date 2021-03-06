/*
 *  tvheadend, Automatic recordings
 *  Copyright (C) 2010 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <ctype.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "tvheadend.h"
#include "settings.h"
#include "dvr.h"
#include "epg.h"
#include "htsp_server.h"

static int dvr_autorec_in_init = 0;

struct dvr_autorec_entry_queue autorec_entries;

/**
 * Unlink - and remove any unstarted
 */
static void
dvr_autorec_purge_spawns(dvr_autorec_entry_t *dae, int del)
{
  dvr_entry_t *de;

  while((de = LIST_FIRST(&dae->dae_spawns)) != NULL) {
    LIST_REMOVE(de, de_autorec_link);
    de->de_autorec = NULL;
    if (!del) continue;
    if (de->de_sched_state == DVR_SCHEDULED)
      dvr_entry_cancel(de);
    else
      dvr_entry_save(de);
  }
}

/**
 * return 1 if the event 'e' is matched by the autorec rule 'dae'
 */
static int
autorec_cmp(dvr_autorec_entry_t *dae, epg_broadcast_t *e)
{
  channel_tag_mapping_t *ctm;
  dvr_config_t *cfg;
  double duration;

  if (!e->channel) return 0;
  if (!e->episode) return 0;
  if(dae->dae_enabled == 0 || dae->dae_weekdays == 0)
    return 0;

  if(dae->dae_channel == NULL &&
     dae->dae_channel_tag == NULL &&
     dae->dae_content_type == 0 &&
     (dae->dae_title == NULL ||
     dae->dae_title[0] == '\0') &&
     dae->dae_brand == NULL &&
     dae->dae_season == NULL &&
     dae->dae_minduration <= 0 &&
     (dae->dae_maxduration <= 0 || dae->dae_maxduration > 24 * 3600) &&
     dae->dae_serieslink == NULL)
    return 0; // Avoid super wildcard match

  // Note: we always test season first, though it will only be set
  //       if configured
  if(dae->dae_serieslink) {
    if (!e->serieslink || dae->dae_serieslink != e->serieslink) return 0;
    return 1;
  }
  if(dae->dae_season)
    if (!e->episode->season || dae->dae_season != e->episode->season) return 0;
  if(dae->dae_brand)
    if (!e->episode->brand || dae->dae_brand != e->episode->brand) return 0;
  if(dae->dae_title != NULL && dae->dae_title[0] != '\0') {
    lang_str_ele_t *ls;
    if(!e->episode->title) return 0;
    RB_FOREACH(ls, e->episode->title, link)
      if (!regexec(&dae->dae_title_preg, ls->str, 0, NULL, 0)) break;
    if (!ls) return 0;
  }

  // Note: ignore channel test if we allow quality unlocking 
  if ((cfg = dae->dae_config) == NULL)
    return 0;
  if (cfg->dvr_sl_quality_lock)
    if(dae->dae_channel != NULL &&
       dae->dae_channel != e->channel)
      return 0;

  if(dae->dae_channel_tag != NULL) {
    LIST_FOREACH(ctm, &dae->dae_channel_tag->ct_ctms, ctm_tag_link)
      if(ctm->ctm_channel == e->channel)
	break;
    if(ctm == NULL)
      return 0;
  }

  if(dae->dae_content_type != 0) {
    epg_genre_t ct;
    memset(&ct, 0, sizeof(ct));
    ct.code = dae->dae_content_type;
    if (!epg_genre_list_contains(&e->episode->genre, &ct, 1))
      return 0;
  }

  if(dae->dae_start >= 0 && dae->dae_start_window >= 0 &&
     dae->dae_start < 24*60 && dae->dae_start_window < 24*60) {
    struct tm a_time, ev_time;
    time_t ta, te, tad;
    localtime_r(&e->start, &a_time);
    ev_time = a_time;
    a_time.tm_min = dae->dae_start % 60;
    a_time.tm_hour = dae->dae_start / 60;
    ta = mktime(&a_time);
    te = mktime(&ev_time);
    if(dae->dae_start > dae->dae_start_window) {
      ta -= 24 * 3600; /* 24 hours */
      tad = ((24 * 60) - dae->dae_start + dae->dae_start_window) * 60;
      if(ta > te || te > ta + tad) {
        ta += 24 * 3600;
        if(ta > te || te > ta + tad)
          return 0;
      }
    } else {
      tad = (dae->dae_start_window - dae->dae_start) * 60;
      if(ta > te || te > ta + tad)
        return 0;
    }
  }

  duration = difftime(e->stop,e->start);

  if(dae->dae_minduration > 0) {
    if(duration < dae->dae_minduration) return 0;
  }

  if(dae->dae_maxduration > 0) {
    if(duration > dae->dae_maxduration) return 0;
  }

  if(dae->dae_weekdays != 0x7f) {
    struct tm tm;
    localtime_r(&e->start, &tm);
    if(!((1 << ((tm.tm_wday ?: 7) - 1)) & dae->dae_weekdays))
      return 0;
  }
  return 1;
}

/**
 *
 */
dvr_autorec_entry_t *
dvr_autorec_create(const char *uuid, htsmsg_t *conf)
{
  dvr_autorec_entry_t *dae;

  dae = calloc(1, sizeof(*dae));

  if (idnode_insert(&dae->dae_id, uuid, &dvr_autorec_entry_class, 0)) {
    if (uuid)
      tvhwarn("dvr", "invalid autorec entry uuid '%s'", uuid);
    free(dae);
    return NULL;
  }

  dae->dae_weekdays = 0x7f;
  dae->dae_pri = DVR_PRIO_NORMAL;
  dae->dae_start = -1;
  dae->dae_start_window = -1;
  dae->dae_config = dvr_config_find_by_name_default(NULL);
  LIST_INSERT_HEAD(&dae->dae_config->dvr_autorec_entries, dae, dae_config_link);

  TAILQ_INSERT_TAIL(&autorec_entries, dae, dae_link);

  idnode_load(&dae->dae_id, conf);

  htsp_autorec_entry_add(dae);

  return dae;
}


dvr_autorec_entry_t*
dvr_autorec_create_htsp(const char *dvr_config_name, const char *title,
                            channel_t *ch, uint32_t enabled, int32_t start, int32_t start_window,
                            uint32_t weekdays, time_t start_extra, time_t stop_extra,
                            dvr_prio_t pri, int retention,
                            int min_duration, int max_duration,
                            const char *owner, const char *creator, const char *comment, 
                            const char *name, const char *directory)
{
  dvr_autorec_entry_t *dae;
  htsmsg_t *conf, *days;

  conf = htsmsg_create_map();
  days = htsmsg_create_list();

  htsmsg_add_u32(conf, "enabled",     enabled > 0 ? 1 : 0);
  htsmsg_add_u32(conf, "retention",   retention);
  htsmsg_add_u32(conf, "pri",         pri);
  htsmsg_add_u32(conf, "minduration", min_duration);
  htsmsg_add_u32(conf, "maxduration", max_duration);
  htsmsg_add_s64(conf, "start_extra", start_extra);
  htsmsg_add_s64(conf, "stop_extra",  stop_extra);
  htsmsg_add_str(conf, "title",       title);
  htsmsg_add_str(conf, "config_name", dvr_config_name ?: "");
  htsmsg_add_str(conf, "owner",       owner ?: "");
  htsmsg_add_str(conf, "creator",     creator ?: "");
  htsmsg_add_str(conf, "comment",     comment ?: "");
  htsmsg_add_str(conf, "name",        name ?: "");
  htsmsg_add_str(conf, "directory",   directory ?: "");

  if (start >= 0)
    htsmsg_add_s32(conf, "start", start);
  if (start_window >= 0)
    htsmsg_add_s32(conf, "start_window", start_window);
  if (ch)
    htsmsg_add_str(conf, "channel", idnode_uuid_as_str(&ch->ch_id));

  int i;
  for (i = 0; i < 7; i++)
    if (weekdays & (1 << i))
      htsmsg_add_u32(days, NULL, i + 1);

  htsmsg_add_msg(conf, "weekdays", days);

  dae = dvr_autorec_create(NULL, conf);
  htsmsg_destroy(conf);

  if (dae) {
    dvr_autorec_save(dae);
    dvr_autorec_changed(dae, 1);
  }

  return dae;
}

/**
 *
 */
dvr_autorec_entry_t *
dvr_autorec_add_series_link(const char *dvr_config_name,
                            epg_broadcast_t *event,
                            const char *owner, const char *creator,
                            const char *comment)
{
  dvr_autorec_entry_t *dae;
  htsmsg_t *conf;
  char *title;
  if (!event || !event->episode)
    return NULL;
  conf = htsmsg_create_map();
  title = regexp_escape(epg_broadcast_get_title(event, NULL));
  htsmsg_add_u32(conf, "enabled", 1);
  htsmsg_add_str(conf, "title", title);
  free(title);
  htsmsg_add_str(conf, "config_name", dvr_config_name ?: "");
  htsmsg_add_str(conf, "channel", channel_get_name(event->channel));
  if (event->serieslink)
    htsmsg_add_str(conf, "serieslink", event->serieslink->uri);
  htsmsg_add_str(conf, "owner", owner ?: "");
  htsmsg_add_str(conf, "creator", creator ?: "");
  htsmsg_add_str(conf, "comment", comment ?: "");
  dae = dvr_autorec_create(NULL, conf);
  htsmsg_destroy(conf);
  return dae;
}

/**
 *
 */
static void
autorec_entry_destroy(dvr_autorec_entry_t *dae, int delconf)
{
  dvr_autorec_purge_spawns(dae, delconf);

  if (delconf)
    hts_settings_remove("dvr/autorec/%s", idnode_uuid_as_str(&dae->dae_id));

  htsp_autorec_entry_delete(dae);

  TAILQ_REMOVE(&autorec_entries, dae, dae_link);
  idnode_unlink(&dae->dae_id);

  if(dae->dae_config)
    LIST_REMOVE(dae, dae_config_link);

  free(dae->dae_name);
  free(dae->dae_directory);
  free(dae->dae_owner);
  free(dae->dae_creator);
  free(dae->dae_comment);

  if(dae->dae_title != NULL) {
    free(dae->dae_title);
    regfree(&dae->dae_title_preg);
  }

  if(dae->dae_channel != NULL)
    LIST_REMOVE(dae, dae_channel_link);

  if(dae->dae_channel_tag != NULL)
    LIST_REMOVE(dae, dae_channel_tag_link);

  if(dae->dae_brand)
    dae->dae_brand->putref(dae->dae_brand);
  if(dae->dae_season)
    dae->dae_season->putref(dae->dae_season);
  if(dae->dae_serieslink)
    dae->dae_serieslink->putref(dae->dae_serieslink);

  free(dae);
}

/**
 *
 */
void
dvr_autorec_save(dvr_autorec_entry_t *dae)
{
  htsmsg_t *m = htsmsg_create_map();

  lock_assert(&global_lock);

  idnode_save(&dae->dae_id, m);
  hts_settings_save(m, "dvr/autorec/%s", idnode_uuid_as_str(&dae->dae_id));
  htsmsg_destroy(m);
}

/* **************************************************************************
 * DVR Autorec Entry Class definition
 * **************************************************************************/

static void
dvr_autorec_entry_class_save(idnode_t *self)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)self;
  dvr_autorec_save(dae);
  dvr_autorec_changed(dae, 1);
}

static void
dvr_autorec_entry_class_delete(idnode_t *self)
{
  autorec_entry_destroy((dvr_autorec_entry_t *)self, 1);
}

static const char *
dvr_autorec_entry_class_get_title (idnode_t *self)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)self;
  const char *s = "";
  if (dae->dae_name && dae->dae_name[0] != '\0')
    s = dae->dae_name;
  else if (dae->dae_comment && dae->dae_comment[0] != '\0')
    s = dae->dae_comment;
  return s;
}

static int
dvr_autorec_entry_class_channel_set(void *o, const void *v)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  channel_t *ch = v ? channel_find_by_uuid(v) : NULL;
  if (ch == NULL) ch = v ? channel_find_by_name(v) : NULL;
  if (ch == NULL) {
    if (dae->dae_channel) {
      LIST_REMOVE(dae, dae_channel_link);
      dae->dae_channel = NULL;
      return 1;
    }
  } else if (dae->dae_channel != ch) {
    if (dae->dae_channel)
      LIST_REMOVE(dae, dae_channel_link);
    dae->dae_channel = ch;
    LIST_INSERT_HEAD(&ch->ch_autorecs, dae, dae_channel_link);
    return 1;
  }
  return 0;
}

static const void *
dvr_autorec_entry_class_channel_get(void *o)
{
  static const char *ret;
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  if (dae->dae_channel)
    ret = idnode_uuid_as_str(&dae->dae_channel->ch_id);
  else
    ret = "";
  return &ret;
}

static char *
dvr_autorec_entry_class_channel_rend(void *o)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  if (dae->dae_channel)
    return strdup(channel_get_name(dae->dae_channel));
  return NULL;
}

static int
dvr_autorec_entry_class_title_set(void *o, const void *v)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  const char *title = v ?: "";
  if (strcmp(title, dae->dae_title ?: "")) {
    if (dae->dae_title) {
       regfree(&dae->dae_title_preg);
       free(dae->dae_title);
       dae->dae_title = NULL;
    }
    if (title[0] != '\0' &&
        !regcomp(&dae->dae_title_preg, title,
                 REG_ICASE | REG_EXTENDED | REG_NOSUB))
      dae->dae_title = strdup(title);
    return 1;
  }
  return 0;
}

static int
dvr_autorec_entry_class_tag_set(void *o, const void *v)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  channel_tag_t *tag = v ? channel_tag_find_by_uuid(v) : NULL;
  if (tag == NULL) tag = v ? channel_tag_find_by_name(v, 0) : NULL;
  if (tag == NULL && dae->dae_channel_tag) {
    LIST_REMOVE(dae, dae_channel_tag_link);
    dae->dae_channel_tag = NULL;
    return 1;
  } else if (dae->dae_channel_tag != tag) {
    if (dae->dae_channel_tag)
      LIST_REMOVE(dae, dae_channel_tag_link);
    dae->dae_channel_tag = tag;
    LIST_INSERT_HEAD(&tag->ct_autorecs, dae, dae_channel_tag_link);
    return 1;
  }
  return 0;
}

static const void *
dvr_autorec_entry_class_tag_get(void *o)
{
  static const char *ret;
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  if (dae->dae_channel_tag)
    ret = idnode_uuid_as_str(&dae->dae_channel_tag->ct_id);
  else
    ret = "";
  return &ret;
}

static char *
dvr_autorec_entry_class_tag_rend(void *o)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  if (dae->dae_channel_tag)
    return strdup(dae->dae_channel_tag->ct_name);
  return NULL;
}

static int
dvr_autorec_entry_class_time_set(void *o, const void *v, int *tm)
{
  const char *s = v;
  int t;

  if(s == NULL || s[0] == '\0' || !isdigit(s[0]))
    t = -1;
  else if(strchr(s, ':') != NULL)
    // formatted time string - convert
    t = (atoi(s) * 60) + atoi(s + 3);
  else {
    t = atoi(s);
  }
  if (t >= 24 * 60)
    t = -1;
  if (t != *tm) {
    *tm = t;
    return 1;
  }
  return 0;
}

static int
dvr_autorec_entry_class_start_set(void *o, const void *v)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  return dvr_autorec_entry_class_time_set(o, v, &dae->dae_start);
}

static int
dvr_autorec_entry_class_start_window_set(void *o, const void *v)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  return dvr_autorec_entry_class_time_set(o, v, &dae->dae_start_window);
}

static const void *
dvr_autorec_entry_class_time_get(void *o, int tm)
{
  static const char *ret;
  static char buf[16];
  if (tm >= 0)
    snprintf(buf, sizeof(buf), "%02d:%02d", tm / 60, tm % 60);
  else
    strcpy(buf, "Any");
  ret = buf;
  return &ret;
}

static const void *
dvr_autorec_entry_class_start_get(void *o)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  return dvr_autorec_entry_class_time_get(o, dae->dae_start);
}

static const void *
dvr_autorec_entry_class_start_window_get(void *o)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  return dvr_autorec_entry_class_time_get(o, dae->dae_start_window);
}

htsmsg_t *
dvr_autorec_entry_class_time_list(void *o, const char *null)
{
  int i;
  htsmsg_t *l = htsmsg_create_list();
  char buf[16];
  htsmsg_add_str(l, NULL, null);
  for (i = 0; i < 24*60;  i += 10) {
    snprintf(buf, sizeof(buf), "%02d:%02d", i / 60, (i % 60));
    htsmsg_add_str(l, NULL, buf);
  }
  return l;
}

static htsmsg_t *
dvr_autorec_entry_class_time_list_(void *o)
{
  return dvr_autorec_entry_class_time_list(o, "Any");
}

static htsmsg_t *
dvr_autorec_entry_class_extra_list(void *o)
{
  return dvr_entry_class_duration_list(o, "Not set (use channel or DVR config)", 4*60, 1);
}

static htsmsg_t *
dvr_autorec_entry_class_minduration_list(void *o)
{
  return dvr_entry_class_duration_list(o, "Any", 24*60, 60);
}

static htsmsg_t *
dvr_autorec_entry_class_maxduration_list(void *o)
{
  return dvr_entry_class_duration_list(o, "Any", 24*60, 60);
}

static int
dvr_autorec_entry_class_config_name_set(void *o, const void *v)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  dvr_config_t *cfg = v ? dvr_config_find_by_uuid(v) : NULL;
  if (cfg == NULL) cfg = v ? dvr_config_find_by_name_default(v): NULL;
  if (cfg == NULL && dae->dae_config) {
    dae->dae_config = NULL;
    LIST_REMOVE(dae, dae_config_link);
    return 1;
  } else if (cfg != dae->dae_config) {
    if (dae->dae_config)
      LIST_REMOVE(dae, dae_config_link);
    LIST_INSERT_HEAD(&cfg->dvr_autorec_entries, dae, dae_config_link);
    dae->dae_config = cfg;
    return 1;
  }
  return 0;
}

static const void *
dvr_autorec_entry_class_config_name_get(void *o)
{
  static const char *ret;
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  if (dae->dae_config)
    ret = idnode_uuid_as_str(&dae->dae_config->dvr_id);
  else
    ret = "";
  return &ret;
}

static char *
dvr_autorec_entry_class_config_name_rend(void *o)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  if (dae->dae_config)
    return strdup(dae->dae_config->dvr_config_name);
  return NULL;
}

static int
dvr_autorec_entry_class_weekdays_set(void *o, const void *v)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  htsmsg_field_t *f;
  uint32_t u32, bits = 0;

  HTSMSG_FOREACH(f, (htsmsg_t *)v)
    if (!htsmsg_field_get_u32(f, &u32) && u32 > 0 && u32 < 8)
      bits |= (1 << (u32 - 1));

  if (bits != dae->dae_weekdays) {
    dae->dae_weekdays = bits;
    return 1;
  }
  return 0;
}

htsmsg_t *
dvr_autorec_entry_class_weekdays_get(uint32_t weekdays)
{
  htsmsg_t *m = htsmsg_create_list();
  int i;
  for (i = 0; i < 7; i++)
    if (weekdays & (1 << i))
      htsmsg_add_u32(m, NULL, i + 1);
  return m;
}

static htsmsg_t *
dvr_autorec_entry_class_weekdays_default(void)
{
  return dvr_autorec_entry_class_weekdays_get(0x7f);
}

static const void *
dvr_autorec_entry_class_weekdays_get_(void *o)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  return dvr_autorec_entry_class_weekdays_get(dae->dae_weekdays);
}

static const struct strtab dvr_autorec_entry_class_weekdays_tab[] = {
  { "Mon", 1 },
  { "Tue", 2 },
  { "Wed", 3 },
  { "Thu", 4 },
  { "Fri", 5 },
  { "Sat", 6 },
  { "Sun", 7 },
};

htsmsg_t *
dvr_autorec_entry_class_weekdays_list ( void *o )
{
  return strtab2htsmsg(dvr_autorec_entry_class_weekdays_tab);
}

char *
dvr_autorec_entry_class_weekdays_rend(uint32_t weekdays)
{
  char buf[32];
  size_t l;
  int i;
  if (weekdays == 0x7f)
    strcpy(buf + 1, "All days");
  else if (weekdays == 0)
    strcpy(buf + 1, "No days");
  else {
    buf[0] = '\0';
    for (i = 0; i < 7; i++)
      if (weekdays & (1 << i)) {
        l = strlen(buf);
        snprintf(buf + l, sizeof(buf) - l, ",%s",
                 val2str(i + 1, dvr_autorec_entry_class_weekdays_tab));
      }
  }
  return strdup(buf + 1);
}

static char *
dvr_autorec_entry_class_weekdays_rend_(void *o)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  return dvr_autorec_entry_class_weekdays_rend(dae->dae_weekdays);
}

static int
dvr_autorec_entry_class_brand_set(void *o, const void *v)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  int save;
  epg_brand_t *brand;

  if (v && *(char *)v == '\0')
    v = NULL;
  brand = v ? epg_brand_find_by_uri(v, 1, &save) : NULL;
  if (brand && dae->dae_brand != brand) {
    if (dae->dae_brand)
      dae->dae_brand->putref((epg_object_t*)dae->dae_brand);
    brand->getref((epg_object_t*)brand);
    dae->dae_brand = brand;
    return 1;
  } else if (brand == NULL && dae->dae_brand) {
    dae->dae_brand->putref((epg_object_t*)dae->dae_brand);
    dae->dae_brand = NULL;
    return 1;
  }
  return 0;
}

static const void *
dvr_autorec_entry_class_brand_get(void *o)
{
  static const char *ret;
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  if (dae->dae_brand)
    ret = dae->dae_brand->uri;
  else
    ret = "";
  return &ret;
}

static int
dvr_autorec_entry_class_season_set(void *o, const void *v)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  int save;
  epg_season_t *season;

  if (v && *(char *)v == '\0')
    v = NULL;
  season = v ? epg_season_find_by_uri(v, 1, &save) : NULL;
  if (season && dae->dae_season != season) {
    if (dae->dae_season)
      dae->dae_season->putref((epg_object_t*)dae->dae_season);
    season->getref((epg_object_t*)season);
    dae->dae_season = season;
    return 1;
  } else if (season == NULL && dae->dae_season) {
    dae->dae_season->putref((epg_object_t*)dae->dae_season);
    dae->dae_season = NULL;
    return 1;
  }
  return 0;
}

static const void *
dvr_autorec_entry_class_season_get(void *o)
{
  static const char *ret;
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  if (dae->dae_season)
    ret = dae->dae_season->uri;
  else
    ret = "";
  return &ret;
}

static int
dvr_autorec_entry_class_series_link_set(void *o, const void *v)
{
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  int save;
  epg_serieslink_t *sl;

  if (v && *(char *)v == '\0')
    v = NULL;
  sl = v ? epg_serieslink_find_by_uri(v, 1, &save) : NULL;
  if (sl && dae->dae_serieslink != sl) {
    if (dae->dae_serieslink)
      dae->dae_serieslink->putref((epg_object_t*)dae->dae_season);
    sl->getref((epg_object_t*)sl);
    dae->dae_serieslink = sl;
    return 1;
  } else if (sl == NULL && dae->dae_serieslink) {
    dae->dae_season->putref((epg_object_t*)dae->dae_season);
    dae->dae_season = NULL;
    return 1;
  }
  return 0;
}

static const void *
dvr_autorec_entry_class_series_link_get(void *o)
{
  static const char *ret;
  dvr_autorec_entry_t *dae = (dvr_autorec_entry_t *)o;
  if (dae->dae_serieslink)
    ret = dae->dae_serieslink->uri;
  else
    ret = "";
  return &ret;
}

static htsmsg_t *
dvr_autorec_entry_class_content_type_list(void *o)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "type",  "api");
  htsmsg_add_str(m, "uri",   "epg/content_type/list");
  return m;
}

const idclass_t dvr_autorec_entry_class = {
  .ic_class      = "dvrautorec",
  .ic_caption    = "DVR Auto-Record Entry",
  .ic_event      = "dvrautorec",
  .ic_save       = dvr_autorec_entry_class_save,
  .ic_get_title  = dvr_autorec_entry_class_get_title,
  .ic_delete     = dvr_autorec_entry_class_delete,
  .ic_properties = (const property_t[]) {
    {
      .type     = PT_BOOL,
      .id       = "enabled",
      .name     = "Enabled",
      .off      = offsetof(dvr_autorec_entry_t, dae_enabled),
    },
    {
      .type     = PT_STR,
      .id       = "name",
      .name     = "Name",
      .off      = offsetof(dvr_autorec_entry_t, dae_name),
    },
	{
      .type     = PT_STR,
      .id       = "directory",
      .name     = "Directory",
      .off      = offsetof(dvr_autorec_entry_t, dae_directory),
    },
    {
      .type     = PT_STR,
      .id       = "title",
      .name     = "Title (Regexp)",
      .set      = dvr_autorec_entry_class_title_set,
      .off      = offsetof(dvr_autorec_entry_t, dae_title),
    },
    {
      .type     = PT_STR,
      .id       = "channel",
      .name     = "Channel",
      .set      = dvr_autorec_entry_class_channel_set,
      .get      = dvr_autorec_entry_class_channel_get,
      .rend     = dvr_autorec_entry_class_channel_rend,
      .list     = channel_class_get_list,
    },
    {
      .type     = PT_STR,
      .id       = "tag",
      .name     = "Channel Tag",
      .set      = dvr_autorec_entry_class_tag_set,
      .get      = dvr_autorec_entry_class_tag_get,
      .rend     = dvr_autorec_entry_class_tag_rend,
      .list     = channel_tag_class_get_list,
    },
    {
      .type     = PT_STR,
      .id       = "start",
      .name     = "Start After",
      .set      = dvr_autorec_entry_class_start_set,
      .get      = dvr_autorec_entry_class_start_get,
      .list     = dvr_autorec_entry_class_time_list_,
      .opts     = PO_SORTKEY
    },
    {
      .type     = PT_STR,
      .id       = "start_window",
      .name     = "Start Before",
      .set      = dvr_autorec_entry_class_start_window_set,
      .get      = dvr_autorec_entry_class_start_window_get,
      .list     = dvr_autorec_entry_class_time_list_,
      .opts     = PO_SORTKEY,
    },
    {
      .type     = PT_TIME,
      .id       = "start_extra",
      .name     = "Extra Start Time",
      .off      = offsetof(dvr_autorec_entry_t, dae_start_extra),
      .list     = dvr_autorec_entry_class_extra_list,
      .opts     = PO_DURATION | PO_SORTKEY
    },
    {
      .type     = PT_TIME,
      .id       = "stop_extra",
      .name     = "Extra Stop Time",
      .off      = offsetof(dvr_autorec_entry_t, dae_stop_extra),
      .list     = dvr_autorec_entry_class_extra_list,
      .opts     = PO_DURATION | PO_SORTKEY
    },
    {
      .type     = PT_U32,
      .islist   = 1,
      .id       = "weekdays",
      .name     = "Week Days",
      .set      = dvr_autorec_entry_class_weekdays_set,
      .get      = dvr_autorec_entry_class_weekdays_get_,
      .list     = dvr_autorec_entry_class_weekdays_list,
      .rend     = dvr_autorec_entry_class_weekdays_rend_,
      .def.list = dvr_autorec_entry_class_weekdays_default
    },
    {
      .type     = PT_INT,
      .id       = "minduration",
      .name     = "Minimal Duration",
      .list     = dvr_autorec_entry_class_minduration_list,
      .off      = offsetof(dvr_autorec_entry_t, dae_minduration),
    },
    {
      .type     = PT_INT,
      .id       = "maxduration",
      .name     = "Maximal Duration",
      .list     = dvr_autorec_entry_class_maxduration_list,
      .off      = offsetof(dvr_autorec_entry_t, dae_maxduration),
    },
    {
      .type     = PT_U32,
      .id       = "content_type",
      .name     = "Content Type",
      .list     = dvr_autorec_entry_class_content_type_list,
      .off      = offsetof(dvr_autorec_entry_t, dae_content_type),
    },
    {
      .type     = PT_U32,
      .id       = "pri",
      .name     = "Priority",
      .list     = dvr_entry_class_pri_list,
      .def.i    = DVR_PRIO_NORMAL,
      .off      = offsetof(dvr_autorec_entry_t, dae_pri),
    },
    {
      .type     = PT_INT,
      .id       = "retention",
      .name     = "Retention",
      .off      = offsetof(dvr_autorec_entry_t, dae_retention),
    },
    {
      .type     = PT_STR,
      .id       = "config_name",
      .name     = "DVR Configuration",
      .set      = dvr_autorec_entry_class_config_name_set,
      .get      = dvr_autorec_entry_class_config_name_get,
      .rend     = dvr_autorec_entry_class_config_name_rend,
      .list     = dvr_entry_class_config_name_list,
    },
    {
      .type     = PT_STR,
      .id       = "brand",
      .name     = "Brand",
      .set      = dvr_autorec_entry_class_brand_set,
      .get      = dvr_autorec_entry_class_brand_get,
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "season",
      .name     = "Season",
      .set      = dvr_autorec_entry_class_season_set,
      .get      = dvr_autorec_entry_class_season_get,
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "serieslink",
      .name     = "Series Link",
      .set      = dvr_autorec_entry_class_series_link_set,
      .get      = dvr_autorec_entry_class_series_link_get,
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "owner",
      .name     = "Owner",
      .off      = offsetof(dvr_autorec_entry_t, dae_owner),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "creator",
      .name     = "Creator",
      .off      = offsetof(dvr_autorec_entry_t, dae_creator),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "comment",
      .name     = "Comment",
      .off      = offsetof(dvr_autorec_entry_t, dae_comment),
    },
    {}
  }
};

/**
 *
 */
void
dvr_autorec_init(void)
{
  htsmsg_t *l, *c;
  htsmsg_field_t *f;

  TAILQ_INIT(&autorec_entries);
  dvr_autorec_in_init = 1;
  if((l = hts_settings_load("dvr/autorec")) != NULL) {
    HTSMSG_FOREACH(f, l) {
      if((c = htsmsg_get_map_by_field(f)) == NULL)
        continue;
      (void)dvr_autorec_create(f->hmf_name, c);
    }
    htsmsg_destroy(l);
  }
  dvr_autorec_in_init = 0;
}

void
dvr_autorec_done(void)
{
  dvr_autorec_entry_t *dae;

  pthread_mutex_lock(&global_lock);
  while ((dae = TAILQ_FIRST(&autorec_entries)) != NULL)
    autorec_entry_destroy(dae, 0);
  pthread_mutex_unlock(&global_lock);
}

void
dvr_autorec_update(void)
{
  dvr_autorec_entry_t *dae;
  TAILQ_FOREACH(dae, &autorec_entries, dae_link) {
    dvr_autorec_changed(dae, 0);
  }
}

/**
 *
 */
void
dvr_autorec_check_event(epg_broadcast_t *e)
{
  dvr_autorec_entry_t *dae;

  TAILQ_FOREACH(dae, &autorec_entries, dae_link)
    if(autorec_cmp(dae, e))
      dvr_entry_create_by_autorec(e, dae);
  // Note: no longer updating event here as it will be done from EPG
  //       anyway
}

void dvr_autorec_check_brand(epg_brand_t *b)
{
// Note: for the most part this will only be relevant should an episode
//       to which a broadcast is linked suddenly get added to a new brand
//       this is pretty damn unlikely!
}

void dvr_autorec_check_season(epg_season_t *s)
{
// Note: I guess new episodes might have been added, but again its likely
//       this will already have been picked up by the check_event call
}

void dvr_autorec_check_serieslink(epg_serieslink_t *s)
{
// TODO: need to implement this
}

/**
 *
 */
void
dvr_autorec_changed(dvr_autorec_entry_t *dae, int purge)
{
  channel_t *ch;
  epg_broadcast_t *e;

  if (purge)
    dvr_autorec_purge_spawns(dae, 1);

  CHANNEL_FOREACH(ch) {
    if (!ch->ch_enabled) continue;
    RB_FOREACH(e, &ch->ch_epg_schedule, sched_link) {
      if(autorec_cmp(dae, e))
        dvr_entry_create_by_autorec(e, dae);
    }
  }

  htsp_autorec_entry_update(dae);
}


/**
 *
 */
void
autorec_destroy_by_channel(channel_t *ch, int delconf)
{
  dvr_autorec_entry_t *dae;

  while((dae = LIST_FIRST(&ch->ch_autorecs)) != NULL)
    autorec_entry_destroy(dae, delconf);
}

/*
 *
 */
void
autorec_destroy_by_channel_tag(channel_tag_t *ct, int delconf)
{
  dvr_autorec_entry_t *dae;

  while((dae = LIST_FIRST(&ct->ct_autorecs)) != NULL) {
    LIST_REMOVE(dae, dae_channel_tag_link);
    dae->dae_channel_tag = NULL;
    idnode_notify_simple(&dae->dae_id);
    if (delconf)
      dvr_autorec_save(dae);
  }
}

/*
 *
 */
void
autorec_destroy_by_id(const char *id, int delconf)
{
  dvr_autorec_entry_t *dae;
  dae = dvr_autorec_find_by_uuid(id);

  if (dae)
    autorec_entry_destroy(dae, delconf);
}

/**
 *
 */
void
autorec_destroy_by_config(dvr_config_t *kcfg, int delconf)
{
  dvr_autorec_entry_t *dae;
  dvr_config_t *cfg = NULL;

  while((dae = LIST_FIRST(&kcfg->dvr_autorec_entries)) != NULL) {
    LIST_REMOVE(dae, dae_config_link);
    if (cfg == NULL && delconf)
      cfg = dvr_config_find_by_name_default(NULL);
    if (cfg)
      LIST_INSERT_HEAD(&cfg->dvr_autorec_entries, dae, dae_config_link);
    dae->dae_config = cfg;
    if (delconf)
      dvr_autorec_save(dae);
  }
}
