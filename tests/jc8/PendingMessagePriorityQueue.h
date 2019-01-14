#pragma once
#include <vector>
#include "pending_message.h"
#include "../../concurrentqueue.h"
using namespace moodycamel;
#include <atomic>

/// <summary>
/// A class that simulates a priority queue,
/// based on <see cref="PendingMessagePriority"/>. 
/// </summary>
class PendingMessagePriorityQueue
{
public:
  /// <summary>
  /// Constructor takes the maximum queue size.
  /// </summary>
  /// <param name="max_enqueued"> The maximum number of items that may be enqueued at any point in time. </param>
  /// <param name="num_priority_levels"> The number of different priority levels allowed. </param>
  /// <remarks>
  /// <c>CAVEAT</c>: Memory occupies a multiple of <paramref name="max_enqueued"/>
  /// </remarks>
  PendingMessagePriorityQueue(size_t max_enqueued, size_t num_priority_levels = 1)
  {
    m_max_enqueued = max_enqueued;
    m_num_priorities = num_priority_levels;
    //
    // any priority level might have the maximum number of items.
    //
    m_Qs.resize(num_priority_levels);
    for(size_t i=0; i<num_priority_levels; ++i)
      m_Qs[i] = std::make_shared<InnerQueue>(max_enqueued);
  }
  ~PendingMessagePriorityQueue()
  {
    // TODO: if (count() > 0) LOG("Lost %u items in descructor.", count());
  }

public:
  /// <summary>
  /// Get the current <c>FULL</c> state of this <see cref="PendingMessagePriorityQueue"/>.
  /// <para><c>CAVEAT</c>: Another thread might change this.  It is a momentary estimate only. </para>
  /// </summary>
  bool is_queue_full() const
  {
    return count() >= max_queue_size();
  }
  /// <summary>
  /// Get the current <c>EMPTY</c> state of this <see cref="PendingMessagePriorityQueue"/>.
  /// <para><c>CAVEAT</c>: Another thread might change this.  It is a momentary estimate only. </para>
  /// </summary>
  bool is_queue_empty() const
  {
    return count() == 0;
  }
  /// <summary>
  /// Get the maximum number of items allowed in this <see cref="PendingMessagePriorityQueue"/>.
  /// </summary>
  uint64_t max_queue_size() const
  {
    return std::atomic_load(&m_max_enqueued);
  }
  /// <summary>
  /// Get the (momentary) count of items in this <see cref="PendingMessagePriorityQueue"/>.
  /// <para><c>CAVEAT</c>: Another thread may push or pop immediately after this call,
  /// so the value is really an estimate, not a hard fact.
  /// </summary>
  uint64_t count() const
  {
    return std::atomic_load(&m_queuedCount);
  }
  /// <summary>
  /// Try and push at the requested priority.
  /// </summary>
  /// <param name="pm"> The <see cref="PendingMessagePtr"/> to be pushed into the queue. </param>
  /// <param name="priority"> The priority level for this item.
  /// <para>Zero is minimum (and default) priority. </para>
  /// <para><c>num_priorities - 1</c> (Set in the constructor) is the highest priority. </para>
  /// </param>
  /// <returns>
  /// The return value is <c>false</c> if the queue is full at the moment.
  /// The return value is <c>true</c> if <paramref name="pm"/> has been pushed to the queue.
  /// </returns>
  bool try_enqueue(PendingMessagePtr& pm, size_t priority = 0)
  {
    if (count() >= max_queue_size())
      return false;

    size_t max_priority = m_num_priorities.load() - 1;
    size_t p = (priority <= max_priority) ? priority : max_priority;
    auto& q = m_Qs[p];
    if (q->try_enqueue(pm))
    {
      std::atomic_fetch_add(&m_queuedCount, 1);
      //printf("debug: enqueued %u@%u\n", pm->id, p);
      return true;
    }
    return false;
  }
  /// <summary>
  /// Try and push at the requested priority.
  /// </summary>
  /// <param name="items"> The starts of an array of <see cref="PendingMessagePtr"/> to be pushed into the queue. </param>
  /// <param name="num_items"> The number of <see cref="PendingMessagePtr"/> in <paramref name="items"/>. </param>
  /// <param name="priority"> The priority level for these items.
  /// <para>Zero is minimum (and default) priority. </para>
  /// <para><c>num_priorities - 1</c> (Set in the constructor) is the highest priority. </para>
  /// </param>
  /// <returns>
  /// The return value is <c>false</c> if the queue is too full at the moment.
  /// The return value is <c>true</c> if all <paramref name="items"/> have been pushed to the queue.
  /// </returns>
  bool try_enqueue_bulk(PendingMessagePtr* items, size_t num_items, size_t priority = 0)
  {
    if (count() >= max_queue_size())
      return false;

    size_t max_priority = m_num_priorities.load() - 1;
    size_t p = (priority <= max_priority) ? priority : max_priority;
    {
      auto& q = m_Qs[p];
      if (q->try_enqueue_bulk(items, num_items))
      {
        std::atomic_fetch_add(&m_queuedCount, num_items);
        printf("debug: enqueued #%u@%u\n", num_items, p);
        return true;
      }
    }
    return false;
  }
  /// <summary>
  /// Try and pop an item from the queue.
  /// Higher priority are popped before lower priority.
  /// </summary>
  /// <param name="pm"> The <see cref="PendingMessagePtr"/> to be set from the queue. </param>
  /// <returns>
  /// The return value is <c>false</c> if the queue is empty at the moment.
  /// The return value is <c>true</c> if <paramref name="pm"/> has been popped from the queue.
  /// </returns>
  bool try_dequeue(PendingMessagePtr& pm)
  {
    if (count() == 0)
      return false;

    size_t num_priority = m_num_priorities.load();
    for (size_t p1 = num_priority; p1 > 0; --p1)
    {
      auto& q = m_Qs[p1-1];
      if (q->try_dequeue(pm))
      {
        std::atomic_fetch_sub(&m_queuedCount, 1);
        //printf("debug: dequeued %u@%u\n", pm->id, p);
        return true;
      }
    }
    return false;
  }
  size_t try_dequeue_bulk(PendingMessagePtr* items, size_t max_items)
  {
    if (count() == 0)
      return false;

    size_t got_items = 0;
    size_t num_priority = m_num_priorities.load();
    for (size_t p1 = num_priority; p1 > 0; --p1)
    {
      auto& q = m_Qs[p1-1];
      if (0 < (got_items = q->try_dequeue_bulk(items, max_items)))
      {
        std::atomic_fetch_sub(&m_queuedCount, got_items);
        //printf("debug: dequeued #%u@%u\n", got_items, p);
        return got_items;
      }
    }
    return 0;
  }

protected:
  std::atomic_uint64_t m_max_enqueued;
  std::atomic_uint64_t m_queuedCount;
  std::atomic<size_t> m_num_priorities;
  typedef ConcurrentQueue<PendingMessagePtr> InnerQueue;
  typedef std::shared_ptr<InnerQueue> InnerQueuePtr;
  typedef std::vector<InnerQueuePtr> PrioritizedQueue;
  PrioritizedQueue m_Qs;
};

