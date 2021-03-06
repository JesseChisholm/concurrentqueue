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
#include "pendingmessageptr_queue.h"

#define NUM_PRODUCERS 10
#define NUM_ITEMS_PRODUCED 10
#define NUM_ITEMS (NUM_PRODUCERS * NUM_ITEMS_PRODUCED)
#define NUM_CONSUMERS 1
#define NUM_ITEMS_CONSUMED 5
#define NUM_TIMES_CONSUMED (NUM_ITEMS / NUM_ITEMS_CONSUMED)
#define NUM_THREADS (NUM_PRODUCERS + NUM_CONSUMERS)

unsigned int my_rand(unsigned int limit)
{
  unsigned int number;
  rand_s(&number);
  // because the high bits are more random than the low bits
  //  we shouldn't simply: return number % limit;
  double value = (((double)number / (double)(UINT_MAX - 1)) * (double)limit);
  return (unsigned int)value;
}

std::atomic<unsigned int> a_extra = 0;

template<typename T>
std::size_t my_dequeue_bulk_tok(ConsumerToken& ctok, ConcurrentQueue<T>& q, T* itemFirst, std::size_t wanted, std::size_t maxDelay = 5000)
{
  if ((maxDelay == 0) || (maxDelay > 30000))
    maxDelay = 5000;
  std::size_t sleepDelay = maxDelay / 20;
  T* itemNext = itemFirst;
  std::size_t result = 0;
  std::size_t need = wanted;
  std::size_t got;
  SystemTime started = getSystemTime();
  for (std::size_t i = 0; (need != 0) && (i < wanted) && (getTimeDelta(started) < maxDelay); ++i) {
    got = q.try_dequeue_bulk(ctok, itemNext, need);
    if (got != 0) {
      printf("consumed in chunk :%u: /%u\n", got, need);
      result += got;
      itemNext += got;
      need -= got;
    }
    if (need != 0) {
      sleep(sleepDelay);
    }
  }
  printf("consumed chunks as:%u: /%u\n", result, wanted);
  return result;
}
template<typename T>
std::size_t my_dequeue_bulk(ConcurrentQueue<T>& q, T* itemFirst, std::size_t wanted, std::size_t maxDelay = 5000)
{
  if ((maxDelay == 0) || (maxDelay > 30000))
    maxDelay = 5000;
  std::size_t sleepDelay = maxDelay / 20;
  T* itemNext = itemFirst;
  std::size_t result = 0;
  std::size_t need = wanted;
  std::size_t got;
  SystemTime started = getSystemTime();
  for (std::size_t i = 0; (need != 0) && (i < wanted) && (getTimeDelta(started) < maxDelay); ++i) {
    got = q.try_dequeue_bulk(itemNext, need);
    if (got != 0) {
      printf("consumed in chunk :%u: /%u\n", got, need);
      result += got;
      itemNext += got;
      need -= got;
    }
    if (need != 0) {
      sleep(sleepDelay);
    }
  }
  printf("consumed chunks as:%u: /%u\n", result, wanted);
  return result;
}
std::atomic<std::size_t> next_pm_id(1);

void init_PendingMessage(PendingMessage& pm, std::size_t force_id = std::string::npos)
{
  pm.id = (force_id == std::string::npos) ? next_pm_id++ : force_id;
  pm.packageName = "packageName";
  pm.messageName = "messageName";
  pm.messageBytes = { 0,1,2,3 };
  pm.customContext = std::make_shared<Context>();
  pm.customContext->sessionId = "sessionId";
  pm.customContext->host = std::make_shared<Host>();
  pm.customContext->host->id = "hostId";
  pm.customContext->program = std::make_shared<Program>();
  pm.customContext->program->id = "programId";
  pm.customContext->program->name = "programName";
  pm.customContext->program->version = "1.2.3-programVersion";
  pm.customContext->program->versionDetails = std::make_shared<VersionDetails>();
  pm.customContext->program->versionDetails->major = 1;
  pm.customContext->program->versionDetails->minor = 2;
  pm.customContext->program->versionDetails->patch = 3;
  pm.customContext->program->versionDetails->prerelease = "programVersion";
  pm.customContext->program->versionDetails->build = 0;
  pm.customContext->program->sdk = std::make_shared<Sdk>();
  pm.customContext->program->sdk->id = "sdkId";
  pm.customContext->program->sdk->name = "sdkName";
  pm.customContext->program->sdk->version = "1.2.3-sdkVersion";
  pm.customContext->program->sdk->versionDetails = std::make_shared<VersionDetails>();
  pm.customContext->program->sdk->versionDetails->major = 1;
  pm.customContext->program->sdk->versionDetails->minor = 2;
  pm.customContext->program->sdk->versionDetails->patch = 3;
  pm.customContext->program->sdk->versionDetails->prerelease = "sdkVersion";
  pm.customContext->program->sdk->versionDetails->build = 0;
}
PendingMessage createPendingMessage(std::size_t force_id) {
  PendingMessage pm;
  init_PendingMessage(pm, force_id);
  return pm;
}
PendingMessage createPendingMessage() {
  return createPendingMessage(next_pm_id++);
}
PendingMessagePtr createPendingMessagePtr(std::size_t force_id) {
  PendingMessagePtr pm = new PendingMessage();
  init_PendingMessage(*pm, force_id);
  return pm;
}
PendingMessagePtr createPendingMessagePtr() {
  return createPendingMessagePtr(next_pm_id++);
}

int main()
{
  //ConcurrentQueue<PendingMessagePtr> q;
  PendingMessageQueue q;
  int dequeued[NUM_ITEMS] = { 0 };
  int bulks[NUM_ITEMS_CONSUMED] = { 0 };
  std::thread threads[NUM_THREADS];

  // enqueue singularly
  // Producers
  for (int i = 0; i != NUM_PRODUCERS; ++i) {
    threads[i] = std::thread([&](int i) {
      unsigned int extra = my_rand(NUM_ITEMS_PRODUCED);
      unsigned int current = a_extra.fetch_add(extra);
      unsigned int delay = 0;
      int maxItems = NUM_ITEMS_PRODUCED + extra;
      printf("producing :%u:%u+%u: = %u:%u\n", i, NUM_ITEMS_PRODUCED, extra, maxItems, current+extra);
      for (int j = 0; j != maxItems; ++j) {
        printf("produce :%u:%u: = %u\n", i, j, i * maxItems + j);
        q.enqueue(createPendingMessagePtr());
        delay = 10 + my_rand(1000);
        sleep(delay);
        // occasionally blast things in memory.
        //if ((i*maxItems + j) % 42 == 0)
        //{
        //  PendingMessagePtr ppm = q.top();
        //  if (ppm)
        //  {
        //    // but don't change its id; just everything else.
        //    init_PendingMessage(*ppm, ppm->id);
        //    //init_PendingMessage(*ppm);
        //  }
        //}
      }
    }, i);
  }

  // dequeue singularly
  //// Consumers
  //for (int i = NUM_PRODUCERS; i != NUM_THREADS; ++i) {
  //  threads[i] = std::thread([&]() {
  //    PendingMessagePtr item;
  //    for (int j = 0; j != NUM_ITEMS_CONSUMED; ++j) {
  //      if (q.try_dequeue(item)) {
  //        ++dequeued[item->id % NUM_ITEMS];
  //      }
  //    }
  //  });
  //}
  // dequeue bulkily
  // Consumers
  for (int i = 0; i != NUM_CONSUMERS; ++i) {
    threads[NUM_PRODUCERS + i] = std::thread([&]() {
      PendingMessagePtr items[NUM_ITEMS_CONSUMED];
      std::size_t count;
      for (std::size_t j = 0; j != NUM_TIMES_CONSUMED; ++j) {
        //count = q.try_dequeue_bulk(items, NUM_ITEMS_CONSUMED);
        count = my_dequeue_bulk(q, items, NUM_ITEMS_CONSUMED);
        printf("consumed in bulk:%u: /%u\n", count, NUM_ITEMS_CONSUMED);
        if (count > 0) {
          ++bulks[count - 1];
          for (; count != 0; --count) {
            ++dequeued[items[count - 1]->id % NUM_ITEMS];
          }
        }
      }
    });
  }

  // Wait for all threads
  printf("waiting for %u threads\n", NUM_THREADS);
  for (int i = 0; i != NUM_THREADS; ++i) {
    threads[i].join();
  }
  printf("waited  for %u threads\n", NUM_THREADS);

  // dequeue singularly
  //// Collect any leftovers (could be some if e.g. consumers finish before producers)
  //PendingMessage item;
  //while (q.try_dequeue(item)) {
  //  ++dequeued[item.id];
  //}
  // dequeue bulkily
  // Collect any leftovers (could be some if e.g. consumers finish before producers)
  {
    PendingMessagePtr items[NUM_ITEMS_CONSUMED];
    std::size_t count;
    //while ((count = q.try_dequeue_bulk(items, NUM_ITEMS_CONSUMED)) != 0) {
    while ((count = my_dequeue_bulk(q, items, NUM_ITEMS_CONSUMED)) != 0) {
      printf("tossed in bulk:%u: /%u\n", count, NUM_ITEMS_CONSUMED);
      ++bulks[count-1];
      for (std::size_t i = 0; i != count; ++i) {
        ++dequeued[items[i]->id % NUM_ITEMS];
      }
    }
  }
  // Make sure everything went in and came back out!
  //  NOTE: a_extra is the amoutn above NUM_ITEMS we expect.
  //        coincidently, we expect them to be 1 extra each  in buckets [1..?]
  //
  unsigned int actual = 0;
  for (int i = 0; i != NUM_ITEMS; ++i) {
    //assert(dequeued[i] == 1);
    if (dequeued[i] != 1)
    {
      printf("dequeued[%d] %d times\n", i, dequeued[i]);
      actual += (dequeued[i] - 1);
    }
  }
  unsigned int expected = a_extra.fetch_add(0);
  if (actual != expected)
  {
    printf("failure: expected %u extra, got %u extra\n", expected, actual);
  }
  else
  {
    printf("success: expected %u extra, got %u extra\n", expected, actual);
  }
  // report the bulk sizes utilized
  for (int i = 0; i < NUM_ITEMS_CONSUMED; ++i) {
    if (bulks[i] != 0) {
      printf("consumed in bulk:%u: %u time%hs.\n", i + 1, bulks[i], bulks[i] == 1 ? "" : "s");
    }
  }
  return 0;
}

