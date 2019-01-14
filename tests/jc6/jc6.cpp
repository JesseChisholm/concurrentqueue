// jc6.cpp : Defines the entry point for the console application.
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

#include "pending_message.h"
typedef ConcurrentQueue<PendingMessagePtr> PendingMessageQueue;

#define NUM_PRODUCERS 10
#define NUM_ITEMS_PRODUCED 10
#define NUM_ITEMS (NUM_PRODUCERS * NUM_ITEMS_PRODUCED)
#define NUM_CONSUMERS 10
#define NUM_ITEMS_CONSUMED 50
#define NUM_TIMES_CONSUMED (NUM_ITEMS / NUM_ITEMS_CONSUMED)
#define NUM_THREADS (NUM_PRODUCERS + NUM_CONSUMERS)
#define MAX_DEQUEUE_MS 100

size_t my_rand(unsigned int limit)
{
  size_t number;
  rand_s(&number);
  // because the high bits are more random than the low bits
  //  we shouldn't simply: return number % limit;
  double value = (((double)number / (double)((size_t)~0)) * (double)limit);
  return (size_t)value;
}

std::atomic<size_t> a_stopped = 0;
std::atomic<size_t> a_produced = 0;
std::atomic<size_t> a_consumed = 0;
std::atomic<size_t> a_dropped = 0;
std::atomic<size_t> dequeued[NUM_ITEMS] = { 0 };
std::atomic<size_t> bulks[NUM_ITEMS_CONSUMED] = { 0 };

/// <summary> Place holder for testing flow control rules. </summary>
/// <param name="pm"> The <c>shared_ptr</c> to the <see cref="PendingMessage"/> to test. </param>
/// <returns> The return value is <c>true</c> if this record passes flow control rules;
/// and <c>false</c> if it fails all flow control rules. </returns>
/// <remarks> <c>NOTE</c>: the record itself is modified if it passed the test. </remarks>
bool flow_control(PendingMessagePtr pm)
{
  // safety net
  if (!pm || !pm->envelope)
  {
    fprintf(stdout, "error: flow_control: NULL argument: pm\n");
    return false;
  }
  // only test this pm once!
  if (!!pm->envelope->flow)
  {
    fprintf(stdout, "warning: flow_control: duplicate test of .id = %d\n", pm->id);
    return true;
  }

  size_t test = my_rand(100);
  // about 2% fail, 98% pass
  if (test > 1)
  {
    // modify pm by setting the envelope.flow.fields for the rule that says it passed.
    pm->envelope->flow = std::make_shared<FlowDetails>();
    // 1/3rd of the time use machine-participation
    // 2/3rd of the time use message-sampling
    pm->envelope->flow->reason = (my_rand(100) <= 33) ? FlowReason::Participation : FlowReason::Sampling;
    // really should be 98%, but for this PLACE HOLDER, this is prettier
    pm->envelope->flow->chance = test / 100.0f;

    return true;
  }
  return false;
}

/// <summary>
/// Pops at most <paramref name="bulk_size"/> records from the queue.
/// <para><c>NOTE</c>: records that fail <see cref="flow_control"/> are dropped, and not returned by this method.</para>
/// </summary>
/// <param name="q"> A reference to the <see cref="PendingMessageQueue"/> in use. </param>
/// <param name="items"> The address of the first position in an <c>array</c>
/// of <see cref="PendingMessagePtr"/> to hold the records popped and passing <see cref="flow_control"/>. </param>
/// <param name="bulk_size"> The number of position available in <paramref name="items"/>. </param>
/// <param name="max_dequeue_ms"> The maximum number of milliseconds to attempt to acquire <paramref name="bulk_items"/> records. </param>
/// <returns> The return value is the number of records in <paramref name="items"/> actually being returned. </returns>
size_t pop_bulk(PendingMessageQueue& q, PendingMessagePtr* items, size_t bulk_size, size_t max_dequeue_ms)
{
  SystemTime started = getSystemTime();
  std::size_t count;
  std::size_t wanted = bulk_size;
  std::size_t result = 0;
  while ((wanted > 0) && (getTimeDelta(started) < max_dequeue_ms))
  {
    count = q.try_dequeue_bulk(&items[result], wanted);
    if (count == 0)
    {
      // back off a bit if nothing available at the moment.
      sleep(10);
    }
    else
    {
      size_t take = 0;
      for (size_t off = 0; off < count; ++off)
      {
        if (flow_control(items[result + off]))
        {
          // accept this into bulk
          if (take != off)
            items[result + take] = items[result + off];
          take++;
        }
        else
        {
          // reject this for bulk
          a_dropped.fetch_add(1);
        }
      }
      if (take > 0)
      {
        //printf("consume :%u: in chunk %u /%u\n", thread_id, take, wanted);
        result += take;
        wanted -= take;
      }
    }
  }
  return result;
}

size_t process_in_bulk(size_t thread_id, PendingMessageQueue& q, size_t bulk_size, size_t max_dequeue_ms)
{
  SystemTime started = getSystemTime();
  PendingMessagePtr* items = new PendingMessagePtr[bulk_size];
  size_t result = pop_bulk(q, items, bulk_size, max_dequeue_ms);
  if (result > 0)
  {
    // NOTE: these are the ones that passed flow control rules.
    printf("consume :%u: in bulk %u /%u\n", thread_id, result, bulk_size);
    a_consumed.fetch_add(result);
    bulks[result - 1].fetch_add(1);
    for (size_t count = result; count != 0; --count)
    {
      if (!!items[count - 1])
      {
        dequeued[items[count - 1]->id % NUM_ITEMS].fetch_add(1);
        items[count - 1] = nullptr; // done with this item
      }
    }
  }
  delete[] items;
  return result;
}
bool process_singularly(size_t thread_id, PendingMessageQueue& q)
{
  PendingMessagePtr item;
  if (q.try_dequeue(item))
  {
    if (!!item) // skip null records
    {
      // test flow control rules.
      if (!flow_control(item))
      {
        a_dropped.fetch_add(1);
      }
      else
      {
        a_consumed.fetch_add(1);
        bulks[0].fetch_add(1);
        printf("consume :%u: .id = %d\n", thread_id, item->id);
        dequeued[item->id % NUM_ITEMS].fetch_add(1);
      }
      item = nullptr; // done with this record.
    }
    return true;
  }
  return false;
}
bool produce_singularly(size_t thread_id, PendingMessageQueue& q)
{
  bool ok;
  int delay;
  PendingMessagePtr pm = createPendingMessagePtr();
  printf("produce :%u: .id=%d\n", thread_id, pm->id);
  // we MUST enqueue!
  while (0 == a_stopped.fetch_add(0))
  {
    ok = q.try_enqueue(pm);
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
  return true;
}

int main()
{
  //ConcurrentQueue<PendingMessagePtr> q;
  PendingMessageQueue q;// (1000, NUM_PRODUCERS, 0);
  std::thread threads[NUM_THREADS];

  // enqueue singularly
  // Producers
  for (int i = 0; i != NUM_PRODUCERS; ++i) {
    threads[i] = std::thread([&](int thread_index) {
      int delay = 0;
      bool ok;
      while (0 == a_stopped.fetch_add(0))
      {
        ok = produce_singularly(thread_index, q); // ignore return
      }
      printf("producing :%u: done\n", thread_index);
    }, i);
  }

  // dequeue bulkily
  // Consumers
  for (int i = NUM_PRODUCERS; i != NUM_THREADS; ++i)
  {
    threads[i] = std::thread([&](int thread_index) {
      size_t processed;
      printf("consuming :%u:\n", thread_index);
      while (0 == a_stopped.fetch_add(0))
      {
        processed = process_in_bulk(thread_index, q, NUM_ITEMS_CONSUMED, MAX_DEQUEUE_MS);
        //printf("consume :%u: in bulk %u /%u\n", i, processed, NUM_ITEMS_CONSUMED);
      }
      printf("consuming :%u: done\n", thread_index);
    }, i);
  }
  //// dequeue singularly
  //// Consumers
  //for (int i = NUM_PRODUCERS; i != NUM_THREADS; ++i) {
  //  threads[i] = std::thread([&](int thread_index) {
  //    printf("consuming :%u:\n", thread_index);
  //    while (0 == a_stopped.fetch_add(0))
  //    {
  //      process_singularly(thread_index, q); // ignore return
  //    }
  //    printf("consuming :%u: done\n", thread_index);
  //  }, i);
  //}

  // Wait for all threads
  sleep(10000);
  printf("---------- waiting for %u threads\n", NUM_THREADS);
  a_stopped.fetch_add(1);
  for (int i = 0; i != NUM_THREADS; ++i) {
    threads[i].join();
  }
  printf("---------- waited  for %u threads\n", NUM_THREADS);

  // dequeue leftovers bulkily
  // Collect any leftovers (could be some if e.g. consumers finish before producers)
  size_t leftover = 0;
  size_t tail_chunk = 0;
  while(0 < (tail_chunk = process_in_bulk(NUM_THREADS, q, NUM_ITEMS_CONSUMED, MAX_DEQUEUE_MS)))
  {
    //printf("debug: leftover chunk %u\n", last_chunk);
    leftover += tail_chunk;
  }
  //// dequeue leftovers singularly
  //// Collect any leftovers (could be some if e.g. consumers finish before producers)
  //while (process_singularly(NUM_THREADS, q))
  //{
  //  leftover += 1; // BOGUS! counts dropped as if consumed.
  //  continue;
  //}
  printf("debug: leftovers %u\n", leftover);

  // Make sure everything went in and came back out!
  //  a_produced is the total amount actually produced.
  //  a_consumed is the total amount actually consumed.
  //  actual     will be the number we counted as consumed.
  // all should be equal!
  //
  size_t actualC = 0;
  for (int i = 0; i != NUM_ITEMS; ++i)
  {
    int N = dequeued[i].fetch_add(0);
    printf("dequeued[%d] %d times\n", i, N);
    actualC += N;
  }
  size_t expectedP = a_produced.fetch_add(0);
  size_t expectedC = a_consumed.fetch_add(0);
  size_t droppedC = a_dropped.fetch_add(0);
  if (((actualC + droppedC) != expectedP) || (actualC != expectedC))
  {
    printf("failure: expected %u produced, expected %u consumed, got %u consumed, %u dropped (%5.2f%%), missing %d\n", expectedP, expectedC, actualC, droppedC, droppedC*100.0 / expectedP, expectedP - (actualC + droppedC));
  }
  else
  {
    printf("success: expected %u produced, expected %u consumed, got %u consumed, %u dropped (%5.2f%%)\n", expectedP, expectedC, actualC, droppedC, droppedC*100.0/ expectedP);
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

