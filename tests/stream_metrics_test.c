// Standalone tests of the production scanner and histograms, without HAProxy
// networking. HTX callbacks are excluded from this build; every exercised
// function is the actual implementation.
#define _GNU_SOURCE
#include "../src/stream_metrics.c"

#include <stdio.h>

#include "unity.h"

static const char *TAGS =
    "{\"model_name\":\"model\",\"ReplicaId\":\"replica\","
    "\"engine_worker_id\":\"worker\",\"engine\":\"0\"}";

/* Fresh thread state and bounds of 100, 200 and 1000 us for every metric. */
void setUp(void) {
  memset(&worker, 0, sizeof(worker));
  for (unsigned metric = 0; metric < NUM_METRICS; metric++) {
    schemas[metric].bucket_count = 3;
    schemas[metric].bounds[0] = 100;
    schemas[metric].bounds[1] = 200;
    schemas[metric].bounds[2] = 1000;
  }
}

void tearDown(void) {
  while (worker.groups) {
    struct group *next = worker.groups->next;
    free(worker.groups->key);
    free(worker.groups);
    worker.groups = next;
  }
}

/* Returns stream state started at 1000 us in a fresh label group. */
static struct observation *observation(void) {
  struct observation *obs = new_observation(1000);
  TEST_ASSERT_NOT_NULL(obs);
  obs->group = get_group(TAGS, strlen(TAGS));
  TEST_ASSERT_NOT_NULL(obs->group);
  return obs;
}

/* Checks a metric's count, sum (us) and noncumulative buckets b0-b3. */
static void assert_histogram(struct group *group, unsigned metric,
                             uint64_t count, uint64_t sum, uint64_t b0,
                             uint64_t b1, uint64_t b2, uint64_t b3) {
  const struct histogram *histogram = &group->hist[metric];
  TEST_ASSERT_EQUAL_UINT64(count, histogram->count);
  TEST_ASSERT_EQUAL_UINT64(sum, histogram->sum);
  TEST_ASSERT_EQUAL_UINT64(b0, histogram->buckets[0]);
  TEST_ASSERT_EQUAL_UINT64(b1, histogram->buckets[1]);
  TEST_ASSERT_EQUAL_UINT64(b2, histogram->buckets[2]);
  TEST_ASSERT_EQUAL_UINT64(b3, histogram->buckets[3]);
}

/* Feeds text in fragments of at most width bytes, all arriving at now. */
static void feed_split(struct observation *obs, const char *text, size_t width,
                       uint64_t now) {
  size_t length = strlen(text);
  for (size_t offset = 0; offset < length; offset += width) {
    size_t take = length - offset < width ? length - offset : width;
    feed(obs, text + offset, take, now);
  }
}

/* A completion chunk exactly as Ray Serve LLM's vLLM serializer emits it. */
static void completion_event(char *out, size_t size, unsigned index,
                             const char *text, unsigned tokens) {
  snprintf(out, size,
           "data: {\"id\":\"cmpl-1\",\"object\":\"text_completion\","
           "\"created\":1,\"model\":\"m\",\"choices\":[{\"index\":%u,"
           "\"text\":\"%s\",\"logprobs\":null,\"finish_reason\":null,"
           "\"stop_reason\":null,\"prompt_token_ids\":null,\"token_ids\":null,"
           "\"weight_versions\":null,\"ray_token_count\":%u}],"
           "\"kv_transfer_params\":null}\n\n",
           index, text, tokens);
}

/* A chat role chunk (zero tokens), three token chunks, a usage chunk and
 * [DONE], each event at its own time and split at every fragment width.
 */
static void test_every_fragment_width(void) {
  const char *role =
      "data: {\"id\":\"chat-1\",\"object\":\"chat.completion.chunk\","
      "\"created\":1,\"model\":\"m\",\"choices\":[{\"index\":0,\"delta\":"
      "{\"role\":\"assistant\",\"content\":\"\"},\"logprobs\":null,"
      "\"finish_reason\":null,\"ray_token_count\":0}]}\n\n";
  const char *usage =
      "data: {\"id\":\"chat-1\",\"object\":\"chat.completion.chunk\","
      "\"created\":1,\"model\":\"m\",\"choices\":[],\"usage\":{"
      "\"prompt_tokens\":3,\"completion_tokens\":6}}\n\ndata: [DONE]\n\n";
  char first[512], second[512], third[512];
  completion_event(first, sizeof(first), 0, "a", 1);
  completion_event(second, sizeof(second), 0, "bc", 2);
  completion_event(third, sizeof(third), 0, "def", 3);
  size_t widths = strlen(first) + 1;
  for (size_t width = 1; width <= widths; width++) {
    setUp();
    struct observation *obs = observation();
    feed_split(obs, role, width, 1010);
    feed_split(obs, first, width, 1050);   // TTFT 50
    feed_split(obs, second, width, 1350);  // 300 us for 2 tokens: 2 x 150
    feed_split(obs, third, width, 4350);   // 3000 us for 3 tokens: 3 x 1000
    feed_split(obs, usage, width, 4400);
    TEST_ASSERT_EQUAL_UINT8(0, obs->dead);
    assert_histogram(obs->group, TTFT, 1, 50, 1, 0, 0, 0);
    assert_histogram(obs->group, ITL, 5, 3300, 0, 2, 3, 0);
    TEST_ASSERT_EQUAL_UINT64(0, worker.errors);
    free_observation(obs);
    tearDown();
  }
}

/* Events coalesced into one fragment arrive together: zero ITL. */
static void test_coalesced_events(void) {
  char first[512], second[512], both[1024];
  completion_event(first, sizeof(first), 0, "a", 1);
  completion_event(second, sizeof(second), 0, "b", 2);
  snprintf(both, sizeof(both), "%s%s", first, second);
  struct observation *obs = observation();
  feed(obs, both, strlen(both), 1100);
  assert_histogram(obs->group, TTFT, 1, 100, 1, 0, 0, 0);
  assert_histogram(obs->group, ITL, 2, 0, 2, 0, 0, 0);
  free_observation(obs);
}

/* The first chunk records TTFT only, whatever its token count; per-token
 * intervals equal to a bound fall in that bound's bucket.
 */
static void test_first_chunk_batch_and_bounds(void) {
  char first[512], second[512], third[512];
  completion_event(first, sizeof(first), 0, "abc", 3);
  completion_event(second, sizeof(second), 0, "d", 4);
  completion_event(third, sizeof(third), 0, "e", 1);
  struct observation *obs = observation();
  feed(obs, first, strlen(first), 1200);    // TTFT 200 (bound), no ITL
  feed(obs, second, strlen(second), 1600);  // 400 us / 4 = 100 (bound)
  feed(obs, third, strlen(third), 1601);    // 1 us
  assert_histogram(obs->group, TTFT, 1, 200, 0, 1, 0, 0);
  assert_histogram(obs->group, ITL, 5, 401, 5, 0, 0, 0);
  free_observation(obs);
}

/* Interleaved choices keep separate clocks, including grown capacity. */
static void test_multiple_choices(void) {
  char event[512];
  struct observation *obs = observation();
  completion_event(event, sizeof(event), 0, "a", 1);
  feed(obs, event, strlen(event), 1100);
  completion_event(event, sizeof(event), 5, "b", 1);
  feed(obs, event, strlen(event), 1300);
  completion_event(event, sizeof(event), 0, "c", 1);
  feed(obs, event, strlen(event), 1400);  // choice 0: 300
  completion_event(event, sizeof(event), 5, "d", 2);
  feed(obs, event, strlen(event), 3300);  // choice 5: 2000 / 2
  TEST_ASSERT_TRUE(obs->choices > 5);
  assert_histogram(obs->group, TTFT, 2, 400, 1, 0, 1, 0);
  assert_histogram(obs->group, ITL, 3, 2300, 0, 0, 3, 0);
  free_observation(obs);
}

/* Marker-like generated text is JSON-escaped and must not match. */
static void test_escaped_markers_in_text(void) {
  char event[512];
  completion_event(
      event, sizeof(event), 0,
      "\\\"choices\\\":[{\\\"index\\\":7 \\\"ray_token_count\\\":9", 1);
  struct observation *obs = observation();
  feed_split(obs, event, 3, 1100);
  assert_histogram(obs->group, TTFT, 1, 100, 1, 0, 0, 0);
  assert_histogram(obs->group, ITL, 0, 0, 0, 0, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(0, obs->index);
  free_observation(obs);
}

/* A carried partial marker that fails must not hide a later real marker,
 * including overlaps of the marker's leading quote.
 */
static void test_carry_overlaps(void) {
  const char *parts[] = {"\"\"choi", "ces\":[{\"index\":0,\"x\":\"\"ray_",
                         "\"ray_tok", "en_count\":2}]}\n\n"};
  struct observation *obs = observation();
  for (unsigned i = 0; i < 4; i++) feed(obs, parts[i], strlen(parts[i]), 1100);
  assert_histogram(obs->group, TTFT, 1, 100, 1, 0, 0, 0);
  assert_histogram(obs->group, ITL, 0, 0, 0, 0, 0, 0);
  TEST_ASSERT_EQUAL_UINT8(0, obs->dead);
  free_observation(obs);
}

/* Malformed or out-of-range numbers disable collection and count one error. */
static void test_malformed_numbers(void) {
  const char *cases[] = {
      "\"choices\":[{\"index\":0,\"ray_token_count\":x}",
      "\"choices\":[{\"index\":0,\"ray_token_count\":1234567890}",
      "\"choices\":[{\"index\":4096,\"ray_token_count\":1}",
  };
  for (unsigned i = 0; i < 3; i++) {
    setUp();
    struct observation *obs = observation();
    feed(obs, cases[i], strlen(cases[i]), 1100);
    TEST_ASSERT_EQUAL_UINT8(1, obs->dead);
    TEST_ASSERT_EQUAL_UINT64(1, worker.errors);
    assert_histogram(obs->group, TTFT, 0, 0, 0, 0, 0, 0);
    // Later input is ignored once disabled.
    char event[512];
    completion_event(event, sizeof(event), 0, "a", 1);
    feed(obs, event, strlen(event), 1200);
    assert_histogram(obs->group, TTFT, 0, 0, 0, 0, 0, 0);
    free_observation(obs);
    tearDown();
  }
}

/* Clock regressions disable collection instead of recording wrapped values. */
static void test_clock_regression(void) {
  char event[512];
  completion_event(event, sizeof(event), 0, "a", 1);
  struct observation *obs = observation();
  feed(obs, event, strlen(event), 999);
  TEST_ASSERT_EQUAL_UINT8(1, obs->dead);
  TEST_ASSERT_EQUAL_UINT64(1, worker.errors);
  free_observation(obs);
}

/* Completion records request latency and routing; impossible routing time is
 * an error.
 */
static void test_finish(void) {
  struct observation *obs = observation();
  finish(obs, 1900, 1, 150);
  assert_histogram(obs->group, REQUEST, 1, 900, 0, 0, 1, 0);
  assert_histogram(obs->group, ROUTING, 1, 150, 0, 1, 0, 0);
  free_observation(obs);
  obs = observation();
  finish(obs, 1100, 1, 101);
  TEST_ASSERT_EQUAL_UINT64(1, worker.errors);
  assert_histogram(obs->group, REQUEST, 1, 900, 0, 0, 1, 0);
  free_observation(obs);
}

/* Identities share one group; empty or NUL-containing keys and the group
 * limit are rejected without reading past stored keys.
 */
static void test_groups(void) {
  struct group *a = get_group(TAGS, strlen(TAGS));
  TEST_ASSERT_EQUAL_PTR(a, get_group(TAGS, strlen(TAGS)));
  TEST_ASSERT_NOT_EQUAL(a, get_group(TAGS, strlen(TAGS) - 1));
  TEST_ASSERT_EQUAL_UINT(2, worker.group_count);
  TEST_ASSERT_NULL(get_group("ab\0cd", 5));
  TEST_ASSERT_NULL(get_group("", 0));
  struct group *b = get_group("ab", 2);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_NULL(get_group("ab\0cd", 5));
  TEST_ASSERT_EQUAL_PTR(b, get_group("ab", 2));
  worker.group_count = MAX_GROUPS;
  TEST_ASSERT_NULL(get_group("new", 3));
  worker.group_count = 3;
}

/* Calls configure with the given bounds for every metric; returns whether it
 * succeeded. Restores the test schema afterwards.
 */
static int configure_with(const long long *bounds, unsigned count) {
  lua_State *lua = luaL_newstate();
  int ok;
  lua_newtable(lua);
  for (unsigned metric = 1; metric <= NUM_METRICS; metric++) {
    lua_newtable(lua);
    lua_newtable(lua);
    for (unsigned i = 0; i < count; i++) {
      lua_pushinteger(lua, bounds[i]);
      lua_rawseti(lua, -2, i + 1);
    }
    lua_setfield(lua, -2, "bounds");
    lua_rawseti(lua, -2, metric);
  }
  lua_pushcfunction(lua, configure);
  lua_insert(lua, 1);
  ok = lua_pcall(lua, 1, 0, 0) == LUA_OK;
  lua_close(lua);
  return ok;
}

/* Bounds must be positive, ascending and bounded; failures leave the schema. */
static void test_configure_validation(void) {
  const long long good[] = {5, 10, 2560000000LL};
  const long long unsorted[] = {10, 5};
  const long long negative[] = {-1, 5};
  const long long huge[] = {5, 20000000000LL};
  TEST_ASSERT_TRUE(configure_with(good, 3));
  TEST_ASSERT_EQUAL_UINT(3, schemas[ITL].bucket_count);
  TEST_ASSERT_EQUAL_UINT64(2560000000ULL, schemas[ITL].bounds[2]);
  TEST_ASSERT_FALSE(configure_with(unsorted, 2));
  TEST_ASSERT_FALSE(configure_with(negative, 2));
  TEST_ASSERT_FALSE(configure_with(huge, 2));
  TEST_ASSERT_EQUAL_UINT64(2560000000ULL, schemas[ITL].bounds[2]);
}

/* snapshot exposes each identity's exact histograms to the exporter. */
static void test_snapshot(void) {
  char event[512];
  struct observation *obs = observation();
  completion_event(event, sizeof(event), 0, "a", 1);
  feed(obs, event, strlen(event), 1100);
  completion_event(event, sizeof(event), 0, "b", 2);
  feed(obs, event, strlen(event), 1400);
  finish(obs, 1500, 1, 50);
  worker.errors = 7;
  lua_State *lua = luaL_newstate();
  TEST_ASSERT_EQUAL_INT(3, snapshot(lua));
  TEST_ASSERT_EQUAL_INT(3, lua_gettop(lua));
  TEST_ASSERT_EQUAL_INT64(1, lua_tointeger(lua, 3));
  TEST_ASSERT_EQUAL_INT64(7, lua_tointeger(lua, 2));
  lua_getfield(lua, 1, TAGS);
  TEST_ASSERT_TRUE(lua_istable(lua, -1));
  const long long expected[NUM_METRICS][2] = {
      {1, 100}, {2, 300}, {1, 50}, {1, 500}};
  for (unsigned metric = 0; metric < NUM_METRICS; metric++) {
    lua_rawgeti(lua, -1, metric + 1);
    lua_getfield(lua, -1, "count");
    TEST_ASSERT_EQUAL_INT64(expected[metric][0], lua_tointeger(lua, -1));
    lua_getfield(lua, -2, "sum_us");
    TEST_ASSERT_EQUAL_INT64(expected[metric][1], lua_tointeger(lua, -1));
    lua_getfield(lua, -3, "buckets");
    TEST_ASSERT_EQUAL_UINT(schemas[metric].bucket_count + 1,
                           lua_rawlen(lua, -1));
    lua_pop(lua, 4);
  }
  lua_close(lua);
  free_observation(obs);
}

static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static uint32_t rnd(uint32_t n) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return (uint32_t)(rng % n);
}

/* Independent reference: linear bucket search, per-token interval as a
 * fraction compared exactly.
 */
static void reference_record(struct histogram *h, unsigned metric,
                             uint64_t elapsed, uint64_t tokens) {
  unsigned bucket = 0;
  while (bucket < schemas[metric].bucket_count &&
         elapsed > schemas[metric].bounds[bucket] * tokens)
    bucket++;
  h->count += tokens;
  h->sum += elapsed;
  h->buckets[bucket] += tokens;
}

/* Random vLLM-shaped streams with marker-like escaped text, zero-token and
 * non-choice events, multi-digit counts and indexes, split at random fragment
 * boundaries. Every histogram must match the reference exactly.
 */
static void test_randomized_streams(void) {
  static const char *noise[] = {"\\\"ray_token_count\\\":7",
                                "\\\"choices\\\":[{\\\"index\\\":3",
                                "\\\"ray_tok",
                                "\\\"choi",
                                "ray_token_count",
                                "\\\\",
                                "123",
                                "\\\"",
                                ":[{",
                                "\\u0022",
                                "x",
                                "\\n\\n"};
  static char wire[1 << 16];
  enum { MAX_EVENTS = 48 };
  for (int iteration = 0; iteration < 3000; iteration++) {
    size_t length = 0;
    size_t terminators[MAX_EVENTS];
    unsigned indexes[MAX_EVENTS], counts[MAX_EVENTS], events = 0;
    unsigned total = 1 + rnd(MAX_EVENTS - 1);
    for (unsigned e = 0; e < total; e++) {
      unsigned kind = rnd(10);
      if (kind == 0) {
        length += snprintf(wire + length, sizeof(wire) - length,
                           "data: {\"choices\":[],\"usage\":{\"x\":1}}\n\n");
        continue;
      }
      if (kind == 1) {
        length += snprintf(wire + length, sizeof(wire) - length,
                           "data: {\"error\":{\"message\":\"\\\"ray_token_count"
                           "\\\":1\"}}\n\ndata: [DONE]\n\n");
        continue;
      }
      unsigned index = rnd(6) == 0 ? 2 + rnd(10) : rnd(2);
      unsigned count = rnd(8) == 0 ? rnd(20000) : rnd(4);
      length += snprintf(wire + length, sizeof(wire) - length,
                         "data: {\"id\":\"c\",\"choices\":[{\"index\":%u,"
                         "\"text\":\"",
                         index);
      for (unsigned n = rnd(5); n; n--)
        length += snprintf(wire + length, sizeof(wire) - length, "%s",
                           noise[rnd(sizeof(noise) / sizeof(*noise))]);
      length += snprintf(wire + length, sizeof(wire) - length,
                         "\",\"token_ids\":null,\"ray_token_count\":%u", count);
      terminators[events] = length;  // The byte that ends the count.
      indexes[events] = index;
      counts[events++] = count;
      length += snprintf(wire + length, sizeof(wire) - length,
                         "}],\"usage\":null}\n\n");
    }
    // Fragment boundaries and the arrival time of each byte's fragment.
    static uint64_t byte_time[1 << 16];
    struct observation *obs = observation();
    uint64_t now = 2000;
    for (size_t offset = 0; offset < length;) {
      size_t take = 1 + rnd(rnd(4) ? 40 : 400);
      if (take > length - offset) take = length - offset;
      now += 1 + rnd(400);
      for (size_t i = offset; i < offset + take; i++) byte_time[i] = now;
      feed(obs, wire + offset, take, now);
      offset += take;
    }
    // Reference model over the generated events.
    struct histogram ttft = {0}, itl = {0};
    uint64_t last[16] = {0};
    for (unsigned e = 0; e < events; e++) {
      uint64_t at = byte_time[terminators[e]];
      if (!counts[e]) continue;
      if (!last[indexes[e]])
        reference_record(&ttft, TTFT, at - 1000, 1);
      else
        reference_record(&itl, ITL, at - last[indexes[e]], counts[e]);
      last[indexes[e]] = at;
    }
    TEST_ASSERT_EQUAL_UINT8(0, obs->dead);
    TEST_ASSERT_EQUAL_UINT64(0, worker.errors);
    TEST_ASSERT_EQUAL_MEMORY(&ttft, &obs->group->hist[TTFT], sizeof(ttft));
    TEST_ASSERT_EQUAL_MEMORY(&itl, &obs->group->hist[ITL], sizeof(itl));
    free_observation(obs);
    tearDown();
    setUp();
  }
}

/* Large intervals and token counts stay exact without overflow. */
static void test_large_values(void) {
  char event[512];
  struct observation *obs = observation();
  completion_event(event, sizeof(event), 0, "a", 1);
  feed(obs, event, strlen(event), 1000 + 2000000000ULL);
  completion_event(event, sizeof(event), 0, "b", 999999999);
  feed(obs, event, strlen(event), 1000 + 4000000000ULL);
  assert_histogram(obs->group, TTFT, 1, 2000000000ULL, 0, 0, 0, 1);
  assert_histogram(obs->group, ITL, 999999999, 2000000000ULL, 999999999, 0, 0,
                   0);
  free_observation(obs);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_every_fragment_width);
  RUN_TEST(test_coalesced_events);
  RUN_TEST(test_first_chunk_batch_and_bounds);
  RUN_TEST(test_multiple_choices);
  RUN_TEST(test_escaped_markers_in_text);
  RUN_TEST(test_carry_overlaps);
  RUN_TEST(test_malformed_numbers);
  RUN_TEST(test_clock_regression);
  RUN_TEST(test_finish);
  RUN_TEST(test_groups);
  RUN_TEST(test_configure_validation);
  RUN_TEST(test_snapshot);
  RUN_TEST(test_large_values);
  RUN_TEST(test_randomized_streams);
  return UNITY_END();
}
