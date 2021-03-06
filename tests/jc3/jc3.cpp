// jc4.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

#include <vector>
#include <atomic>

//#include "../common/simplethread.h"
//#include "../common/simplethread.cpp"
#include "../common/systemtime.h"
#include "../common/systemtime.cpp"
#include "../../concurrentqueue.h"
using namespace moodycamel;

#define NUM_PRODUCERS 10
#define NUM_ITEMS_PRODUCED 10
#define NUM_CONSUMERS 1
#define NUM_ITEMS_CONSUMED 5
#define NUM_TIMES_CONSUMED 20
#define NUM_THREADS (NUM_PRODUCERS + NUM_CONSUMERS)
#define NUM_ITEMS (NUM_PRODUCERS * NUM_ITEMS_PRODUCED)

template<typename T>
std::size_t my_dequeue_bulk_tok(ConsumerToken& ctok, ConcurrentQueue<T>& q, T* itemFirst, std::size_t wanted)
{
  T* itemNext = itemFirst;
  std::size_t result = 0;
  std::size_t need = wanted;
  std::size_t got;
  SystemTime started = getSystemTime();
  for (std::size_t i = 0; (need != 0) && (i < wanted) && (getTimeDelta(started) < 5000.0); ++i) {
    got = q.try_dequeue_bulk(ctok, itemNext, need);
    if (got != 0) {
      printf("consumed in chunk :%u: /%u\n", got, need);
      result += got;
      itemNext += got;
      need -= got;
    }
    if (need != 0) {
      sleep(250);
    }
  }
  printf("consumed chunks as:%u: /%u\n", result, wanted);
  return result;
}
template<typename T>
std::size_t my_dequeue_bulk(ConcurrentQueue<T>& q, T* itemFirst, std::size_t wanted)
{
  T* itemNext = itemFirst;
  std::size_t result = 0;
  std::size_t need = wanted;
  std::size_t got;
  SystemTime started = getSystemTime();
  for (std::size_t i = 0; (need != 0) && (i < wanted) && (getTimeDelta(started) < 5000.0); ++i) {
    got = q.try_dequeue_bulk(itemNext, need);
    if (got != 0) {
      printf("consumed in chunk :%u: /%u\n", got, need);
      result += got;
      itemNext += got;
      need -= got;
    }
    if (need != 0) {
      sleep(250);
    }
  }
  printf("consumed chunks as:%u: /%u\n", result, wanted);
  return result;
}

typedef struct {
  std::string sessionId;
} Context;
typedef struct {
  std::size_t id;
  std::string packageName;
  std::string messageName;
  std::vector<BYTE> messageBytes;
  Context customContext;
} PendingMessage;
std::atomic<std::size_t> next_pm_id(1);
PendingMessage createPendingMessage() {
  PendingMessage pm;
  pm.id = next_pm_id++;
  pm.packageName = "packageName";
  pm.messageName = "messageName";
  pm.messageBytes = { 0,1,2,3 };
  pm.customContext.sessionId = "sessionId";
  return pm;
}
PendingMessage createPendingMessage(std::size_t force_id) {
  PendingMessage pm;
  pm.id = force_id;
  pm.packageName = "packageName";
  pm.messageName = "messageName";
  pm.messageBytes = { 0,1,2,3 };
  pm.customContext.sessionId = "sessionId";
  return pm;
}

int main()
{
  ConcurrentQueue<PendingMessage> q;
  int dequeued[NUM_ITEMS] = { 0 };
  int bulks[NUM_ITEMS_CONSUMED] = { 0 };
  std::thread threads[NUM_THREADS];

  // enqueue singularly
  // Producers
  for (int i = 0; i != NUM_PRODUCERS; ++i) {
    threads[i] = std::thread([&](int i) {
      for (int j = 0; j != NUM_ITEMS_PRODUCED; ++j) {
        printf("produce :%u:%u: = %u\n", i, j, i * NUM_ITEMS_PRODUCED + j);
        q.enqueue(createPendingMessage());
        sleep(1000);
      }
    }, i);
  }

  // dequeue singularly
  //// Consumers
  //for (int i = NUM_PRODUCERS; i != NUM_THREADS; ++i) {
  //  threads[i] = std::thread([&]() {
  //    PendingMessage item;
  //    for (int j = 0; j != NUM_ITEMS_CONSUMED; ++j) {
  //      if (q.try_dequeue(item)) {
  //        ++dequeued[item.id % NUM_ITEMS];
  //      }
  //    }
  //  });
  //}
  // dequeue bulkily
  // Consumers
  for (int i = 0; i != NUM_CONSUMERS; ++i) {
    threads[NUM_PRODUCERS + i] = std::thread([&]() {
      PendingMessage items[NUM_ITEMS_CONSUMED];
      std::size_t count;
      for (std::size_t j = 0; j != NUM_TIMES_CONSUMED; ++j) {
        //count = q.try_dequeue_bulk(items, NUM_ITEMS_CONSUMED);
        count = my_dequeue_bulk(q, items, NUM_ITEMS_CONSUMED);
        printf("consumed in bulk:%u: /%u\n", count, NUM_ITEMS_CONSUMED);
        if (count > 0) {
          ++bulks[count - 1];
          for (; count != 0; --count) {
            ++dequeued[items[count - 1].id % NUM_ITEMS];
          }
        }
      }
    });
  }

  // Wait for all threads
  for (int i = 0; i != NUM_THREADS; ++i) {
    threads[i].join();
  }

  // dequeue singularly
  //// Collect any leftovers (could be some if e.g. consumers finish before producers)
  //PendingMessage item;
  //while (q.try_dequeue(item)) {
  //  ++dequeued[item.id];
  //}
  // dequeue bulkily
  // Collect any leftovers (could be some if e.g. consumers finish before producers)
  {
    PendingMessage items[NUM_ITEMS_CONSUMED / 2];
    std::size_t count;
    //while ((count = q.try_dequeue_bulk(items, NUM_ITEMS_CONSUMED / 2)) != 0) {
    while ((count = my_dequeue_bulk(q, items, NUM_ITEMS_CONSUMED / 2)) != 0) {
      printf("consumed in bulk:%u: /%u\n", count, NUM_ITEMS_CONSUMED / 2);
      ++bulks[count-1];
      for (std::size_t i = 0; i != count; ++i) {
        ++dequeued[items[i].id % NUM_ITEMS];
      }
    }
  }
  // Make sure everything went in and came back out!
  for (int i = 0; i != NUM_ITEMS; ++i) {
    assert(dequeued[i] == 1);
  }
  // report the bulk sizes utilized
  for (int i = 0; i < NUM_ITEMS_CONSUMED; ++i) {
    if (bulks[i] != 0) {
      printf("consumed in bulk:%u: %u time%hs.\n", i + 1, bulks[i], bulks[i] == 1 ? "" : "s");
    }
  }
  return 0;
}

