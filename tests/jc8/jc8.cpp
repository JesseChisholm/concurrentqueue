// jc8.cpp : Defines the entry point for the console application.
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
#include "PendingMessagePriorityQueue.h"

// How many producer threads to create
#define NUM_PRODUCERS 15
// How many consumer threads to create
#define NUM_CONSUMERS 1
// Total number of threads
#define NUM_THREADS (NUM_PRODUCERS + NUM_CONSUMERS)
// Maximum number of items allowed to be enqueued at one time.
#define MAX_ENQUEUED_ITEMS 1000
// How many `dequeued` buckets to count things in
#define NUM_ITEMS 100
// How many items does consumer threads try to pop at once
#define NUM_ITEMS_CONSUMED 50
// How many ms does consumer thread try to fill its NUM_ITEMS_CONSUMED
#define MAX_DEQUEUE_MS 100
// How many priority levels to test with
#define NUM_PRIORITIES 3
// Maximum number of milliseconds an item is allowed to wait in the queue.
#define MAX_TIME_OFFSET_MS 60000.0  // 1 minute
//#define MAX_TIME_OFFSET_MS 100.0    // 100 ms

size_t my_rand(unsigned int limit)
{
  size_t number;
  rand_s(&number);
  // because the high bits are more random than the low bits
  //  we shouldn't simply: return number % limit;
  double value = (((double)number / (double)((size_t)~0)) * (double)limit);
  return (size_t)value;
}

std::atomic<size_t> a_stop_producing = 0;
std::atomic<size_t> a_stop_consuming = 0;
std::atomic<size_t> a_produced = 0;
std::atomic<size_t> a_consumed = 0;
std::atomic<size_t> a_dropped = 0;
std::atomic<size_t> dequeued[NUM_ITEMS] = { 0 };
std::atomic<size_t> bulks[NUM_ITEMS_CONSUMED] = { 0 };
std::atomic<double> a_max_time_offset = 0.0;

/// <summary> Simple functor declaration. </summary>
/// <param name="p"> The <see cref="PendingMessagePtr"/> being tested. </param>
/// <returns> The return is <c>true</c> if <paramref name="p"/> passes this filter, <c>false</c> if it fails. </returns>
/// <remarks> <c>CAVEAT</c>: The filter function <u>is allowed</u> to modify the <see cref="PendingMessage"/> if it needs to.  Just be aware of any threading issues.</remarks>
typedef bool(*take_filter_t)(PendingMessagePtr pm);

bool accept_all(PendingMessagePtr pm)
{
  return true;
}

/// <summary> Place holder for testing time_offset exceeded rule. </summary>
/// <param name="pm"> The <c>shared_ptr</c> to the <see cref="PendingMessage"/> to test. </param>
/// <returns> The return value is <c>false</c> if this record has been waiting too long;
/// and <c>true</c> if it is still young enough to use. </returns>
bool within_allotted_time(PendingMessagePtr pm)
{
  // safety net
  if (!pm || !pm->envelope || !pm->envelope->context)
  {
    fprintf(stdout, "error: within_allotted_time: NULL argument: pm\n");
    return false;
  }
  static double maxTimeOffset = MAX_TIME_OFFSET_MS;
  double diff_time = getTimeDelta(pm->queuedTime);
  if (diff_time < maxTimeOffset)
  {
    return true;
  }
  //fprintf(stdout, "info: within_allotted_time: rejected .id = %d\n", pm->id);
  return false;
}

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
  //fprintf(stdout, "info: flow_control: rejected .id = %d\n", pm->id);
  return false;
}

bool McArthyAnd(PendingMessagePtr& pm) {
  return true;
}

template <typename UnaryPredicateFunctor>
bool McArthyAnd(PendingMessagePtr& pm, UnaryPredicateFunctor filter) {
  return filter(pm);
}

template <typename FirstFunctor, typename... FunctorList>
bool McArthyAnd(PendingMessagePtr& pm, FirstFunctor filter, FunctorList... funcs) {
  return filter(pm) && McArthyAnd(pm, funcs...);
}


/// <summary>
/// Even though the name is <c>..>Filtered</c> there are no filters
/// and this method just takes N if available within max_dequeue_ms.
/// See also the templated version of TakeNFiltered, which does apply filters.
/// </summary>
/// <param name="q"> The <see cref="PendingMessagePriorityQueue"/> to pop from. </param>
/// <param name="items"> The <see cref="PendingMessagePtr"/>[] to pop into. </param>
/// <param name="bulk_size"> The number of items desired. </param>
/// <param name="max_dequeue_ms"> The maximum number of millisecodns to try and pop more items. </param>
/// <returns> The number of actual items popped. </returns>
size_t TakeNFiltered(PendingMessagePriorityQueue& q,
                     PendingMessagePtr* items,
                     size_t bulk_size,
                     size_t max_dequeue_ms)
{
  SystemTime started = getSystemTime();
  std::size_t count;
  std::size_t wanted = bulk_size;
  std::size_t result = 0;
  size_t lost = 0;
  PendingMessagePtr test;
  while ((wanted > 0) && (getTimeDelta(started) < max_dequeue_ms))
  {
    count = q.try_dequeue_bulk(&items[result], wanted);
    if (count == 0)
    {
      // back off a bit if nothing available at the moment.
      sleep(1);
      continue;
    }
    // NOTE: no filters means always take
    //printf("consume: %u/%u\n",  count, wanted);
    result += count;
    wanted -= count;
  }
  return result;
}

/// <summary>
/// Takes N if available within max_dequeue_ms, and they pass all filters.
/// </summary>
/// <param name="q"> The <see cref="PendingMessagePriorityQueue"/> to pop from. </param>
/// <param name="items"> The <see cref="PendingMessagePtr"/>[] to pop into. </param>
/// <param name="bulk_size"> The number of items desired. </param>
/// <param name="max_dequeue_ms"> The maximum number of millisecodns to try and pop more items. </param>
/// <param name="filters"> Zero or more <c>UnaryPredicateFunctor</c> filters to apply. </param>
/// <returns> The number of actual items popped. </returns>
/// <remarks>
/// There is a log line for each <see cref="PendingMessagePtr"/> popped and dropped
/// because it failed one of the filters.
/// </remarks>
template<typename... Filters>
size_t TakeNFiltered(PendingMessagePriorityQueue& q,
                     PendingMessagePtr* items,
                     size_t bulk_size,
                     size_t max_dequeue_ms,
                     Filters... filters)
{
  SystemTime started = getSystemTime();
  std::size_t count;
  std::size_t wanted = bulk_size;
  std::size_t result = 0;
  size_t lost = 0;
  PendingMessagePtr test;
  while ((wanted > 0) && (getTimeDelta(started) < max_dequeue_ms))
  {
    count = q.try_dequeue_bulk(&items[result], wanted);
    if (count == 0)
    {
      // back off a bit if nothing available at the moment.
      sleep(1);
      continue;
    }
    size_t take = 0;
    for (size_t off = 0; off < count; ++off)
    {
      if (!McArthyAnd(items[result + off], filters...))
      {
        //fprintf(stdout, "warning: TakeNFiltered: rejected .id = %d\n", items[result + off]->id);
        // reject this for bulk
        a_dropped.fetch_add(1);
        lost++;
        continue;
      }
      // accept this into bulk
      if (take != off)
        items[result + take] = items[result + off];
      take++;
    }
    if (take > 0)
    {
      //printf("consume: %u/%u\n",  take, wanted);
      result += take;
      wanted -= take;
    }
  }
  return result;

  //
  // NOTE: SINGLE POP AT A TIME VERSION, left for posterity!
  //
  /*
  SystemTime started = getSystemTime();
  std::size_t wanted = bulk_size;
  std::size_t result = 0;
  size_t lost = 0;
  PendingMessagePtr test;
  while ((wanted > 0) && (getTimeDelta(started) < max_dequeue_ms))
  {
    if (q.try_dequeue(test))
    {
      // NOTE: This verison of TakeNFiltered tests all filters given.
      //
      if (!McArthyAnd(test, filters...))
      {
        //fprintf(stdout, "warning: TakeNFiltered: rejected .id = %d\n", test->id);
        a_dropped.fetch_add(1);
        ++lost;
        continue;
      }
      items[result] = test;
      ++result;
      --wanted;
    }
  }
  return result;
  */
}


/// <summary>
/// Pops at most <paramref name="bulk_size"/> records, that pass the filter, from the queue.
/// <para><c>NOTE</c>: records that fail <paramref name="filter"/> are dropped, and not returned by this method.</para>
/// </summary>
/// <param name="q"> A reference to the <see cref="PendingMessageQueue"/> in use. </param>
/// <param name="items"> The address of the first position in an <c>array</c>
/// of <see cref="PendingMessagePtr"/> to hold the records popped and passing <paramref name="filter"/>. </param>
/// <param name="bulk_size"> The number of positions available in <paramref name="items"/>. </param>
/// <param name="max_dequeue_ms"> The maximum number of milliseconds to attempt to acquire <paramref name="bulk_size"/> records. </param>
/// <returns> The return value is the number of records in <paramref name="items"/> actually being returned. </returns>
size_t pop_bulk_filtered(PendingMessagePriorityQueue& q, PendingMessagePtr* items, size_t bulk_size, size_t max_dequeue_ms, take_filter_t filter)
{
  return TakeNFiltered(q, items, bulk_size, max_dequeue_ms, filter);
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
size_t pop_bulk(PendingMessagePriorityQueue& q, PendingMessagePtr* items, size_t bulk_size, size_t max_dequeue_ms)
{
  // expect 0% dropped
  //return TakeNFiltered(q, items, bulk_size, max_dequeue_ms);
  // expect 2% dropped
  //return TakeNFiltered(q, items, bulk_size, max_dequeue_ms, flow_control);
  // expect 2% + ?% dropped
  return TakeNFiltered(q, items, bulk_size, max_dequeue_ms, flow_control, within_allotted_time);
}

/// <returns> The maximum time_offset (milliseconds as a double) in this collection. </returns>
double process_collection(PendingMessagePtr* items, size_t bulk_size)
{
  // NOTE: these are the ones that passed all filter rules in the layer above.
  a_consumed.fetch_add(bulk_size);
  bulks[bulk_size - 1].fetch_add(1);
  double diff_time = 0.0;
  for (size_t count = bulk_size; count != 0; --count)
  {
    if (!!items[count - 1])
    {
      double item_diff = getTimeDelta(items[count - 1]->queuedTime);
      if (item_diff > diff_time)
        diff_time = item_diff;
      dequeued[items[count - 1]->id % NUM_ITEMS].fetch_add(1);
      items[count - 1] = nullptr; // done with this item
    }
  }
  return diff_time;
}

size_t process_in_bulk(size_t thread_id, PendingMessagePriorityQueue& q, size_t bulk_size, size_t max_dequeue_ms, double& max_time_offset)
{
  max_time_offset = 0.0;
  SystemTime started = getSystemTime();
  PendingMessagePtr* items = new PendingMessagePtr[bulk_size];
  // NOTE: `pop_bulk` is `pop_bulk_filtered` with `flow_control` as the filter.
  size_t result = pop_bulk(q, items, bulk_size, max_dequeue_ms);
  if (result > 0)
  {
    //printf("consume :%u: in bulk %u /%u\n", thread_id, result, bulk_size);
    max_time_offset = process_collection(items, result);
  }
  delete[] items;
  return result;
}
bool produce_singularly(size_t thread_id, PendingMessagePriorityQueue& q)
{
  int delay;
  PendingMessagePtr pm = createPendingMessagePtr();
  // TODO: This ought to be done by createPendingMessagePtr
  //       but pendingmessage.cpp doesn't include the moodycamel stuff.
  //       this file does.
  pm->queuedTime = getSystemTime();
  size_t priority = thread_id % NUM_PRIORITIES; // 0 .. (NUM_PRIORITIES-1)
  //printf("produce :%u@%u: .id=%d\n", thread_id, priority, pm->id);
  // we MUST enqueue!
  while (0 == a_stop_producing.fetch_add(0))
  {
    if (!q.is_queue_full())
    {
      if (q.try_enqueue(pm, priority))
      {
        a_produced.fetch_add(1);
        break;
      }
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
  //  1st arg is maximum items queued at any given time. REQUIRED
  //  2nd arg is maximum number of priority level to manage. OPTIONAL, default == 1
  PendingMessagePriorityQueue q(MAX_ENQUEUED_ITEMS, NUM_PRIORITIES);
  std::thread threads[NUM_THREADS];

  SystemTime start_benchmark = getSystemTime();

  // enqueue singularly in many threads
  // Producers
  for (int i = 0; i != NUM_PRODUCERS; ++i) {
    threads[i] = std::thread([&](int thread_index) {
      size_t priority = thread_index % NUM_PRIORITIES; // 0 .. (NUM_PRIORITIES-1)
      printf("producing :%u@%u:\n", thread_index, priority);
      while (0 == a_stop_producing.fetch_add(0))
      {
        produce_singularly(thread_index, q); // ignore return
      }
      printf("producing :%u@%u: done\n", thread_index, priority);
    }, i);
  }

  // dequeue in bulk in one thread
  // Consumers
  for (int i = NUM_PRODUCERS; i != NUM_THREADS; ++i)
  {
    threads[i] = std::thread([&](int thread_index) {
      size_t processed;
      printf("consuming :%u:\n", thread_index);
      double chunk_time_offset;
      while (0 == a_stop_consuming.fetch_add(0))
      {
        processed = process_in_bulk(thread_index, q, NUM_ITEMS_CONSUMED, MAX_DEQUEUE_MS, chunk_time_offset);
        //printf("consume :%u: in bulk %u /%u\n", i, processed, NUM_ITEMS_CONSUMED);
        //
        // NOTE: This doesn't change the value if some other thread already chanegd it.
        //
        double old_value = std::atomic_load(&a_max_time_offset);
        a_max_time_offset.compare_exchange_strong(old_value, chunk_time_offset);
      }
      printf("consuming :%u: done\n", thread_index);
    }, i);
  }

  printf("---------- let %u threads process for 10 s\n", NUM_THREADS);
  sleep(10000);
  // Wait for all threads
  printf("---------- waiting for %u producer thread(s)\n", NUM_PRODUCERS);
  a_stop_producing.fetch_add(1);
  for (int i = 0; i != NUM_PRODUCERS; ++i) {
    threads[i].join();
  }
  printf("---------- waiting for %u consumer thread(s)\n", NUM_CONSUMERS);
  a_stop_consuming.fetch_add(1);
  for (int i = NUM_PRODUCERS; i < NUM_THREADS;  ++i) {
    threads[i].join();
  }
  printf("---------- waited  for %u threads\n", NUM_THREADS);

  // dequeue leftovers in bulk
  // Collect any leftovers (could be some if e.g. consumers finish before producers)
  size_t leftover = 0;
  bool process_leftovers = false;
  if (!q.is_queue_empty())
  {
    size_t tail_chunk = 0;
    PendingMessagePtr* items = new PendingMessagePtr[NUM_ITEMS_CONSUMED];
    while (0 < (tail_chunk = TakeNFiltered(q, items, NUM_ITEMS_CONSUMED, MAX_DEQUEUE_MS)))
    {
      if (process_leftovers)
      {
        printf("consume ::: in bulk %u /%u\n", tail_chunk, NUM_ITEMS_CONSUMED);
        process_collection(items, tail_chunk);
      }
      else
      {
        printf("lost ::: in bulk %u /%u\n", tail_chunk, NUM_ITEMS_CONSUMED);
        leftover += tail_chunk;
      }
    }
    delete[] items;
  }
  a_dropped.fetch_add(leftover);

  double benchmark_ms = getTimeDelta(start_benchmark);

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
    //printf("dequeued[%d] %d times\n", i, N);
    actualC += N;
  }
  size_t expectedP = a_produced.fetch_add(0);
  size_t expectedC = a_consumed.fetch_add(0);
  size_t droppedC = a_dropped.fetch_add(0);
  if (((actualC + droppedC) != expectedP) || (actualC != expectedC))
  {
    printf("failure: produced %u, expected %u consumed, got %u consumed, %u dropped (%5.2f%%), missing %d\n",
      expectedP,
      expectedC,
      actualC,
      droppedC,
      droppedC*100.0 / expectedP,
      expectedP - (actualC + droppedC));
  }
  else
  {
    printf("success: produced %u, expected %u consumed, got %u consumed, %u dropped (%5.2f%%)\n",
      expectedP,
      expectedC,
      actualC,
      droppedC,
      droppedC*100.0/ expectedP);
  }
  printf("notice:: ran for  %9.3f seconds\n", benchmark_ms / 1000.0);
  printf("notice:: produced %9.3f messages/s\n", expectedP*1000.0 / benchmark_ms);
  printf("notice:: consumed %9.3f messages/s\n", actualC*1000.0 / benchmark_ms);
  printf("notice:: dropped  %9.3f messages/s\n", droppedC*1000.0 / benchmark_ms);
  printf("\n");
  printf("notice:: flow_control expects to drop about 2%%\n");
  double actualT = a_max_time_offset.load();
  printf("notice:: within_allotted_time dropped %s, as the limit is %5.3f ms\n",
    (actualT < MAX_TIME_OFFSET_MS) ? "none" : "some",
    MAX_TIME_OFFSET_MS);
  printf("notice:: actual maximum time_offset was %6.3f ms\n", actualT);
  if (leftover > 0)
  {
    printf("notice:: There were %u leftover in the queue after all threads stopped.", leftover);
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
