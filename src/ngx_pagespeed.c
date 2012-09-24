/*
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Usage:
 *  server {
 *    pagespeed    on|off;
 *    logstuff     on|off;
 *  }
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

ngx_module_t ngx_pagespeed;

// Hack for terser exploration.
#define DBG(r, args...) \
  ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0, args)

typedef struct {
  ngx_flag_t  logstuff;
  ngx_flag_t  active;
} ngx_http_pagespeed_loc_conf_t;

static ngx_command_t ngx_http_pagespeed_commands[] = {
  { ngx_string("logstuff"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_flag_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_pagespeed_loc_conf_t, logstuff),
    NULL },

  { ngx_string("pagespeed"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_flag_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_pagespeed_loc_conf_t, active),
    NULL },

  ngx_null_command
};

static void *
ngx_http_pagespeed_create_loc_conf(ngx_conf_t *cf)
{
  ngx_http_pagespeed_loc_conf_t  *conf;

  conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_pagespeed_loc_conf_t));
  if (conf == NULL) {
    return NGX_CONF_ERROR;
  }
  conf->logstuff = NGX_CONF_UNSET;
  conf->active = NGX_CONF_UNSET;
  return conf;
}

static char *
ngx_http_pagespeed_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
  ngx_http_pagespeed_loc_conf_t *prev = parent;
  ngx_http_pagespeed_loc_conf_t *conf = child;

  ngx_conf_merge_value(conf->logstuff, prev->logstuff, 0);  // Default off.
  ngx_conf_merge_value(conf->active, prev->active, 0);  // Default off.

  return NGX_CONF_OK;
}

static ngx_http_output_body_filter_pt ngx_http_next_body_filter;


ngx_int_t exp_note_processed(ngx_http_request_t *r, ngx_chain_t *in)
{
  // Find the end of the buffer chain.
  ngx_chain_t *chain_link;
  int chain_contains_last_buffer = 0;
  for ( chain_link = in; chain_link != NULL; chain_link = chain_link->next ) {
    if (chain_link->buf->last_buf) {
      chain_contains_last_buffer = 1;
      if (chain_link->next != NULL) {
        DBG(r, "Chain link thinks its last but has a child.");
        return NGX_ERROR;
      }
      break;  // Chain link now is the last link in the chain.
    }
  }

  if (!chain_contains_last_buffer) {
    // None of the buffers had last_buf set, meaning we have an incomplete chain
    // and are still waiting to get the final buffer.  Let other body filters
    // act on the buffers we have so far and wait until we're called again with
    // the last buffer.
    DBG(r, "Need the last buffer.");
    return ngx_http_next_body_filter(r, in);
  }

  // Prepare a new buffer to put the note into.
  ngx_buf_t *b = ngx_calloc_buf(r->pool);
  if (b == NULL) {
    return NGX_ERROR;
  }

  // Write to the new buffer.
  // FIXME(jefftk): this doesn't work
  char *note = "<!-- Processed through ngx_pagespeed -->";
  int note_len = strlen(note);
  b->pos = ngx_pnalloc(r->pool, note_len);
  strncpy((char*)b->pos, note, note_len);
  b->last = b->pos + note_len - 1;
  DBG(r, "\nAttempted to append: '%*s'\n", note_len, b->pos);

  // Link the new buffer into the buffer chain.
  ngx_chain_t *added_link = ngx_alloc_chain_link(r->pool);
  if (added_link == NULL) {
    return NGX_ERROR;
  }

  added_link->buf = b;
  added_link->next = NULL;
  chain_link->next = added_link;

  chain_link->buf->last_buf = 0;
  added_link->buf->last_buf = 1;

  return NGX_OK;
}

void exp_inspect_buffer_chain(ngx_http_request_t *r, ngx_chain_t *in)
{
  // Inspect the buffer chain
  DBG(r, "Inspecting buffer chain");
  ngx_chain_t *chain_link;
  int link_no = 0;
  for ( chain_link = in;
        chain_link != NULL;
        chain_link = chain_link->next, link_no++ ) {
    ngx_buf_t *b = chain_link->buf;
    DBG(r,
        "\n\n"
        "Link %d\n"
        "  pos: %p\n"
        "  last: %p\n"
        "  file_pos: %d\n"
        "  file_last: %d\n"
        "  start: %p\n"
        "  end: %p\n"
        "\n",
        link_no,
        b->pos,
        b->last,
        b->file_pos,
        b->file_last,
        b->start,
        b->end
        );

    if (b->pos && b->last) {
      DBG(r, "\n[pos last] contains: '%*s'\n", b->last - b->pos, b->pos);
    }
    if (b->start && b->end) {
      DBG(r, "\n[start end] contains: '%*s'\n", b->end - b->start, b->start);
    }
    if (b->file) {
      ssize_t file_size = b->file_last - b->file_pos;
      u_char *tmp_buf = ngx_pnalloc(r->pool, file_size);
      ssize_t n = ngx_read_file(b->file, tmp_buf, file_size, b->file_pos);
      if (n != file_size) {
        DBG(r, "Failed to read file; got %d bytes expected %s bytes",
            n, file_size);
      } else {
        DBG(r, "\n[file_pos file_last] contains: '%*s'\n",
            file_size, tmp_buf);
      }
      ngx_pfree(r->pool, tmp_buf);
    }
  }
}

static
ngx_int_t ngx_http_pagespeed_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
  ngx_http_pagespeed_loc_conf_t *pagespeed_config;
  pagespeed_config = ngx_http_get_module_loc_conf(r, ngx_pagespeed);

  if (pagespeed_config->logstuff) {
    DBG(r, "We've been invoked!");
  }

  ngx_int_t status = exp_note_processed(r, in);
  if (status != NGX_OK) {
    return status;
  }

  if (pagespeed_config->logstuff) {
    exp_inspect_buffer_chain(r, in);
  }

  // Continue with the next filter.
  return ngx_http_next_body_filter(r, in);
}

static ngx_int_t
ngx_http_pagespeed_init(ngx_conf_t *cf)
{
  ngx_http_pagespeed_loc_conf_t *pagespeed_config;
  pagespeed_config = ngx_http_conf_get_module_loc_conf(cf, ngx_pagespeed);

  if (pagespeed_config->active) {
    if (pagespeed_config->logstuff) {
      ngx_conf_log_error(NGX_LOG_ERR, cf, 0, "We're active!");
    }
    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_pagespeed_body_filter;
  }

  return NGX_OK;
}

static ngx_http_module_t ngx_http_pagespeed_module_ctx = {
  NULL,
  ngx_http_pagespeed_init,  // Post configuration.
  NULL,
  NULL,
  NULL,
  NULL,
  ngx_http_pagespeed_create_loc_conf,
  ngx_http_pagespeed_merge_loc_conf
};

ngx_module_t ngx_pagespeed = {
  NGX_MODULE_V1,
  &ngx_http_pagespeed_module_ctx,
  ngx_http_pagespeed_commands,
  NGX_HTTP_MODULE,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NGX_MODULE_V1_PADDING
};

