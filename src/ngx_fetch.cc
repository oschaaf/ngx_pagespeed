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

// Author: x.dinic@gmail.com (Junmin Xiong)
// Fetch resources through asynchronous,it run in a nginx event loop
//  - resolver event would be hook when started a NgxFetch
//  - when NgxFetchResolveDone is called NgxFetch compled resolver,then 
//    connect, create a "reuqest", hooked write and read event.
//  - write, read and handler response in a event loop.

#include "ngx_fetch.h"
#include "net/instaweb/util/public/basictypes.h"
#include "base/logging.h"

#include <algorithm>
#include <string>
#include <vector>

#include "net/instaweb/util/public/basictypes.h"
#include "base/scoped_ptr.h"
#include "base/stl_util-inl.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/inflating_fetch.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/response_headers_parser.h"
#include "net/instaweb/public/version.h"
#include "net/instaweb/util/public/condvar.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/pool.h"
#include "net/instaweb/util/public/pool_element.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/public/writer.h"

namespace net_instaweb {
  NgxFetch::NgxFetch(const GoogleString& url,
    AsyncFetch* async_fetch,
    MessageHandler* message_handler,
    int64 timeout_ms)
  : str_url_(url),
    fetcher_(NULL),
    async_fetch_(async_fetch),
    parser_(async_fetch->response_headers()),
    message_handler_(message_handler),
    bytes_received_(0),
    fetch_start_ms_(0),
    fetch_end_ms_(0) {
    ngx_memzero(&url_, sizeof(url_));
    log_ = NULL;
    pool_ = NULL;
    timeout_event_ = NULL;
    connection_ = NULL;
  }

  NgxFetch::~NgxFetch() {
    if (timeout_event_ && timeout_event_->timer_set) {
        ngx_del_timer(timeout_event_);
    }
    if (connection_ != NULL) {
      ngx_close_connection(connection_);
    }
    if (pool_ != NULL) {
      ngx_destroy_pool(pool_);
    }
  }

  // this function call by NgxUrlAsyncFetcher::StartFetch
  // ParseUrl and init a fetch
  bool NgxFetch::Start(NgxUrlAsyncFetcher* fetcher) {
    fetcher_ = fetcher;
    
    if (!Init()) {
      Cancel();
      return false;
    }

    return true;
  }

  // init pool, timeout event and hook dns resolver handler
  bool NgxFetch::Init() {
    //TODO: log when return false;

    pool_ = ngx_create_pool(12288, log_);
    if (pool_ == NULL) { return false; }

    if (!ParseUrl()) {
      Cancel();
      return false;
    }
    timeout_event_ = static_cast<ngx_event_t *>(ngx_pcalloc(pool_, sizeof(ngx_event_t)));
    if (timeout_event_ == NULL) { return false; }
    timeout_event_->data = this;
    timeout_event_->handler = NgxFetchTimeout;

    ngx_event_add_timer(timeout_event_, static_cast<ngx_msec_t> (timeout_ms_));
    r_ = static_cast<ngx_http_request_t *>(ngx_pcalloc(pool_,
                                           sizeof(ngx_http_request_t)));
    if (r_ == NULL) { return false; }
    status_ = static_cast<ngx_http_status_t*>(ngx_pcalloc(pool_,
                                              sizeof(ngx_http_status_t)));
    if (status_ == NULL) { return false;}
    ngx_resolver_ctx_t temp;
    temp.name.data = url_.host.data;
    temp.name.len = url_.host.len;
    resolver_ctx_ = ngx_resolve_start(fetcher_->resolver_, &temp);
    if (resolver_ctx_ == NULL || resolver_ctx_ == NGX_NO_RESOLVER) {
      return false;
    }

    resolver_ctx_->data = this;
    resolver_ctx_->name.data = url_.host.data;
    resolver_ctx_->name.len = url_.host.len;
    resolver_ctx_->type = NGX_RESOLVE_A;
    resolver_ctx_->handler = NgxFetchResolveDone;
    resolver_ctx_->timeout = fetcher_->resolver_timeout_;

    if (ngx_resolve_name(resolver_ctx_) != NGX_OK) {
      return false;
    }
    return true;
  }

  const char* NgxFetch::str_url() { return str_url_.c_str(); }

  // cancel a fetch, delete timeout event and close the connection
  void NgxFetch::Cancel() {
    if (timeout_event_ != NULL && timeout_event_->timer_set) {
      ngx_del_timer(timeout_event_);
    }
    if (connection_) {
      ngx_close_connection(connection_);
    }

    CallbackDone(false);
  }

  // this function called only once. the fetch with error when success eq false
  void NgxFetch::CallbackDone(bool success) {
    if (async_fetch_ == NULL) {
      LOG(FATAL) << "BUG: Serf callback called more than once on same fetch "
        << str_url_.c_str() << "(" << this << ").Please report this"
        << "at https://groups.google.com/forum/#!forum/ngx-pagespeed-discuss";
      return;
    }

    if (fetcher_->track_original_content_length()
        && async_fetch_->response_headers()->Has(
          HttpAttributes::kXOriginalContentLength)) {
      async_fetch_->extra_response_headers()->SetOriginalContentLength(
          bytes_received_);
    }
    fetcher_->FetchComplete(this);
    async_fetch_->Done(success);
    async_fetch_ = NULL;
  }

  size_t NgxFetch::bytes_received() { return bytes_received_; }
  void NgxFetch::bytes_received_add(int64 x) { bytes_received_ += x;}

  int64 NgxFetch::fetch_start_ms() { return fetch_start_ms_; }

  void NgxFetch::set_fetch_start_ms(int64 start_ms) {
    fetch_start_ms_ = start_ms;
  }

  int64 NgxFetch::fetch_end_ms() { return fetch_end_ms_; }

  void NgxFetch::set_fetch_end_ms(int64 end_ms) { fetch_end_ms_ = end_ms; }

  MessageHandler* NgxFetch::message_handler() { return message_handler_; }

  bool NgxFetch::ParseUrl() {
    url_.url.len = str_url_.length();
    url_.url.data = static_cast<u_char*>(ngx_palloc(pool_, url_.url.len));
    if (url_.url.data == NULL) {
      return false;
    }
    str_url_.copy(reinterpret_cast<char*>(url_.url.data), str_url_.length(), 0);
    size_t add;
    u_short port;
    if (ngx_strncasecmp(url_.url.data, (u_char*)"http://", 7) == 0) {
      add = 7;
      port = 80;

    } else if (ngx_strncasecmp(url_.url.data, (u_char*)"https://", 8) == 0) {
      add = 8;
      port = 443;

    } else { // not https or http
      return false;
    }

    url_.url.data += add;
    url_.url.len -= add;
    url_.default_port = port;
    url_.no_resolve = 1;
    url_.uri_part = 1;

    if (ngx_parse_url(pool_, &url_) == NGX_OK) {
      return true;
    }

    return false;
  }

  // init a request when resolver has beed done.
  void NgxFetch::NgxFetchResolveDone(ngx_resolver_ctx_t* resolver_ctx) {
    NgxFetch* fetch = static_cast<NgxFetch*>(resolver_ctx->data);
    if (resolver_ctx->state != NGX_OK) {
      fetch->Cancel();
      return;
    }

    ngx_resolve_name_done(resolver_ctx);

    if (fetch->InitRquest() != NGX_OK) {
      // TODO : LOG;
      fetch->Cancel();
    }
  }

  // prepare send data for this fetch, and hook write event.
  int NgxFetch::InitRquest() {
    in_ = ngx_create_temp_buf(pool_, 4096);
    if (in_ == NULL) {
      return NGX_ERROR;
    }

    FixUserAgent();
    FixHost();
    RequestHeaders* request_headers = async_fetch_->request_headers();
    ConstStringStarVector v;
    size_t size = 0;
    size = sizeof("GET ") - 1 + url_.uri.len + sizeof("HTTP/1.1\r\n") - 1;
    for (int i = 0; i < request_headers->NumAttributes(); i++) {

      // name: value\r\n
      size += request_headers->Name(i).length()
           + request_headers->Value(i).length()
           + 4; // for ": \r\n"
    }
    size += 2; // "\r\n";
    out_ = ngx_create_temp_buf(pool_, size);
    if (out_ == NULL) {
      return NGX_ERROR;
    }

    out_->last = ngx_cpymem(out_->last, "GET ", 4);
    out_->last = ngx_cpymem(out_->last, url_.uri.data, url_.uri.len);
    out_->last = ngx_cpymem(out_->last, "HTTP/1.1\r\n", 10);

    for (int i = 0; i < request_headers->NumAttributes(); i++) {
      const GoogleString& name = request_headers->Name(i);
      const GoogleString& value = request_headers->Value(i);
      out_->last = ngx_cpymem(out_->last, name.c_str(), name.length());
      *(out_->last++) = ':';
      *(out_->last++) = ' ';
      out_->last = ngx_cpymem(out_->last, value.c_str(), value.length());
      *(out_->last++) = CR;
      *(out_->last++) = LF;
    }
    *(out_->last++) = CR;
    *(out_->last++) = LF;

    int rc = Connect();
    if (rc == NGX_AGAIN) {
      return NGX_OK;
    } else if (rc < NGX_OK) {
      return rc;
    }

    NgxFetchWrite(connection_->write);
    return NGX_OK;
  }

  int NgxFetch::Connect() {
    struct sockaddr_in sin;
    ngx_memzero(&sin, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(url_.port);
    sin.sin_addr.s_addr = resolver_ctx_->addrs[0];

    ngx_peer_connection_t pc;
    ngx_memzero(&pc, sizeof(pc));
    pc.sockaddr = (struct sockaddr *) &sin;
    pc.socklen = sizeof(struct sockaddr_in);
    pc.name = &url_.host;

    // get callback is dump function, it just returns NGX_OK
    pc.get = ngx_event_get_peer;
    pc.log_error = NGX_ERROR_ERR;
    pc.log = fetcher_->log_;
    pc.rcvbuf = -1;

    int rc = ngx_event_connect_peer(&pc);
    connection_ = pc.connection;
    connection_->write->handler = NgxFetchWrite;
    connection_->read->handler = NgxFetchRead;
    connection_->data = this;

    if (ngx_handle_write_event(connection_->write, 0) != NGX_OK) {
      return NGX_ERROR;
    }

    //TODO: set write timeout when rc == NGX_AGAIN
    return rc;
  }

  // when send completly,hook read event,and set response handler.
  // TODO(jummin) add write timeout when writer return NGX_AGAIN.
  void NgxFetch::NgxFetchWrite(ngx_event_t* wev) {
    ngx_connection_t* c = static_cast<ngx_connection_t*>(wev->data);
    NgxFetch* fetch = static_cast<NgxFetch*>(c->data);
    ngx_buf_t* out = fetch->out_;

    // TODO: set write event timeout

    while (out->pos < out->last) {
      int n = c->send(c, out->pos, out->last - out->pos);
      if (n >= 0) {
        out->pos += n;
      } else if (n == NGX_AGAIN) {
        // add send timeout, 10 only used for test
        // ngx_add_timer(c->write, 10);
        if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
          // write event can not be hooked
          fetch->CallbackDone(false);
        }
        return;
      } else {
        c->error = 1;
        fetch->CallbackDone(false);
        return;
      }
    }

    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
      c->error = 1;
      fetch->CallbackDone(false);
    }

    return;
  }

  // add read timeout when connection isn't ready to recv
  void NgxFetch::NgxFetchRead(ngx_event_t* rev) {
    ngx_connection_t* c = static_cast<ngx_connection_t*>(rev->data);
    NgxFetch* fetch = static_cast<NgxFetch*>(c->data);

    for (;;) {
      int n = c->recv(c, fetch->in_->start, fetch->in_->end - fetch->in_->start);
      if (n == NGX_AGAIN) {
        break;
      }

      if (n == 0) {
        //connection is closed
        fetch->CallbackDone(true);
        return;
      } else if (n > 0) {
        fetch->in_->pos = fetch->in_->start;
        fetch->in_->last = fetch->in_->start + n;
        if (!fetch->response_handler(c)) {
          fetch->Cancel();
        }
      }

      if (!rev->ready) {
        break;
      }
    }

    if (ngx_handle_read_event(rev, 0) != NGX_OK) {
      fetch->Cancel();
    }

    // there is a timer for every fetch, read or write timer is needed??
    if (rev->active) {
    // is 10 OK?
      ngx_add_timer(rev, 10);
    }
  }

  // parse statusline "HTTP/1.1 200 OK\r\n"
  bool NgxFetch::NgxFetchHandleStatusLine(ngx_connection_t* c) {
    NgxFetch* fetch = static_cast<NgxFetch*>(c->data);
    // nginx version befor 1.1.4,ngx_http_parse_status_line didn't 
    // save http_version.so ensure version > 1.1.4
    ngx_int_t n = ngx_http_parse_status_line(fetch->r_, fetch->in_,
                                             fetch->status_);
    if (n == NGX_ERROR) { // parse status line error
      return false;
    } else if (n == NGX_AGAIN) { // not completed
      return true;
    }
    ResponseHeaders* response_headers =
      fetch->async_fetch_->response_headers();
    response_headers->SetStatusAndReason(
        static_cast<HttpStatus::Code>(fetch->get_code()));
    response_headers->set_major_version(fetch->get_major_version());
    response_headers->set_minor_version(fetch->get_minor_version());
    fetch->set_response_handler(NgxFetchHandleHeader);
    return fetch->response_handler(c);
  }

  // parse httpheaders,
  bool NgxFetch::NgxFetchHandleHeader(ngx_connection_t* c) {
    NgxFetch* fetch = static_cast<NgxFetch*>(c->data);
    char* data = reinterpret_cast<char*>(fetch->in_->pos);
    size_t size = fetch->in_->last - fetch->in_->pos;
    size_t n = fetch->parser_.ParseChunk(StringPiece(data, size),
        fetch->message_handler_);
    if (n > size) {
      return false;
    } else if (fetch->parser_.headers_complete()) {
      if (fetch->fetcher_->track_original_content_length()) {
        int64 content_length;
        fetch->async_fetch_->response_headers()->FindContentLength(
            &content_length);
        fetch->async_fetch_->response_headers()->SetOriginalContentLength(
            content_length);
      }

      fetch->in_->pos += n;
      fetch->set_response_handler(NgxFetchHandleBody);
      return fetch->response_handler(c);
    }
    return true;
  }

  bool NgxFetch::NgxFetchHandleBody(ngx_connection_t* c) {
    NgxFetch* fetch = static_cast<NgxFetch*>(c->data);
    char* data = reinterpret_cast<char*>(fetch->in_->pos);
    size_t size = fetch->in_->last - fetch->in_->pos;
    if (size == 0) {
      return true;
    }

    fetch->bytes_received_add(static_cast<int64>(size));
    return fetch->async_fetch_->Write(StringPiece(data, size),
        fetch->message_handler());
  }

  void NgxFetch::NgxFetchTimeout(ngx_event_t* tev) {
    NgxFetch* fetch = static_cast<NgxFetch*>(tev->data);
    if (tev->timer_set) {
      ngx_del_timer(tev);
    }
    fetch->Cancel();
  }

  void NgxFetch::FixHost() {
    RequestHeaders* request_headers = async_fetch_->request_headers();
    request_headers->RemoveAll(HttpAttributes::kHost);

    request_headers->Add(HttpAttributes::kHost,
        GoogleString(reinterpret_cast<char*>(url_.host.data), url_.host.len));
  }

  void NgxFetch::FixUserAgent() {
    GoogleString user_agent;
    ConstStringStarVector v;
    RequestHeaders* request_headers = async_fetch_->request_headers();
    if (request_headers->Lookup(HttpAttributes::kUserAgent, &v)) {
      for(int i = 0, n = v.size(); i < n; i++) {
        if (i != 0) {
          user_agent += " ";
        }

        if (v[i] != NULL) {
          user_agent += *(v[i]);
        }
      }
      request_headers->RemoveAll(HttpAttributes::kUserAgent);
    }
    if (user_agent.empty()) {
      user_agent += "NgxFetch";
    }
    request_headers->Add(HttpAttributes::kUserAgent, user_agent);
  }
} // namespace net_instaweb