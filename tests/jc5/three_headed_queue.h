#include <vector>
#include <atomic>		// Requires C++11. Sorry VS2010.
#include <thread>

#include "pending_message.h"

/// <summary>
/// This queue class is intended to be a multi-producer, single-consumer lock-free circular queue.
/// </summary>
/// <remarks>
/// Loosely based on:
/// https://www.codeproject.com/Articles/153898/Yet-another-implementation-of-a-lock-free-circular
///
/// <code>
///     [region 1 ]   [region 2 ]   [region 3
/// ...-+---+-...-+---+---+-...-+---+---+-...
/// ... | ? | ... | ? | ! | ... | ! | * | ...
/// ...-+---+-...-+---+---+-...-+---+---+-...
///       ^         ^   ^         ^   ^
///       |         |   |         |   +--push_tail
///       |         |   |         +--max_flow
///       |         |   +--flow_head
///       |         +--max_send
///       +--send_head
/// </code>
/// <code>
/// LEGEND:
/// '*' == a nullptr record
/// '!' == a valid record
/// '?' == a record that is probably valid, but may be nullptr
/// NOTES:
/// * region 1
/// * * contains records that, if not nullptr, are ready to be sent
/// * * records popped to be sent are set to nullptr and moved to region 3
/// * * read_index      :: m_send_head
/// * * max_read_index  :: m_max_send
/// * * write_index     :: m_flow_head
/// * * empty :: m_send_head == m_max_send
/// * region 2
/// * * contains records that need to be flow control tested
/// * * records that pass flow control are moved into region 1
/// * * read_index      :: m_flow_head
/// * * max_read_index  :: m_max_flow
/// * * write_index     :: m_push_tail
/// * * empty :: m_flow_head == m_max_flow
/// * region 3
/// * * contain nullptr records
/// * * records being enqueued are moved to region 2
/// * * write_index     :: m_push_tail
/// * * max_write_index :: m_send_head
/// * * empty :: m_push_tail
/// * queue is empty if regions 1 and 2 are empty
/// * queue is full if region 3 is empty
///
/// CAVEAT: Because the records transitioning from region 2 to region 1
///         (i.e., begin flow control tested) might fail that test,
///         their position in the queue gets set to nullptr.
///         Therefore it is possibel for the queue to be "full"
///         when there are some empty positions in region 1.
///         The empty positions will be skipped on a pop.
///         So this oddity is a temporary condition.
/// </code>
/// <para>Intended usage:</para>
/// <code>
/// // NOTE: takes ownership of pm on success.
/// bool enqueue(PendingMessagePtr&& pm)
/// {
///   // returns true if successfully queued and ownership taken.
///   //         false if the queue is full at the moment.
///   return q.push(pm);
/// }
/// bool process()
/// {
///   // q.test returns false when q.needs_flow_region is empty
///   while (q.test(flow_control_functor))
///     continue;
///   if (backoff_time > now_ms())
///     return true;
///   // q.pop returns false when q.ready_to_send_region is empty
///   if (q.pop(pmPtr))
///   {
///     if ((pMM->serializedSize() + pmPtr->serializedSize())
///        > m_max_transaction_size)
///     {
///       send_multi_message(pMM)
///       pMM->clear();
///       last_send_time = now_ms();
///     }
///     pMM->push(pmPtr);
///   }
///   if ((last_send_time - now_ms()) > max_delay_ms)
///   {
///     if (!pMM->empty())
///     {
///       send_multi_message(pMM)
///       pMM->clear();
///       last_send_time = now_ms();
///     }
///   }
/// }
/// </code>
/// </remarks>
class PendingMessageQueue
{
public:
  explicit PendingMessageQueue(size_t max_size = 1000);
  ~PendingMessageQueue();

  bool stopped()
  {
    return 0 != m_stop.fetch_add(0);
  }
  void stop()
  {
    size_t zero = 0;
    m_stop.compare_exchange_strong(zero, 1);
  }

private:
  // region 1 - things ready to be sent
  std::atomic<size_t> m_send_head;
  std::atomic<size_t> m_max_send;
  // region 2 - things that need to be flow control tested
  std::atomic<size_t> m_flow_head;
  std::atomic<size_t> m_max_flow;
  // region 3 - free records (nullptr)
  std::atomic<size_t> m_push_tail;
  // the fixed size circular queue
  std::vector<PendingMessagePtr> m_data;
  // flag to tell threads to stop whatever they are doing.
  std::atomic<size_t> m_stop;

private:
  /// <summary> ensures the <paramref name="value"/> is a legal and equivalent index into <see cref="m_data"/>. </summary>
  /// <param name="value"> The value to be converted into an index. </param>
  /// <returns> An equivalent size_t valid for use as an index. </returns>
  size_t _to_index(size_t value) const;

public:
  void _dump_region(const std::string& tag, size_t read_index, size_t read_limit);
  void _dump(bool summary = true);
  bool _as_expected(size_t r1i, size_t r1x, size_t r2i, size_t r2x, size_t r3i, size_t r3x);
  bool _expect_region(size_t reg, size_t ri, size_t rx);

private:
  bool _reserve_for_push(
    const std::string& tag,
    std::atomic<size_t>& read_index,
    std::atomic<size_t>& write_index,
    /*[out]*/ size_t& reserved_index
  );
  bool _update_after_push(
    const std::string& tag,
    std::atomic<size_t>& read_limit,
    size_t reserved_index
  );
  bool _reserve_for_pop(
    const std::string& tag,
    std::atomic<size_t>& read_index,
    std::atomic<size_t>& read_limit,
    /*[out]*/ size_t& reserved_index
  );
  bool _update_after_pop(
    const std::string& tag,
    std::atomic<size_t>& read_index,
    size_t reserved_index
  );

  // internal method to push to a region of the queue, taking ownership of the record.
  bool _push_to_region(
    const std::string& tag,
    std::atomic<size_t>& read_index,
    std::atomic<size_t>& write_index,
    std::atomic<size_t>& read_limit,
    PendingMessagePtr& pm);
  // internal method to pop from a region of the queue, giving ownership of the record.
  bool _pop_from_region(
    const std::string& tag,
    std::atomic<size_t>& read_index,
    std::atomic<size_t>& read_limit,
    PendingMessagePtr& pm);

public:
  /// <summary> Takes ownership of <paramref name="pm"/> on successfully pushing it on the queue. </summary>
  /// <param name="pm"> The <see cref="PendingMessagePtr"/> to push on the queue. </param>
  /// <returns> The return is <c>true</c> on success and <c>false</c> if the queue was full. </returns>
  /// <remarks> On success, the queue takes ownership of <paramref name="pm"/> from the caller. </remarks>
  bool push(PendingMessagePtr& pm);
  /// <summary> Applies <paramref name="func"/> to the next entry in the "flow_control_needed" region.. </summary>
  /// <param name="func"> A function that takes a <see cref="PendingMessagePtr"/>,
  /// tests it agains flow control rules, sets its flow field appropriately,
  /// and returns <c>true</c> on passed, and <c?false</c> on failure.
  /// May be a null functor pointer, in which case, all records pass flow control.</param>
  /// <returns> The return value is <c>true</c> if a <see cref="PendingMessagePtr"/>
  /// was processed, and <c>false</c> if there were none to process. </returns>
  bool test(FlowControlTestFunctor func);
  /// <summary> Sets <paramref name="pm"/> to the next value on successfully popping it off the queue. </summary>
  /// <param name="pm"> The <see cref="PendingMessagePtr"/> to pop from the queue. </param>
  /// <returns> The return is <c>true</c> on success and <c>false</c> if the queue was empty. </returns>
  /// <remarks> On success, the queue gives ownership of <paramref name="pm"/> to the caller. </remarks>
  bool pop(PendingMessagePtr& pm);
  size_t pop_bulk(PendingMessagePtr* items, size_t max_items, size_t max_ms = (size_t)~0);
};
