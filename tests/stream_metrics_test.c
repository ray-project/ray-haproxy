/* Standalone tests of the production parser/ring, without HAProxy networking.
 * HTX callbacks are excluded from this build. All exercised parser,
 * queue, and retirement functions are the actual implementation. */
#define _GNU_SOURCE
#include "../src/stream_metrics.c"

#include "unity.h"
#include <stdio.h>

void setUp(void) {
  memset(&worker, 0, sizeof(worker));
  TEST_ASSERT_EQUAL_INT(0, init_worker());
  for (unsigned m = 0; m < 4; m++) {
    schemas[m].n = 3;
    schemas[m].bounds[0] = 100;
    schemas[m].bounds[1] = 200;
    schemas[m].bounds[2] = 1000;
  }
}

static struct observation *observation(const char *replica) {
  char tags[256];
  snprintf(tags,
           sizeof(tags),
           "{\"model_name\":\"model\",\"ReplicaId\":\"%s\","
           "\"engine_worker_id\":\"worker\",\"engine\":\"0\"}",
           replica);
  struct observation *o = allocate_observation();
  TEST_ASSERT_TRUE(o);
  o->owner = &worker;
  o->last = LUA_NOREF;
  o->started = 1000;
  o->group = get_group(tags, tags, strlen(tags));
  TEST_ASSERT_TRUE(o->group);
  return o;
}

static void assert_histogram(struct group *g,
                             unsigned metric,
                             uint64_t count,
                             uint64_t sum,
                             uint64_t b0,
                             uint64_t b1,
                             uint64_t b2,
                             uint64_t b3) {
  struct histogram *h = &g->hist[metric];
  TEST_ASSERT_EQUAL_UINT64(count, h->count);
  TEST_ASSERT_EQUAL_UINT64(sum, h->sum);
  TEST_ASSERT_EQUAL_UINT64(b0, h->buckets[0]);
  TEST_ASSERT_EQUAL_UINT64(b1, h->buckets[1]);
  TEST_ASSERT_EQUAL_UINT64(b2, h->buckets[2]);
  TEST_ASSERT_EQUAL_UINT64(b3, h->buckets[3]);
}

static void fragment(struct observation *o,
                     const char *wire,
                     uint64_t now,
                     size_t width) {
  size_t length = strlen(wire);
  for (size_t offset = 0; offset < length; offset += width) {
    size_t bytes = length - offset < width ? length - offset : width;
    feed(o, wire + offset, bytes, now);
  }
}

static void parser(size_t width) {
  struct observation *o = observation("parser");
  fragment(o,
           ": comment\r\nevent: ignored\r\n\r\n"
           "data: {\"choices\":[{\"index\":0,\"ray_token_count\":0,\"delta\":{\"role\":\"assistant\"}}]}\r\n\r\n",
           1050, width);
  fragment(o,
           "data: {\"choices\":[{\"index\":0,\"text\":\"a\",\"ray_token_count\":3}]}\r\n\r\n"
           "data: {\"choices\":[{\"index\":1,\"delta\":{\"content\":\"\\ud83d\\ude00\"},\"ray_token_count\":2}]}\r\n\r\n",
           1100, width);
  /* The HAProxy fragment completing the event supplies the timestamp. */
  const char *partial = "data: {\"choices\":[{\"index\":0,\"text\":\"b\",\"ray_token_count\":4}]}";
  feed(o, partial, strlen(partial), 1300);
  fragment(o, "\n\n", 1400, width);
  fragment(o,
           "data: {\"choices\":[{\"index\":1,\"token_ids\":[42],\"ray_token_count\":1}]}\n\n"
           "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning\":\"x\"},\"ray_token_count\":1}]}\n\n"
           "data: {\"choices\":[{\"index\":0,\"text\":\"c\",\"ray_token_count\":1}]}\n\n",
           1600, width);
  fragment(o,
           "data: {\"choices\":[],\"usage\":{\"completion_tokens\":12}}\n\n"
           "data: {\"choices\":[{\"index\":0,\"text\":\"\",\"finish_reason\":\"stop\",\"ray_token_count\":0}]}\n\n"
           "data: [DONE]\n\n", 1650, width);
  finish(o, 1700, 1, 50);
  assert_histogram(o->group, 0, 2, 200, 2, 0, 0, 0);
  /* 4 x 75us, 1 x 500us, 1 x 200us, 1 x 0us. Initial batches have no ITL;
   * coalesced events legitimately have zero observed arrival separation. */
  assert_histogram(o->group, 1, 7, 1000, 5, 1, 1, 0);
  assert_histogram(o->group, 2, 1, 50, 1, 0, 0, 0);
  assert_histogram(o->group, 3, 1, 700, 0, 0, 1, 0);
  release(o);
  free(o);
}

static void test_fractional_itl(void) {
  struct observation *o = observation("fractional");
  const char *first = "data: {\"choices\":[{\"index\":0,\"ray_token_count\":3}]}\n\n";
  const char *second = "data: {\"choices\":[{\"index\":0,\"ray_token_count\":3}]}\n\n";
  feed(o, first, strlen(first), 1100);
  feed(o, second, strlen(second), 1401);
  /* 301/3us is greater than 100us: do not truncate before bucket selection.
   * Histogram sum preserves the full interval rather than rounding N times. */
  assert_histogram(o->group, 1, 3, 301, 0, 3, 0, 0);
  release(o);
  free(o);
}

static void wait_drained(void) {
  struct background *bg = worker.background;
  uint64_t deadline = clock_us() + 5000000;
  while (atomic_load(&bg->tail) != atomic_load(&bg->head)) {
    TEST_ASSERT_TRUE(clock_us() < deadline);
    struct timespec idle = {.tv_nsec = 1000000};
    nanosleep(&idle, NULL);
  }
}

static void test_async_lifetime(void) {
  TEST_ASSERT_TRUE(init_background() == 0);
  atomic_store(&worker.background->test_delay_us, 10000);
  struct observation *o = observation("async");
  struct group *g = o->group;
  const char *wire = "data: {\"choices\":[{\"text\":\"x\",\"ray_token_count\":1}]}\n\n";
  TEST_ASSERT_TRUE(enqueue(o, wire, strlen(wire), 1100, 0, 0, 0));
  const char *second = "data: {\"choices\":[{\"text\":\"y\",\"ray_token_count\":3}]}\n\n";
  TEST_ASSERT_TRUE(enqueue(o, second, strlen(second), 1500, 0, 0, 0));
  TEST_ASSERT_TRUE(enqueue(o, NULL, 0, 1700, 1, 1, 50));
  /* Detach before parsing ends: retirement must keep the request alive. */
  drop_reference(o);
  wait_drained();
  assert_histogram(g, 0, 1, 100, 1, 0, 0, 0);
  assert_histogram(g, 1, 3, 400, 0, 3, 0, 0);
  assert_histogram(g, 3, 1, 700, 0, 0, 1, 0);
}

static void test_overflow_recovery(void) {
  TEST_ASSERT_TRUE(init_background() == 0);
  atomic_store(&worker.background->test_delay_us, 1000000);
  struct observation *bad = observation("overflow");
  for (unsigned i = 0; i < MAX_PENDING + 32; i++) enqueue(bad, "x", 1, 1100, 0, 0, 0);
  TEST_ASSERT_TRUE(atomic_load(&bad->dead));
  TEST_ASSERT_TRUE(atomic_load(&worker.errors) == 1);
  drop_reference(bad);
  atomic_store(&worker.background->test_delay_us, 0);
  wait_drained();
  struct observation *good = observation("recovery");
  struct group *g = good->group;
  const char *wire = "data: {\"choices\":[{\"text\":\"x\",\"ray_token_count\":1}]}\n\n";
  TEST_ASSERT_TRUE(enqueue(good, wire, strlen(wire), 1200, 0, 0, 0));
  TEST_ASSERT_TRUE(enqueue(good, NULL, 0, 1500, 1, 1, 20));
  drop_reference(good);
  wait_drained();
  assert_histogram(g, 0, 1, 200, 0, 1, 0, 0);
  assert_histogram(g, 3, 1, 500, 0, 0, 1, 0);
  TEST_ASSERT_TRUE(atomic_load(&worker.errors) == 1);
}

static void test_oversized_fragment(void) {
  TEST_ASSERT_TRUE(init_background() == 0);
  struct observation *o = observation("large");
  struct group *g = o->group;
  struct item *item = prepare_item(o, INLINE_FRAGMENT + 1, 1200, 0, 0, 0);
  TEST_ASSERT_TRUE(item && item->allocated && item->body != item->inline_body);
  const char *wire = "data: {\"choices\":[{\"text\":\"x\",\"ray_token_count\":1}]}\n\n";
  memset(item->body, '\n', INLINE_FRAGMENT + 1);
  memcpy(item->body + INLINE_FRAGMENT + 1 - strlen(wire), wire, strlen(wire));
  publish_item(o, item, INLINE_FRAGMENT + 1);
  TEST_ASSERT_TRUE(enqueue(o, NULL, 0, 1500, 1, 1, 20));
  drop_reference(o);
  wait_drained();
  assert_histogram(g, 0, 1, 200, 0, 1, 0, 0);
  struct observation *bounded = observation("bounded");
  TEST_ASSERT_TRUE(!prepare_item(bounded, MAX_BYTES + 1, 1500, 0, 0, 0));
  TEST_ASSERT_TRUE(atomic_load(&bounded->dead));
  drop_reference(bounded);
}

static void test_event_limit(void) {
  const char *prefix = "data: {\"choices\":[{\"text\":\"";
  const char *suffix = "\",\"ray_token_count\":1}]}\n\n";
  for (unsigned extra = 0; extra <= 1; extra++) {
    size_t length = MAX_PAYLOAD + extra;
    char *wire = malloc(length + 1);
    TEST_ASSERT_TRUE(wire);
    memcpy(wire, prefix, strlen(prefix));
    memset(wire + strlen(prefix), 'x', length - strlen(prefix) - strlen(suffix));
    memcpy(wire + length - strlen(suffix), suffix, strlen(suffix) + 1);
    struct observation *o = observation(extra ? "too-large" : "at-limit");
    fragment(o, wire, 1200, 17001);
    TEST_ASSERT_TRUE(atomic_load(&o->dead) == (int)extra);
    assert_histogram(o->group, 0, extra ? 0 : 1, extra ? 0 : 200, 0, extra ? 0 : 1, 0, 0);
    release(o);
    free(o);
    free(wire);
  }
  TEST_ASSERT_TRUE(atomic_load(&worker.errors) == 1);
  TEST_ASSERT_TRUE(worker.bytes == 0);
}

static void test_small_queue(void) {
  TEST_ASSERT_EQUAL_INT(0, setenv("RAY_SERVE_STREAM_METRICS_QUEUE_SLOTS", "3", 1));
  TEST_ASSERT_EQUAL_INT(0, init_background());
  struct background *bg = worker.background;
  TEST_ASSERT_TRUE(bg->capacity == 3);
  TEST_ASSERT_TRUE(bg->allocation < 8 * 1024);
  atomic_store(&bg->test_delay_us, 1000000);
  struct observation *o = observation("small-queue");
  for (unsigned i = 0; i < 4; i++) enqueue(o, "x", 1, 1100, 0, 0, 0);
  TEST_ASSERT_TRUE(atomic_load(&o->dead));
  TEST_ASSERT_TRUE(bg->high_water == 3);
  drop_reference(o);
  atomic_store(&bg->test_delay_us, 0);
  wait_drained();
  unsetenv("RAY_SERVE_STREAM_METRICS_QUEUE_SLOTS");
}

void tearDown(void) {
  if (worker.background) deinit_thread(NULL, NULL);
  while (worker.groups) {
    struct group *g = worker.groups;
    worker.groups = g->next;
    free(g->key);
    for (unsigned i = 0; i < 4; i++) free(g->labels[i]);
    free(g);
  }
  lua_close(worker.parser);
  unsetenv("RAY_SERVE_STREAM_METRICS_QUEUE_SLOTS");
}

static void test_parser_byte_fragments(void) { parser(1); }
static void test_parser_split_fragments(void) { parser(17); }
static void test_parser_coalesced_events(void) { parser(4096); }

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_parser_byte_fragments);
  RUN_TEST(test_parser_split_fragments);
  RUN_TEST(test_parser_coalesced_events);
  RUN_TEST(test_fractional_itl);
  RUN_TEST(test_async_lifetime);
  RUN_TEST(test_overflow_recovery);
  RUN_TEST(test_oversized_fragment);
  RUN_TEST(test_event_limit);
  RUN_TEST(test_small_queue);
  return UNITY_END();
}
