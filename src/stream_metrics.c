/* Native HTX observer for ray-haproxy 2.8.25, loaded through Lua's C ABI.
 * Network threads capture timestamps and copy bounded fragments into a SPSC
 * queue. A dedicated in-process thread parses and aggregates each queue. Lua
 * reads coherent cumulative snapshots without waiting for the parser.
 * Built alongside the matching HAProxy binary by ray-haproxy's wheel pipeline.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <haproxy/global.h>
#include <haproxy/filters.h>
#include <haproxy/http_htx.h>
#include <haproxy/htx.h>
#include <haproxy/sample-t.h>
#include <haproxy/stream-t.h>
#include <haproxy/vars.h>
#include <haproxy/listener.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern char haproxy_version[];

#define MAX_PAYLOAD 262144
#define MAX_TAGS 2048
#define MAX_GROUPS 4096
#define MAX_BUCKETS 128
#define MAX_BYTES (128 * 1024 * 1024)
#define MAX_PENDING 1024
#define INLINE_FRAGMENT 2048
#define TAG_HEADER "x-ray-llm-metric-tags"

extern int luaopen_ray_stream_metrics_impl(lua_State *L);
struct histogram {
  uint64_t count, sum, buckets[MAX_BUCKETS + 1];
};
struct schema {
  unsigned n;
  uint64_t bounds[MAX_BUCKETS];
};
struct group {
  struct group *next;
  char *key;
  char *labels[4];
  struct histogram hist[4];
};
struct worker {
  lua_State *parser;
  int decoder, array_mt;
  struct group *groups;
  unsigned group_count;
  atomic_uint_fast64_t errors;
  size_t bytes;
  struct background *background;
  unsigned active;
  int export_job;
  _Alignas(64) pthread_mutex_t hist_lock;
};
struct observation {
  struct worker *owner;
  struct group *group;
  uint64_t started;
  char *buffer;
  size_t used, capacity;
  int last, pending_cr, ended;
  _Alignas(64) atomic_int dead;
  uint64_t retire_sequence;
  struct observation *gc_next;
};
struct item {
  struct observation *observation;
  char *body;
  size_t length;
  uint64_t now, router;
  int ending, has_router;
  int allocated;
  char inline_body[INLINE_FRAGMENT];
};
struct background {
  struct worker *owner;
  pthread_t thread;
  _Alignas(64) atomic_uint_fast64_t head, produced_bytes;
  _Alignas(64) atomic_uint_fast64_t tail, consumed_bytes;
  _Atomic(struct observation *) garbage;
  atomic_int ready, stop;
#ifdef RAY_STREAM_METRICS_UNIT_TEST
  atomic_uint test_delay_us;
#endif
  unsigned users;
  size_t capacity;
  uint64_t high_water;
  size_t allocation;
  struct observation *deferred;
  struct item items[];
};
static struct schema schemas[4];
static __thread struct worker worker;
static int init_background(void);
static int enqueue(
    struct observation *, const char *, size_t, uint64_t, int, int, uint64_t);
static void drop_reference(struct observation *);

static uint64_t clock_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static int init_worker(void) {
  lua_State *L;
  if (worker.parser) return 0;
  L = worker.parser = luaL_newstate();
  if (!L) return -1;
  luaopen_ray_stream_metrics_impl(L);
  lua_getfield(L, -1, "decode_array_with_array_mt");
  lua_pushboolean(L, 1);
  lua_call(L, 1, 0);
  lua_getfield(L, -1, "decode_invalid_numbers");
  lua_pushboolean(L, 0);
  lua_call(L, 1, 0);
  lua_getfield(L, -1, "decode_max_depth");
  lua_pushinteger(L, 64);
  lua_call(L, 1, 0);
  lua_getfield(L, -1, "array_mt");
  worker.array_mt = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, "decode");
  worker.decoder = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_settop(L, 0);
  /* Temporary JSON objects die within one callback and never share a heap
   * with the request router's Lua coroutines. */
  lua_gc(L, LUA_GCGEN, 0, 0);
  return 0;
}
static int is_array(lua_State *L, int index) {
  int equal;
  if (!lua_getmetatable(L, index)) return 0;
  lua_rawgeti(L, LUA_REGISTRYINDEX, worker.array_mt);
  equal = lua_rawequal(L, -1, -2);
  lua_pop(L, 2);
  return equal;
}
static int is_object(lua_State *L, int index) {
  return lua_istable(L, index) && !is_array(L, index);
}
static int decode(const char *data, size_t length) {
  lua_State *L = worker.parser;
  lua_settop(L, 0);
  if (memchr(data, 0, length)) return 0;
  /* vLLM produces UTF-8 JSON. Timing/count metadata does not require a
   * separate UTF-8 scan or a second copy into the Lua heap. */
  lua_rawgeti(L, LUA_REGISTRYINDEX, worker.decoder);
  lua_pushlstring(L, data, length);
  return !lua_pcall(L, 1, 1, 0);
}
static void record_weighted(struct group *g, unsigned metric, uint64_t elapsed, uint64_t tokens) {
  unsigned lo = 0, hi = schemas[metric].n;
  struct histogram *h = &g->hist[metric];
  double interval = (double)elapsed / tokens;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    if (interval > schemas[metric].bounds[mid])
      lo = mid + 1;
    else
      hi = mid;
  }
  h->count += tokens;
  h->sum += elapsed;
  h->buckets[hi] += tokens;
}
static void record(struct group *g, unsigned metric, uint64_t elapsed) {
  record_weighted(g, metric, elapsed, 1);
}
static void disable(struct observation *o) {
  if (!atomic_exchange(&o->dead, 1)) {
    o->owner->errors++;
  }
}
static struct group *get_group(const char *key, const char *json, size_t length) {
  const char *fields[] = {"model_name", "ReplicaId", "engine_worker_id", "engine"};
  lua_State *L = worker.parser;
  struct group *g;
  int n = 0;
  for (g = worker.groups; g; g = g->next)
    if (!strcmp(g->key, key)) return g;
  if (worker.group_count >= MAX_GROUPS || !decode(json, length) || !is_object(L, 1))
    return NULL;
  lua_pushnil(L);
  while (lua_next(L, 1)) {
    n++;
    lua_pop(L, 1);
  }
  if (n != 4) return NULL;
  g = calloc(1, sizeof(*g));
  if (!g) return NULL;
  for (unsigned i = 0; i < 4; i++) {
    size_t size;
    const char *val;
    lua_getfield(L, 1, fields[i]);
    if (lua_type(L, -1) != LUA_TSTRING) goto fail;
    val = lua_tolstring(L, -1, &size);
    if (memchr(val, 0, size)) goto fail;
    g->labels[i] = strndup(val, size);
    lua_pop(L, 1);
    if (!g->labels[i]) goto fail;
  }
  g->key = strdup(key);
  if (!g->key) goto fail;
  g->next = worker.groups;
  worker.groups = g;
  worker.group_count++;
  lua_settop(L, 0);
  return g;
fail:
  for (unsigned i = 0; i < 4; i++) free(g->labels[i]);
  free(g);
  lua_settop(L, 0);
  return NULL;
}
static void token_event(struct observation *o,
                        const char *data,
                        size_t length,
                        uint64_t now) {
  lua_State *L = worker.parser;
  int valid;
  if (!length || (length == 6 && !memcmp(data, "[DONE]", 6))) return;
  if (!decode(data, length) || !is_object(L, 1)) goto done;
  lua_getfield(L, 1, "choices");
  if (!lua_istable(L, 2) || !is_array(L, 2) || !lua_rawlen(L, 2)) goto done;
  if (o->last == LUA_NOREF) {
    lua_newtable(L);
    o->last = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, o->last); /* per-choice state at 3 */
  for (size_t i = 1; i <= lua_rawlen(L, 2); i++) {
    uint64_t previous;
    lua_settop(L, 3);
    lua_rawgeti(L, 2, i); /* choice at 4 */
    if (!is_object(L, 4)) continue;
    lua_getfield(L, 4, "ray_token_count");
    lua_Integer count = lua_tointegerx(L, 5, &valid);
    if (!valid || count < 0) { disable(o); break; }
    lua_pop(L, 1);
    uint64_t tokens = count;
    if (!tokens) continue;
    lua_getfield(L, 4, "index"); /* index at 5 */
    if (lua_isnil(L, 5)) { lua_pop(L, 1); lua_pushinteger(L, 0); }
    lua_pushvalue(L, 5);
    lua_rawget(L, 3); /* previous timestamp at 6 */
    int first = lua_isnil(L, 6);
    previous = first ? o->started : (uint64_t)lua_tointeger(L, 6);
    if (now < previous) { disable(o); break; }
    /* The event carries generated tokens since the preceding emitted choice,
     * including suppressed engine yields. Normalize
     * the HAProxy arrival interval by N and add N identical observations,
     * without an O(N) loop or rounding fractional microseconds. The first
     * positive-token event contributes TTFT only, like Dynamo's first yield. */
    record_weighted(o->group, first ? 0 : 1, now - previous, first ? 1 : tokens);
    lua_pushvalue(L, 5);
    lua_pushinteger(L, now);
    lua_rawset(L, 3);
  }
done:
  lua_settop(L, 0);
}
/* Compact data: lines in place; keep multiline SSE semantics and ignore event,
 * id, comments and terminal/role/usage chunks. */
static void complete_event(struct observation *o, uint64_t now) {
  size_t read = 0, write = 0;
  int lines = 0;
  while (read < o->used) {
    const char *newline = memchr(o->buffer + read, '\n', o->used - read);
    size_t end = newline ? (size_t)(newline - o->buffer) : o->used;
    if (end - read >= 5 && !memcmp(o->buffer + read, "data:", 5)) {
      size_t begin = read + 5;
      while (begin < end && isspace((unsigned char)o->buffer[begin])) begin++;
      if (lines++) o->buffer[write++] = '\n';
      memmove(o->buffer + write, o->buffer + begin, end - begin);
      write += end - begin;
    }
    read = end + 1;
  }
  token_event(o, o->buffer, write, now);
  o->used = 0;
}
static int append_bytes(struct observation *o, const char *data, size_t length) {
  if (length > MAX_PAYLOAD - o->used) {
    disable(o);
    return 0;
  }
  if (o->used + length > o->capacity) {
    size_t cap = o->capacity ? o->capacity : 512;
    char *buffer;
    while (cap < o->used + length) cap *= 2;
    if (cap > MAX_PAYLOAD) cap = MAX_PAYLOAD;
    if (worker.bytes + cap - o->capacity > MAX_BYTES) {
      disable(o);
      return 0;
    }
    buffer = realloc(o->buffer, cap);
    if (!buffer) {
      disable(o);
      return 0;
    }
    worker.bytes += cap - o->capacity;
    o->capacity = cap;
    o->buffer = buffer;
  }
  if (length) memcpy(o->buffer + o->used, data, length);
  o->used += length;
  return 1;
}
static void feed(struct observation *o, const char *data, size_t length, uint64_t now) {
  if (o->dead || !o->group || o->ended) return;
  while (length && !o->dead) {
    if (o->pending_cr) {
      o->pending_cr = 0;
      if (*data != '\n' && !append_bytes(o, "\r", 1)) return;
    }
    const char *newline = memchr(data, '\n', length);
    size_t prefix = newline ? (size_t)(newline - data) : length;
    const char *cr = memchr(data, '\r', prefix);
    if (cr) {
      size_t bytes = cr - data;
      if (!append_bytes(o, data, bytes)) return;
      data += bytes + 1;
      length -= bytes + 1;
      o->pending_cr = 1;
      continue;
    }
    size_t bytes = prefix + (newline != NULL);
    if (!append_bytes(o, data, bytes)) return;
    data += bytes;
    length -= bytes;
    if (newline && o->used >= 2 && o->buffer[o->used - 2] == '\n')
      complete_event(o, now);
  }
}
static void finish(struct observation *o, uint64_t now, int has_router, uint64_t router) {
  if (!o->group || o->ended) return;
  o->ended = 1;
  if (now < o->started || (has_router && router > now - o->started)) {
    disable(o);
    return;
  }
  record(o->group, 3, now - o->started);
  if (has_router) record(o->group, 2, router);
}
static void release(struct observation *o) {
  if (o->last != LUA_NOREF) luaL_unref(worker.parser, LUA_REGISTRYINDEX, o->last);
  worker.bytes -= o->capacity;
  free(o->buffer);
  o->buffer = NULL;
  o->last = LUA_NOREF;
  o->capacity = 0;
}

/* One network producer and one parser consumer per ring. Retirement sequences
 * keep detached requests alive until every captured fragment has been parsed.
 * Final destruction always occurs on the parser thread owning Lua registry refs. */
static void drop_reference(struct observation *o) {
  struct background *bg = o->owner->background;
  struct observation *previous;
  if (!bg) {
    /* Test handles may be collected after per-thread parser shutdown. */
    free(o->buffer);
    free(o);
    return;
  }
  o->retire_sequence = atomic_load_explicit(&bg->head, memory_order_relaxed);
  previous = atomic_load(&bg->garbage);
  do {
    o->gc_next = previous;
  } while (!atomic_compare_exchange_weak(&bg->garbage, &previous, o));
}
static void collect_garbage(struct background *bg) {
  struct observation *o = atomic_exchange(&bg->garbage, NULL);
  while (o) {
    struct observation *next = o->gc_next;
    o->gc_next = bg->deferred;
    bg->deferred = o;
    o = next;
  }
  struct observation **link = &bg->deferred;
  uint64_t tail = atomic_load_explicit(&bg->tail, memory_order_relaxed);
  while (*link) {
    o = *link;
    if (o->retire_sequence <= tail) {
      *link = o->gc_next;
      release(o);
      free(o);
    } else
      link = &o->gc_next;
  }
}
static void *parse_background(void *argument) {
  struct background *bg = argument;
  const struct timespec idle = {.tv_nsec = 500000};
  pthread_setname_np(pthread_self(), "ray-stream-metr");
  if (init_worker()) {
    atomic_store(&bg->ready, -1);
    return NULL;
  }
  atomic_store(&bg->ready, 1);
  while (1) {
    uint64_t tail = atomic_load_explicit(&bg->tail, memory_order_relaxed);
    collect_garbage(bg);
    if (tail == atomic_load_explicit(&bg->head, memory_order_acquire)) {
      if (atomic_load(&bg->stop)) break;
      nanosleep(&idle, NULL);
      continue;
    }
    struct item *item = &bg->items[tail % bg->capacity];
#ifdef RAY_STREAM_METRICS_UNIT_TEST
    unsigned delay = atomic_load(&bg->test_delay_us);
    if (delay && !atomic_load(&item->observation->dead)) {
      struct timespec pause = {.tv_sec = delay / 1000000,
                               .tv_nsec = (delay % 1000000) * 1000};
      nanosleep(&pause, NULL);
    }
#endif
    pthread_mutex_lock(&bg->owner->hist_lock);
    if (item->ending)
      finish(item->observation, item->now, item->has_router, item->router);
    else
      feed(item->observation, item->body, item->length, item->now);
    pthread_mutex_unlock(&bg->owner->hist_lock);
    if (item->allocated) free(item->body);
    atomic_store_explicit(
        &bg->consumed_bytes,
        atomic_load_explicit(&bg->consumed_bytes, memory_order_relaxed) + item->length,
        memory_order_release);
    atomic_store_explicit(&bg->tail, tail + 1, memory_order_release);
  }
  collect_garbage(bg);
  lua_close(worker.parser);
  worker.parser = NULL;
  return NULL;
}
static int init_background(void) {
  struct background *bg;
  const struct timespec idle = {.tv_nsec = 100000};
  size_t capacity = MAX_PENDING;
  const char *setting = getenv("RAY_SERVE_STREAM_METRICS_QUEUE_SLOTS");
  if (worker.background) return 0;
  if (setting) {
    char *end;
    errno = 0;
    unsigned long long value = strtoull(setting, &end, 10);
    if (!isdigit((unsigned char)*setting) || *end || errno || !value ||
        value > (SIZE_MAX - sizeof(*bg) - 63) / sizeof(struct item)) return -1;
    capacity = value;
  }
  size_t allocation = (sizeof(*bg) + capacity * sizeof(struct item) + 63) & ~(size_t)63;
  bg = aligned_alloc(64, allocation);
  if (bg) memset(bg, 0, allocation);
  if (!bg || pthread_mutex_init(&worker.hist_lock, NULL)) {
    free(bg);
    return -1;
  }
  atomic_init(&bg->head, 0);
  atomic_init(&bg->tail, 0);
  atomic_init(&bg->produced_bytes, 0);
  atomic_init(&bg->consumed_bytes, 0);
  atomic_init(&bg->garbage, NULL);
  atomic_init(&bg->ready, 0);
  atomic_init(&bg->stop, 0);
#ifdef RAY_STREAM_METRICS_UNIT_TEST
  atomic_init(&bg->test_delay_us, 0);
#endif
  bg->owner = &worker;
  bg->capacity = capacity;
  bg->allocation = allocation;
  worker.background = bg;
  if (pthread_create(&bg->thread, NULL, parse_background, bg)) goto fail;
  while (!atomic_load(&bg->ready)) nanosleep(&idle, NULL);
  if (atomic_load(&bg->ready) == 1) return 0;
  pthread_join(bg->thread, NULL);
fail:
  pthread_mutex_destroy(&worker.hist_lock);
  worker.background = NULL;
  free(bg);
  return -1;
}
static struct item *prepare_item(struct observation *o,
                                 size_t capacity,
                                 uint64_t now,
                                 int ending,
                                 int has_router,
                                 uint64_t router) {
  struct background *bg = o->owner->background;
  uint64_t head = atomic_load_explicit(&bg->head, memory_order_relaxed);
  uint64_t pending = atomic_load_explicit(&bg->produced_bytes, memory_order_relaxed) -
                     atomic_load_explicit(&bg->consumed_bytes, memory_order_acquire);
  uint64_t pending_items = head - atomic_load_explicit(&bg->tail, memory_order_acquire);
  if (pending_items >= bg->capacity ||
      capacity > MAX_BYTES - pending) {
    disable(o);
    return NULL;
  }
  struct item *item = &bg->items[head % bg->capacity];
  if (pending_items + 1 > bg->high_water) bg->high_water = pending_items + 1;
  item->observation = o;
  item->now = now;
  item->ending = ending;
  item->has_router = has_router;
  item->router = router;
  item->allocated = capacity > INLINE_FRAGMENT;
  item->body = item->allocated ? malloc(capacity) : item->inline_body;
  if (!item->body) {
    disable(o);
    return NULL;
  }
  return item;
}
static void publish_item(struct observation *o, struct item *item, size_t length) {
  struct background *bg = o->owner->background;
  uint64_t head = atomic_load_explicit(&bg->head, memory_order_relaxed);
  item->length = length;
  atomic_store_explicit(
      &bg->produced_bytes,
      atomic_load_explicit(&bg->produced_bytes, memory_order_relaxed) + length,
      memory_order_release);
  atomic_store_explicit(&bg->head, head + 1, memory_order_release);
}
static int enqueue(struct observation *o,
                   const char *body,
                   size_t length,
                   uint64_t now,
                   int ending,
                   int has_router,
                   uint64_t router) {
  struct item *item = prepare_item(o, length, now, ending, has_router, router);
  if (!item) return 0;
  if (length) memcpy(item->body, body, length);
  publish_item(o, item, length);
  return 1;
}
static struct observation *allocate_observation(void) {
  struct observation *o = aligned_alloc(64, sizeof(*o));
  if (o) {
    memset(o, 0, sizeof(*o));
    atomic_init(&o->dead, 0);
  }
  return o;
}
#ifndef RAY_STREAM_METRICS_UNIT_TEST
static int attach(struct stream *s, struct filter *f) {
  struct observation *o = allocate_observation();
  if (!o) {
    worker.errors++;
    return 0;
  }
  o->owner = &worker;
  o->last = LUA_NOREF;
  o->started = clock_us();
  f->ctx = o;
  worker.active++;
  return 1;
}
static void detach(struct stream *s, struct filter *f) {
  struct observation *o = f->ctx;
  if (o) {
    drop_reference(o);
    f->ctx = NULL;
    worker.active--;
  }
}
static int start_analyze(struct stream *s, struct filter *f, struct channel *chn) {
  if (!(chn->flags & CF_ISRESP)) ((struct observation *)f->ctx)->started = clock_us();
  return 1;
}
static int headers(struct stream *s, struct filter *f, struct http_msg *msg) {
  struct observation *o = f->ctx;
  struct htx *htx;
  struct http_hdr_ctx ctx = {0};
  struct sample sample = {.px = s->be, .sess = s->sess, .strm = s};
  char key[MAX_TAGS + 1];
  int found = 0;
  if (!(msg->chn->flags & CF_ISRESP)) return 1;
  htx = htxbuf(&msg->chn->buf);
  while (http_find_header(htx, ist(TAG_HEADER), &ctx, 1)) {
    if (!found && ctx.value.len <= MAX_TAGS) {
      memcpy(key, ctx.value.ptr, ctx.value.len);
      key[ctx.value.len] = 0;
      found = 1;
    }
    http_remove_header(htx, &ctx);
  }
  if (!found || http_get_stline(htx)->info.res.status != 200) return 1;
  memset(&ctx, 0, sizeof(ctx));
  if (!http_find_header(htx, ist("content-type"), &ctx, 1) || ctx.value.len < 17 ||
      memcmp(ctx.value.ptr, "text/event-stream", 17))
    return 1;
  if (!vars_get_by_name("txn.via_ingress_request_router",
                        sizeof("txn.via_ingress_request_router") - 1,
                        &sample,
                        NULL) ||
      !sample.data.u.sint)
    return 1;
  if (!(o->group = get_group(key, key, strlen(key)))) {
    disable(o);
    return 1;
  }
  register_data_filter(s, msg->chn, f);
  return 1;
}
static int payload(struct stream *s,
                   struct filter *f,
                   struct http_msg *msg,
                   unsigned offset,
                   unsigned len) {
  struct observation *o = f->ctx;
  struct htx *htx = htxbuf(&msg->chn->buf);
  struct htx_blk *blk;
  unsigned available = len;
  size_t used = 0;
  struct item *item;
  uint64_t now = clock_us();
  if (!available) return 0;
  if (!(msg->chn->flags & CF_ISRESP) || o->dead || !o->group) return len;
  item = prepare_item(o, available, now, 0, 0, 0);
  if (!item) return available;
  char *copy = item->body;
  for (blk = htx_get_first_blk(htx); blk && len; blk = htx_get_next_blk(htx, blk)) {
    uint32_t size = htx_get_blksz(blk);
    struct ist val;
    if (offset >= size) {
      offset -= size;
      continue;
    }
    val = istadv(htx_get_blk_value(htx, blk), offset);
    offset = 0;
    val = isttrim(val, len);
    len -= val.len;
    if (htx_get_blk_type(blk) == HTX_BLK_DATA) {
      memcpy(copy + used, val.ptr, val.len);
      used += val.len;
    }
  }
  if (copy) {
    if (used)
      publish_item(o, item, used);
    else if (item->allocated)
      free(copy);
  }
  /* Filters consume all available bytes, without changing forwarding. */
  return available;
}
static int http_end(struct stream *s, struct filter *f, struct http_msg *msg) {
  struct sample smp = {.px = s->be, .sess = s->sess, .strm = s};
  int found;
  if (!(msg->chn->flags & CF_ISRESP)) return 1;
  found = vars_get_by_name("txn.ingress_request_router_latency_us",
                           sizeof("txn.ingress_request_router_latency_us") - 1,
                           &smp,
                           NULL);
  if (((struct observation *)f->ctx)->group)
    enqueue(f->ctx, NULL, 0, clock_us(), 1,
            found && smp.data.u.sint >= 0, found ? smp.data.u.sint : 0);
  return 1;
}
#endif
static int per_thread(struct proxy *px, struct flt_conf *conf) {
  if (init_worker()) return -1;
  if (init_background()) return -1;
  worker.background->users++;
#ifndef RAY_STREAM_METRICS_UNIT_TEST
  if (!worker.export_job) {
    _HA_ATOMIC_INC(&jobs);
    worker.export_job = 1;
  }
#endif
  return 0;
}
static void deinit_thread(struct proxy *px, struct flt_conf *conf) {
  struct background *bg = worker.background;
  if (!bg || (bg->users && --bg->users)) return;
  atomic_store(&bg->stop, 1);
  pthread_join(bg->thread, NULL);
  pthread_mutex_destroy(&worker.hist_lock);
  worker.background = NULL;
  free(bg);
}
#ifndef RAY_STREAM_METRICS_UNIT_TEST
static struct flt_ops ops = {.init_per_thread = per_thread,
                             .deinit_per_thread = deinit_thread,
                             .attach = attach,
                             .detach = detach,
                             .channel_start_analyze = start_analyze,
                             .http_headers = headers,
                             .http_payload = payload,
                             .http_end = http_end};
static int parse(char **args,
                 int *cur,
                 struct proxy *px,
                 struct flt_conf *conf,
                 char **err,
                 void *private) {
  (*cur)++;
  conf->id = "stream metrics";
  conf->ops = &ops;
  conf->flags |= FLT_CFG_FL_HTX;
  return 0;
}
static int configure(lua_State *L) {
  for (unsigned m = 0; m < 4; m++) {
    lua_rawgeti(L, 1, m + 1);
    lua_getfield(L, -1, "bounds");
    schemas[m].n = lua_rawlen(L, -1);
    if (schemas[m].n > MAX_BUCKETS) return luaL_error(L, "too many histogram buckets");
    for (unsigned b = 0; b < schemas[m].n; b++) {
      lua_rawgeti(L, -1, b + 1);
      schemas[m].bounds[b] = luaL_checkinteger(L, -1);
      lua_pop(L, 1);
    }
    lua_pop(L, 2);
  }
  return 0;
}

/* This private exporter listener must survive soft-stop until final reports
 * are acknowledged. Like HAProxy's master CLI, it cannot alone keep a process
 * alive. Per-thread export jobs keep it alive only until bounded final flush. */
static int export_bind(char **args, int cur, struct proxy *px,
                       struct bind_conf *bind, char **err) {
  struct listener *listener;
  bind->options |= BC_O_NOSTOP;
  list_for_each_entry(listener, &bind->listeners, by_bind)
    _HA_ATOMIC_INC(&unstoppable_jobs);
  return 0;
}
static struct flt_kw_list keywords = {"RAY", { }, {
  {"ray_stream_metrics", parse, NULL}, {NULL, NULL, NULL}
}};
static struct bind_kw_list bind_keywords = {"RAY", { }, {
  {"ray-metrics-export", export_bind, 0}, {NULL, NULL, 0}
}};
__attribute__((constructor)) static void register_keywords(void) {
  flt_register_keywords(&keywords);
  bind_register_keywords(&bind_keywords);
}
#endif
static int snapshot(lua_State *L) {
  const char *keys[] = {"model_name", "ReplicaId", "engine_worker_id", "engine"};
  if (worker.background && pthread_mutex_trylock(&worker.hist_lock)) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  for (struct group *g = worker.groups; g; g = g->next) {
    lua_newtable(L);
    lua_newtable(L);
    for (unsigned i = 0; i < 4; i++) {
      lua_pushstring(L, g->labels[i]);
      lua_setfield(L, -2, keys[i]);
    }
    lua_setfield(L, -2, "tags");
    lua_newtable(L);
    for (unsigned m = 0; m < 4; m++) {
      lua_newtable(L);
      lua_pushinteger(L, g->hist[m].count);
      lua_setfield(L, -2, "count");
      lua_pushinteger(L, g->hist[m].sum);
      lua_setfield(L, -2, "sum_us");
      lua_newtable(L);
      for (unsigned b = 0; b <= schemas[m].n; b++) {
        lua_pushinteger(L, g->hist[m].buckets[b]);
        lua_rawseti(L, -2, b + 1);
      }
      lua_setfield(L, -2, "buckets");
      lua_rawseti(L, -2, m + 1);
    }
    lua_setfield(L, -2, "histograms");
    lua_setfield(L, -2, g->key);
  }
  lua_pushinteger(L, worker.errors);
  lua_pushinteger(L, worker.group_count);
  if (worker.background) pthread_mutex_unlock(&worker.hist_lock);
  return 3;
}
#ifndef RAY_STREAM_METRICS_UNIT_TEST
static int status(lua_State *L) {
  struct background *bg = worker.background;
  lua_newtable(L);
#define VALUE(key, value) lua_pushinteger(L, value); lua_setfield(L, -2, key)
  VALUE("stopping", _HA_ATOMIC_LOAD(&stopping));
  VALUE("now_us", clock_us());
  VALUE("active", worker.active);
  VALUE("pending", bg ? atomic_load(&bg->head) - atomic_load(&bg->tail) : 0);
  VALUE("slots", bg ? bg->capacity : 0);
  VALUE("allocation_bytes", bg ? bg->allocation : 0);
  VALUE("high_water", bg ? bg->high_water : 0);
  VALUE("errors", atomic_load(&worker.errors));
#undef VALUE
  return 1;
}
static int release_export(lua_State *L) {
  if (worker.export_job) {
    worker.export_job = 0;
    _HA_ATOMIC_DEC(&jobs);
  }
  return 0;
}
int luaopen_ray_stream_metrics(lua_State *L) {
  if (strcmp(haproxy_version, RAY_HAPROXY_ABI))
    return luaL_error(L, "stream observer must use its matching ray-haproxy binary");
  luaopen_ray_stream_metrics_impl(L);
  lua_pushcfunction(L, configure);
  lua_setfield(L, -2, "configure");
  lua_pushcfunction(L, snapshot);
  lua_setfield(L, -2, "snapshot");
  lua_pushcfunction(L, status);
  lua_setfield(L, -2, "status");
  lua_pushcfunction(L, release_export);
  lua_setfield(L, -2, "release_export");
  return 1;
}
#endif
