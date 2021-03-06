// jc5.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

#include <vector>
#include <atomic>

//#include "../common/simplethread.h"
//#include "../common/simplethread.cpp"
#include "../common/systemtime.h"
#include "../common/systemtime.cpp"
using namespace moodycamel;

#include "three_headed_queue.h"

#define NUM_PRODUCERS 1
#define NUM_ITEMS_PRODUCED 10
#define NUM_ITEMS (NUM_PRODUCERS * NUM_ITEMS_PRODUCED)
#define NUM_CONSUMERS 1
#define NUM_ITEMS_CONSUMED 5
#define NUM_TIMES_CONSUMED (NUM_ITEMS / NUM_ITEMS_CONSUMED)
#define NUM_THREADS (NUM_PRODUCERS + NUM_CONSUMERS)

size_t my_rand(unsigned int limit)
{
  size_t number;
  rand_s(&number);
  // because the high bits are more random than the low bits
  //  we shouldn't simply: return number % limit;
  double value = (((double)number / (double)((size_t)~0)) * (double)limit);
  return (size_t)value;
}

std::atomic<size_t> a_produced = 0;
std::atomic<size_t> a_consumed = 0;
std::atomic<size_t> dequeued[NUM_ITEMS] = { 0 };
std::atomic<size_t> bulks[NUM_ITEMS_CONSUMED] = { 0 };

bool process_in_bulk(size_t thread_id, PendingMessageQueue& q, size_t bulk_size)
{
  PendingMessagePtr* items = new PendingMessagePtr[bulk_size];
  std::size_t count;
  while (q.test(nullptr)) // all pass!
  {
    printf("shuffled :%u: one such record from flow to send\n", thread_id);
    continue;
  }
  count = q.pop_bulk(items, bulk_size, 5000);
  printf("consume :%u: in bulk %u /%u\n", thread_id, count, bulk_size);
  if (count > 0)
  {
    a_consumed.fetch_add(count);
    bulks[count - 1].fetch_add(1);
    for (; count != 0; --count)
    {
      if (!!items[count - 1])
      {
        dequeued[items[count - 1]->id % NUM_ITEMS].fetch_add(1);
      }
    }
  }
  return count > 0;
}
bool process_singularly(size_t thread_id, PendingMessageQueue& q)
{
  PendingMessagePtr item = nullptr;
  while (q.test(nullptr)) // all pass!
  {
    printf("shuffled :%u: one such record from flow to send\n", thread_id);
    continue;
  }
  if (q.pop(item))
  {
    if (!!item) // skip records that failed q.test
    {
      a_consumed.fetch_add(1);
      bulks[0].fetch_add(1);
      printf("consume :%u: .id = %d\n", thread_id, item->id);
      dequeued[item->id % NUM_ITEMS].fetch_add(1);
      item = nullptr; // done with this record.
    }
    return true;
  }
  return false;
}
int main()
{
  // UNIT TESTS
  //{
  //  PendingMessageQueue qq(10);
  //  PendingMessagePtr pm = nullptr;
  //  bool ok;
  //  for (int i = 0; i < 10; ++i)
  //  {
  //    // [0..0), [0..0), [0..0)
  //    if (i == 0)
  //    {
  //      ok = qq._as_expected(0, 0, 0, 0, 0, 0);
  //    }
  //    printf("push a record\n");
  //    pm = createPendingMessagePtr();
  //    //printf("debug: pushing: id = %d\n", pm ? pm->id : -1);
  //    ok = qq.push(pm);
  //    // [0..0), [0..1), [1..0)
  //    if (i == 0)
  //    {
  //      ok = qq._as_expected(0, 0, 0, 1, 1, 0);
  //      ok = qq._expect_region(2, 0, 1);
  //    }
  //    //qq._dump(false);
  //    printf("test a record\n");
  //    ok = qq.test(nullptr);
  //    // [0..1), [1..1), [1..0)
  //    if (i == 0)
  //    {
  //      ok = qq._as_expected(0, 1, 1, 1, 1, 0);
  //      ok = qq._expect_region(2, 1, 1);
  //      ok = qq._expect_region(1, 0, 1);
  //    }
  //    //qq._dump(false);
  //    printf("pop  a record\n");
  //    ok = qq.pop(pm);
  //    // [1..1), [1..1), [1..1)
  //    if (i == 0)
  //    {
  //      ok = qq._as_expected(1, 1, 1, 1, 1, 1);
  //      ok = qq._expect_region(1, 1, 1);
  //    }
  //    printf("debug: popped: id = %d\n", pm ? pm->id : -1);
  //    //qq._dump(false);
  //  }
  //}
  //printf("debug: ----- done with unit test -----\n");

  //ConcurrentQueue<PendingMessagePtr> q;
  PendingMessageQueue q(15);
  std::thread threads[NUM_THREADS];

  // enqueue singularly
  // Producers
  for (int i = 0; i != NUM_PRODUCERS; ++i) {
    threads[i] = std::thread([&](int i) {
      int delay = 0;
      bool ok;
      while (!q.stopped())
      {
        PendingMessagePtr pm = createPendingMessagePtr();
        printf("produce :%u: .id=%d\n", i, pm->id);
        // we MUST enqueue!
        while (!q.stopped())
        {
          ok = q.push(pm);
          if (ok)
          {
            a_produced.fetch_add(1);
            break;
          }
          // back off a randomized amount
          delay = 10 + my_rand(100);
          sleep(delay);
        }
        // we DID enqueue (finally!)
      }
      printf("producing :%u: done\n", i);
    }, i);
  }

  // dequeue bulkily
  // Consumers
  for (int i = NUM_PRODUCERS; i != NUM_THREADS; ++i)
  {
    threads[i] = std::thread([&]() {
      printf("consuming :%u:\n", i);
      while (!q.stopped())
      {
        process_in_bulk(i, q, NUM_ITEMS_CONSUMED); // ignore return
      }
      printf("consuming :%u: done\n", i);
    });
  }
  //// dequeue singularly
  //// Consumers
  //for (int i = NUM_PRODUCERS; i != NUM_THREADS; ++i) {
  //  threads[i] = std::thread([&]() {
  //    printf("consuming :%u:\n", i);
  //    while (!q.stopped())
  //    {
  //      process_singularly(i, q); // ignore return
  //    }
  //    printf("consuming :%u: done\n", i);
  //  });
  //}

  // Wait for all threads
  sleep(10000);
  q.stop();
  printf("waiting for %u threads\n", NUM_THREADS);
  for (int i = 0; i != NUM_THREADS; ++i) {
    threads[i].join();
  }
  printf("waited  for %u threads\n", NUM_THREADS);

  // dequeue leftovers singularly
  // Collect any leftovers (could be some if e.g. consumers finish before producers)
  while (process_singularly(NUM_THREADS, q))
  {
    continue;
  }

  // Make sure everything went in and came back out!
  //  a_produced is the total amount actually produced.
  //  a_consumed is the total amount actually consumed.
  //  actual     will be the number we counted as consumed.
  // all should be equal!
  //
  size_t actual = 0;
  for (int i = 0; i != NUM_ITEMS; ++i)
  {
    int N = dequeued[i].fetch_add(0);
    printf("dequeued[%d] %d times\n", i, N);
    actual += N;
  }
  size_t expectedP = a_produced.fetch_add(0);
  size_t expectedC = a_consumed.fetch_add(0);
  if ((actual != expectedP) || (actual != expectedC))
  {
    printf("failure: expected %u produced, expected %u consumed, got %u consumed\n", expectedP, expectedC, actual);
  }
  else
  {
    printf("success: expected %u produced, expected %u consumed, got %u consumed\n", expectedP, expectedC, actual);
  }
  // report the bulk sizes utilized
  for (int i = 0; i < NUM_ITEMS_CONSUMED; ++i)
  {
    int N = bulks[i].fetch_add(0);
    if (N != 0)
    {
      printf("consumed in bulk:%u: %u time%hs.\n", i + 1, N, N == 1 ? "" : "s");
    }
  }
  return 0;
}

