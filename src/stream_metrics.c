// Native HTTP transaction (HTX) observer for ray-haproxy 2.8.25, loaded through
// Lua's C ABI. HTX is HAProxy's internal HTTP message representation: typed
// blocks containing headers, body data, trailers, and end markers.
//
// The observer runs inline on the network thread that forwards each response.
// It scans response body bytes in place for two fixed markers emitted by Ray
// Serve LLM's native vLLM serializer:
//
//   "choices":[{"index":I      the choice index of a streamed chunk (native)
//   "ray_token_count":N        tokens generated since that choice's last chunk
//
// No bytes are copied and no JSON is decoded per event. Split markers and
// numbers are carried across fragments in a few bytes of per-stream state.
// Histograms are per network thread; the same thread's Lua task exports them,
// so recording and snapshots never race and need no locks. Built alongside the
// matching HAProxy binary by ray-haproxy's wheel pipeline.
#define _GNU_SOURCE
#include <haproxy/filters.h>
#include <haproxy/global.h>
#include <haproxy/http_htx.h>
#include <haproxy/htx.h>
#include <haproxy/listener.h>
#include <haproxy/sample-t.h>
#include <haproxy/stream-t.h>
#include <haproxy/vars.h>
#include <lauxlib.h>
#include <lua.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern char haproxy_version[];

#define MAX_TAGS 2048     // Longest accepted identity header value, in bytes.
#define MAX_GROUPS 4096   // Label groups per network thread.
#define MAX_BUCKETS 128   // Finite histogram boundaries per metric.
#define MAX_CHOICES 4096  // Highest tracked choice index + 1 per stream.
#define MAX_DIGITS 9      // Longest accepted index or token count.
#define MAX_BOUND_US 10000000000ULL  // Largest bucket bound: 10^4 s.
#define INLINE_CHOICES 2             // Choice clocks stored without allocation.
#define NUM_METRICS 4
#define TAG_HEADER "x-ray-llm-metric-tags"

enum metric { TTFT = 0, ITL = 1, ROUTING = 2, REQUEST = 3 };

// Markers. The index marker is vLLM's native serialization of a stream chunk's
// first (and only) choice; the count marker is added by Ray Serve LLM.
static const char INDEX_MARKER[] = "\"choices\":[{\"index\":";
static const char COUNT_MARKER[] = "\"ray_token_count\":";
#define INDEX_MARKER_LEN (sizeof(INDEX_MARKER) - 1)
#define COUNT_MARKER_LEN (sizeof(COUNT_MARKER) - 1)
#define MAX_MARKER_LEN INDEX_MARKER_LEN

extern int luaopen_cjson(lua_State *lua);

// Cumulative observations for one metric; snapshots do not reset these values.
struct histogram {
  uint64_t count;  // Observation count; token-weighted for ITL.
  uint64_t sum;    // Sum of observed durations in microseconds.
  // Noncumulative range counts; index bucket_count is the overflow range.
  uint64_t buckets[MAX_BUCKETS + 1];
};

// Bucket boundaries shared by all threads, configured once at load.
struct schema {
  unsigned bucket_count;         // Number of valid finite upper boundaries.
  uint64_t bounds[MAX_BUCKETS];  // Ascending inclusive upper bounds, in us.
};

// One identity (header value) on one network thread.
struct group {
  struct group *next;  // Next group of the owning thread.
  char *key;           // Owned identity JSON; decoded by Lua at export.
  struct histogram hist[NUM_METRICS];
};

// Thread-local state. Written by filter callbacks and read by the Lua exporter
// task, both of which run only on this HAProxy thread.
struct worker {
  struct group *groups;  // Label groups; NULL initially.
  unsigned group_count;  // Number of groups.
  uint64_t errors;       // Collection failures (never affect forwarding).
  unsigned active;       // Attached streams on this thread.
  int export_job;        // Whether this thread holds an HAProxy job.
};

// Scanner phases: find a marker, then read the decimal number that follows.
enum phase { SEEK_INDEX = 0, READ_INDEX, SEEK_COUNT, READ_COUNT };

// Per-stream state.
struct observation {
  struct group *group;  // Label group; NULL until headers enable capture.
  uint64_t started;     // Monotonic stream start, in us.
  uint64_t *last;       // Per-choice last token time in us; 0 = no token yet.
  uint32_t choices;     // Capacity of last.
  uint32_t index;       // Choice index of the chunk being scanned.
  uint32_t value;       // Number being read.
  uint8_t phase;        // enum phase.
  uint8_t digits;       // Digits read into value.
  uint8_t carry_len;    // Bytes in carry.
  uint8_t dead;         // Collection disabled after an error.
  char carry[MAX_MARKER_LEN];  // Fragment tail that may begin a marker.
  uint64_t inline_last[INLINE_CHOICES];
};

static struct schema schemas[NUM_METRICS];
static __thread struct worker worker;

/* Adds tokens observations of elapsed/tokens us to group's metric; the sum
 * grows by elapsed. tokens must be > 0.
 */
static void record_weighted(struct group *group, unsigned metric,
                            uint64_t elapsed, uint64_t tokens) {
  const struct schema *schema = &schemas[metric];
  struct histogram *histogram = &group->hist[metric];
  unsigned lo = 0, hi = schema->bucket_count;
  // Compare elapsed / tokens with each bound exactly, without division.
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    if (elapsed > schema->bounds[mid] * tokens)
      lo = mid + 1;
    else
      hi = mid;
  }
  histogram->count += tokens;
  histogram->sum += elapsed;
  histogram->buckets[hi] += tokens;
}

/* Disables collection for obs and counts one error; forwarding continues. */
static void disable(struct observation *obs) {
  if (!obs->dead) {
    obs->dead = 1;
    worker.errors++;
  }
}

/* Finds key's group or creates it. Returns NULL for an empty key or one with
 * a NUL byte (stored keys must be exactly length bytes), at the group limit,
 * or on allocation failure.
 */
static struct group *get_group(const char *key, size_t length) {
  struct group *group;
  if (!length || memchr(key, 0, length)) return NULL;
  for (group = worker.groups; group; group = group->next)
    if (!strncmp(group->key, key, length) && !group->key[length]) return group;
  if (worker.group_count >= MAX_GROUPS) return NULL;
  group = calloc(1, sizeof(*group));
  if (!group) return NULL;
  group->key = strndup(key, length);
  if (!group->key) {
    free(group);
    return NULL;
  }
  group->next = worker.groups;
  worker.groups = group;
  worker.group_count++;
  return group;
}

/* Records tokens new tokens of choice index arriving at now (us). The first
 * token-bearing chunk records TTFT only; as in Dynamo's frontend metrics, any
 * further tokens it carries add no ITL. Later chunks record
 * (now - previous) / tokens, weighted by tokens. Zero-token chunks (roles,
 * finish markers) do not advance the clock.
 */
static void observe(struct observation *obs, uint32_t index, uint32_t tokens,
                    uint64_t now) {
  uint64_t previous;
  if (!tokens) return;
  if (index >= obs->choices) {
    uint32_t capacity = obs->choices;
    uint64_t *grown;
    if (index >= MAX_CHOICES) {
      disable(obs);
      return;
    }
    while (capacity <= index) capacity *= 2;
    grown = calloc(capacity, sizeof(*grown));
    if (!grown) {
      disable(obs);
      return;
    }
    memcpy(grown, obs->last, obs->choices * sizeof(*grown));
    if (obs->last != obs->inline_last) free(obs->last);
    obs->last = grown;
    obs->choices = capacity;
  }
  previous = obs->last[index];
  if (!previous) {
    if (now < obs->started) {
      disable(obs);
      return;
    }
    record_weighted(obs->group, TTFT, now - obs->started, 1);
  } else {
    if (now < previous) {
      disable(obs);
      return;
    }
    record_weighted(obs->group, ITL, now - previous, tokens);
  }
  obs->last[index] = now;
}

/* Stores the longest suffix of data[0..length) that is a proper prefix of
 * marker into obs->carry, so a marker split across fragments is found later.
 */
static void save_carry(struct observation *obs, const char *data, size_t length,
                       const char *marker, size_t marker_len) {
  size_t k = length < marker_len - 1 ? length : marker_len - 1;
  for (; k; k--)
    if (!memcmp(data + length - k, marker, k)) break;
  memcpy(obs->carry, data + length - k, k);
  obs->carry_len = k;
}

/* Scans length body bytes of one fragment arriving at now (us). */
static void feed(struct observation *obs, const char *data, size_t length,
                 uint64_t now) {
  while (length && !obs->dead) {
    if (obs->phase == SEEK_INDEX || obs->phase == SEEK_COUNT) {
      const char *marker =
          obs->phase == SEEK_INDEX ? INDEX_MARKER : COUNT_MARKER;
      size_t marker_len =
          obs->phase == SEEK_INDEX ? INDEX_MARKER_LEN : COUNT_MARKER_LEN;
      const char *found;
      if (obs->carry_len) {
        // A marker may start in the previous fragment's tail. Search the tail
        // joined with just enough new bytes to complete one.
        char joined[2 * MAX_MARKER_LEN];
        size_t take = length < marker_len - 1 ? length : marker_len - 1;
        size_t joined_len = obs->carry_len + take;
        memcpy(joined, obs->carry, obs->carry_len);
        memcpy(joined + obs->carry_len, data, take);
        found = memmem(joined, joined_len, marker, marker_len);
        if (found && found < joined + obs->carry_len) {
          size_t used = (size_t)(found - joined) + marker_len - obs->carry_len;
          data += used;
          length -= used;
          obs->carry_len = 0;
          obs->phase++;
          obs->value = obs->digits = 0;
          continue;
        }
        obs->carry_len = 0;
        if (take == length) {
          // Every new byte is part of the joined tail; keep any marker prefix.
          save_carry(obs, joined, joined_len, marker, marker_len);
          return;
        }
      }
      found = memmem(data, length, marker, marker_len);
      if (!found) {
        save_carry(obs, data, length, marker, marker_len);
        return;
      }
      length -= (size_t)(found - data) + marker_len;
      data = found + marker_len;
      obs->phase++;
      obs->value = obs->digits = 0;
      continue;
    }
    // READ_INDEX or READ_COUNT: the number may continue in a later fragment.
    while (length && (unsigned char)(*data - '0') < 10) {
      if (++obs->digits > MAX_DIGITS) {
        disable(obs);
        return;
      }
      obs->value = obs->value * 10 + (uint32_t)(*data - '0');
      data++;
      length--;
    }
    if (!length) return;
    if (!obs->digits) {
      disable(obs);
      return;
    }
    if (obs->phase == READ_INDEX) {
      obs->index = obs->value;
      obs->phase = SEEK_COUNT;
    } else {
      observe(obs, obs->index, obs->value, now);
      obs->phase = SEEK_INDEX;
    }
  }
}

/* Records request latency at now (us) and, if has_router, the router's
 * measured round trip (us).
 */
static void finish(struct observation *obs, uint64_t now, int has_router,
                   uint64_t router) {
  if (obs->dead) return;
  if (now < obs->started || (has_router && router > now - obs->started)) {
    disable(obs);
    return;
  }
  record_weighted(obs->group, REQUEST, now - obs->started, 1);
  if (has_router) record_weighted(obs->group, ROUTING, router, 1);
}

/* Returns zeroed per-stream state started at now (us), or NULL. */
static struct observation *new_observation(uint64_t now) {
  struct observation *obs = calloc(1, sizeof(*obs));
  if (!obs) return NULL;
  obs->started = now;
  obs->last = obs->inline_last;
  obs->choices = INLINE_CHOICES;
  return obs;
}

/* Releases per-stream state. */
static void free_observation(struct observation *obs) {
  if (obs->last != obs->inline_last) free(obs->last);
  free(obs);
}

#ifndef RAY_STREAM_METRICS_UNIT_TEST
/* Returns monotonic microseconds. */
static uint64_t clock_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Attaches per-stream state, timestamped when the stream starts. */
static int attach(struct stream *stream, struct filter *filter) {
  struct observation *obs = new_observation(clock_us());
  if (!obs) {
    worker.errors++;
    return 0;  // Ignore the filter for this stream; forwarding continues.
  }
  filter->ctx = obs;
  worker.active++;
  return 1;
}

/* Releases the stream's state. */
static void detach(struct stream *stream, struct filter *filter) {
  if (filter->ctx) {
    free_observation(filter->ctx);
    filter->ctx = NULL;
    worker.active--;
  }
}

/* Removes the internal identity header from responses and enables body
 * observation only for successful, tagged SSE responses that passed through
 * the ingress request router.
 */
static int headers(struct stream *stream, struct filter *filter,
                   struct http_msg *msg) {
  struct observation *obs = filter->ctx;
  struct htx *htx;
  struct htx_sl *sl;
  struct http_hdr_ctx ctx = {0};
  struct sample sample = {
      .px = stream->be, .sess = stream->sess, .strm = stream};
  char key[MAX_TAGS];
  size_t key_len = 0;
  int found = 0;
  if (!(msg->chn->flags & CF_ISRESP)) return 1;
  htx = htxbuf(&msg->chn->buf);
  while (http_find_header(htx, ist(TAG_HEADER), &ctx, 1)) {
    if (!found && ctx.value.len && ctx.value.len <= MAX_TAGS) {
      memcpy(key, ctx.value.ptr, ctx.value.len);
      key_len = ctx.value.len;
      found = 1;
    }
    http_remove_header(htx, &ctx);
  }
  sl = http_get_stline(htx);
  if (!found || !sl || sl->info.res.status != 200) return 1;
  memset(&ctx, 0, sizeof(ctx));
  if (!http_find_header(htx, ist("content-type"), &ctx, 1) ||
      ctx.value.len < 17 || memcmp(ctx.value.ptr, "text/event-stream", 17))
    return 1;
  if (!vars_get_by_name("txn.via_ingress_request_router",
                        sizeof("txn.via_ingress_request_router") - 1, &sample,
                        NULL) ||
      !sample.data.u.sint)
    return 1;
  if (!(obs->group = get_group(key, key_len))) {
    disable(obs);
    return 1;
  }
  register_data_filter(stream, msg->chn, filter);
  return 1;
}

/* Scans the response body bytes HAProxy is about to forward, in place, at
 * their arrival time. Always forwards everything that is available.
 */
static int payload(struct stream *stream, struct filter *filter,
                   struct http_msg *msg, unsigned offset, unsigned len) {
  struct observation *obs = filter->ctx;
  struct htx *htx;
  struct htx_blk *blk;
  unsigned remaining = len;
  uint64_t now;
  if (!len || obs->dead || !obs->group || !(msg->chn->flags & CF_ISRESP))
    return len;
  htx = htxbuf(&msg->chn->buf);
  now = clock_us();
  for (blk = htx_get_first_blk(htx); blk && remaining;
       blk = htx_get_next_blk(htx, blk)) {
    uint32_t size = htx_get_blksz(blk);
    struct ist value;
    if (offset >= size) {
      offset -= size;
      continue;
    }
    value = isttrim(istadv(htx_get_blk_value(htx, blk), offset), remaining);
    offset = 0;
    remaining -= value.len;
    if (htx_get_blk_type(blk) == HTX_BLK_DATA)
      feed(obs, value.ptr, value.len, now);
  }
  return len;
}

/* Records response completion and the ingress routing duration. */
static int http_end(struct stream *stream, struct filter *filter,
                    struct http_msg *msg) {
  struct observation *obs = filter->ctx;
  struct sample sample = {
      .px = stream->be, .sess = stream->sess, .strm = stream};
  int found;
  if (!(msg->chn->flags & CF_ISRESP) || !obs->group) return 1;
  found = vars_get_by_name("txn.ingress_request_router_latency_us",
                           sizeof("txn.ingress_request_router_latency_us") - 1,
                           &sample, NULL);
  finish(obs, clock_us(), found && sample.data.u.sint >= 0,
         found ? (uint64_t)sample.data.u.sint : 0);
  return 1;
}
#endif

#ifndef RAY_STREAM_METRICS_UNIT_TEST
/* Holds one HAProxy job per thread so a retiring process stays alive until
 * Lua completes its final export.
 */
static int per_thread(struct proxy *px, struct flt_conf *conf) {
  if (!worker.export_job) {
    _HA_ATOMIC_INC(&jobs);
    worker.export_job = 1;
  }
  return 0;
}

// Callback contract for the matching HAProxy 2.8.25 source:
// https://github.com/haproxy/haproxy/blob/5cf5d068355fd617905ae50f9a7e16c2898f3228/include/haproxy/filters-t.h#L159
// This interface uses internal structs and is version-coupled, not a stable
// cross-release ABI. Build this module against its matching HAProxy binary.
static struct flt_ops ops = {.init_per_thread = per_thread,
                             .attach = attach,
                             .detach = detach,
                             .http_headers = headers,
                             .http_payload = payload,
                             .http_end = http_end};

/* Registers the observer in the proxy's filter configuration. */
static int parse(char **args, int *cur, struct proxy *px, struct flt_conf *conf,
                 char **err, void *private) {
  (*cur)++;
  conf->id = "stream metrics";
  conf->ops = &ops;
  conf->flags |= FLT_CFG_FL_HTX;
  return 0;
}

/* Keeps the private exporter listener usable during soft-stop for the final
 * flush. Lua releases the export jobs afterwards.
 */
static int export_bind(char **args, int cur, struct proxy *px,
                       struct bind_conf *bind, char **err) {
  struct listener *listener;
  bind->options |= BC_O_NOSTOP;
  list_for_each_entry(listener, &bind->listeners, by_bind)
      _HA_ATOMIC_INC(&unstoppable_jobs);
  return 0;
}

static struct flt_kw_list keywords = {
    "RAY", {}, {{"ray_stream_metrics", parse, NULL}, {NULL, NULL, NULL}}};
static struct bind_kw_list bind_keywords = {
    "RAY", {}, {{"ray-metrics-export", export_bind, 0}, {NULL, NULL, 0}}};

/* Registers the filter and exporter bind keywords at module load. */
__attribute__((constructor)) static void register_keywords(void) {
  flt_register_keywords(&keywords);
  bind_register_keywords(&bind_keywords);
}
#endif

/* Reads four ordered histogram schemas from Lua's first argument, with bounds
 * in microseconds.
 */
static int configure(lua_State *lua) {
  struct schema parsed[NUM_METRICS];
  luaL_checktype(lua, 1, LUA_TTABLE);
  for (unsigned metric = 0; metric < NUM_METRICS; metric++) {
    lua_rawgeti(lua, 1, metric + 1);
    luaL_checktype(lua, -1, LUA_TTABLE);
    lua_getfield(lua, -1, "bounds");
    luaL_checktype(lua, -1, LUA_TTABLE);
    if (lua_rawlen(lua, -1) > MAX_BUCKETS)
      return luaL_error(lua, "too many histogram buckets");
    parsed[metric].bucket_count = lua_rawlen(lua, -1);
    for (unsigned bucket = 0; bucket < parsed[metric].bucket_count; bucket++) {
      lua_Integer bound;
      lua_rawgeti(lua, -1, bucket + 1);
      bound = luaL_checkinteger(lua, -1);
      // Positive, ascending and small enough that bound * tokens (< 10^9)
      // cannot overflow when bucketing.
      if (bound <= 0 || bound > MAX_BOUND_US ||
          (bucket && (uint64_t)bound <= parsed[metric].bounds[bucket - 1]))
        return luaL_error(lua, "invalid histogram bounds");
      parsed[metric].bounds[bucket] = (uint64_t)bound;
      lua_pop(lua, 1);
    }
    lua_pop(lua, 2);
  }
  memcpy(schemas, parsed, sizeof(schemas));
  return 0;
}

/* Pushes {key -> {histograms}} for this thread, its error count, and its
 * group count. Runs on the thread that records, so values are coherent.
 */
static int snapshot(lua_State *lua) {
  luaL_checkstack(lua, 8, "stream metrics snapshot");
  lua_newtable(lua);
  for (struct group *group = worker.groups; group; group = group->next) {
    lua_newtable(lua);
    for (unsigned metric = 0; metric < NUM_METRICS; metric++) {
      const struct histogram *histogram = &group->hist[metric];
      lua_newtable(lua);
      lua_pushinteger(lua, histogram->count);
      lua_setfield(lua, -2, "count");
      lua_pushinteger(lua, histogram->sum);
      lua_setfield(lua, -2, "sum_us");
      lua_newtable(lua);
      for (unsigned bucket = 0; bucket <= schemas[metric].bucket_count;
           bucket++) {
        lua_pushinteger(lua, histogram->buckets[bucket]);
        lua_rawseti(lua, -2, bucket + 1);
      }
      lua_setfield(lua, -2, "buckets");
      lua_rawseti(lua, -2, metric + 1);
    }
    lua_setfield(lua, -2, group->key);
  }
  lua_pushinteger(lua, worker.errors);
  lua_pushinteger(lua, worker.group_count);
  return 3;
}

#ifndef RAY_STREAM_METRICS_UNIT_TEST
/* Pushes this thread's retirement, active-stream and error status. */
static int status(lua_State *lua) {
  lua_newtable(lua);
  lua_pushinteger(lua, _HA_ATOMIC_LOAD(&stopping));
  lua_setfield(lua, -2, "stopping");
  lua_pushinteger(lua, clock_us());
  lua_setfield(lua, -2, "now_us");
  lua_pushinteger(lua, worker.active);
  lua_setfield(lua, -2, "active");
  lua_pushinteger(lua, worker.errors);
  lua_setfield(lua, -2, "errors");
  return 1;
}

/* Releases this thread's export job after Lua's final flush; idempotent. */
static int release_export(lua_State *lua) {
  if (worker.export_job) {
    worker.export_job = 0;
    _HA_ATOMIC_DEC(&jobs);
  }
  return 0;
}

/* Checks the HAProxy ABI and pushes CJSON plus the observer functions.
 * Called by Lua's package.loadlib.
 */
int luaopen_ray_stream_metrics(lua_State *lua) {
  if (strcmp(haproxy_version, RAY_HAPROXY_ABI))
    return luaL_error(
        lua, "stream observer must use its matching ray-haproxy binary");
  luaopen_cjson(lua);
  lua_pushcfunction(lua, configure);
  lua_setfield(lua, -2, "configure");
  lua_pushcfunction(lua, snapshot);
  lua_setfield(lua, -2, "snapshot");
  lua_pushcfunction(lua, status);
  lua_setfield(lua, -2, "status");
  lua_pushcfunction(lua, release_export);
  lua_setfield(lua, -2, "release_export");
  return 1;
}
#endif
