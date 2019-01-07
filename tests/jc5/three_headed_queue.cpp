#include "stdafx.h"

#include "../common/systemtime.h"
//#include "../common/systemtime.cpp"
using namespace moodycamel;
#include "three_headed_queue.h"

PendingMessageQueue::PendingMessageQueue(size_t max_size /*= 1000*/)
{
  // region 1 - things ready to be sent
  //  [ m_send_head .. m_max_send )
  std::atomic_init(&m_send_head, 0U);
  std::atomic_init(&m_max_send, 0U);

  // region 2 - things that need to be flow control tested
  //  [ m_flow_head .. m_max_flow )
  std::atomic_init(&m_flow_head, 0U);
  std::atomic_init(&m_max_flow, 0U);

  // region 3 - free records (nullptr)
  //  [ m_push_tail .. m_send_head )
  std::atomic_init(&m_push_tail, 0U);

  // the fixed size circular queue
  //  one guard position at end of region 3
  m_data.resize(max_size + 1);

  _dump();
}
PendingMessageQueue::~PendingMessageQueue()
{
  size_t lost_index;

  // clear the needs_flow_control region!
  while (_reserve_for_pop("2", m_flow_head, m_max_flow, lost_index))
  {
    _update_after_pop("2", m_flow_head, lost_index);
    // TODO: log this lost record
    fprintf(stdout, "error: lost flow record %d\n", m_data[_to_index(lost_index)] ? m_data[_to_index(lost_index)]->id : (size_t)-1);
  }
  // clear the ready_to_be_send region!
  while (_reserve_for_pop("1", m_send_head, m_max_send, lost_index))
  {
    _update_after_pop("1", m_send_head, lost_index);
    // TODO: log this lost record
    fprintf(stdout, "error: lost send record %d\n", m_data[_to_index(lost_index)] ? m_data[_to_index(lost_index)]->id : (size_t)-1);
  }
}
size_t PendingMessageQueue::_to_index(size_t value) const
{
  return value % m_data.size();
}
void PendingMessageQueue::_dump_region(const std::string& tag, size_t read_index, size_t read_limit)
{
  for (size_t i = read_index; _to_index(i) != _to_index(read_limit); ++i)
  {
    printf("%s[%u==%u].id = %d\n", tag.c_str(), i, _to_index(i), m_data[_to_index(i)] ? m_data[_to_index(i)]->id : (size_t)-1);
  }
}
void PendingMessageQueue::_dump(bool summary /*= true*/)
{
  size_t head_send = m_send_head.fetch_add(0);
  size_t max_send = m_max_send.fetch_add(0);
  size_t head_flow = m_flow_head.fetch_add(0);
  size_t max_flow = m_max_flow.fetch_add(0);
  size_t head_push = m_push_tail.fetch_add(0);

  printf("region 1 [%u .. %u) %s\n", head_send, max_send, head_send==max_send ? "empty" : "");
  if (!summary)
    _dump_region("send", head_send, max_send);

  printf("region 2 [%u .. %u) %s\n", head_flow, max_flow, head_flow == max_flow ? "empty" : "");
  if (!summary)
    _dump_region("flow", head_flow, max_flow);

  printf("region 3 [%u .. %u) %s\n", head_push, ((head_send+m_data.size()-1)% m_data.size()), (head_push+1) == head_send ? "empty" : "");
  if (!summary)
    _dump_region("free", head_push, head_send);
}
bool PendingMessageQueue::_as_expected(size_t r1i, size_t r1x, size_t r2i, size_t r2x, size_t r3i, size_t r3x)
{
  size_t head_send = m_send_head.fetch_add(0);
  size_t max_send = m_max_send.fetch_add(0);
  size_t head_flow = m_flow_head.fetch_add(0);
  size_t max_flow = m_max_flow.fetch_add(0);
  size_t head_push = m_push_tail.fetch_add(0);

  if ((r1i == head_send) && (r1x == max_send)
    && (r2i == head_flow) && (r2x == max_flow)
    && (r3i == head_push) && (r3x == head_send))
  {
    return true;
  }
  fprintf(stdout, "error: _as_expected( [%d..%d) [%d..%d) [%d..%d) ) but [%d..%d) [%d..%d) [%d..%d)\n",
    r1i, r1x, r2i, r2x, r3i, r3x,
    head_send, max_send, head_flow, max_flow, head_push, head_send);
  return false;
}
bool PendingMessageQueue::_expect_region(size_t reg, size_t ri, size_t rx)
{
  size_t reg_index;
  size_t max_index;
  switch (reg)
  {
  case 1:
    reg_index = m_send_head.fetch_add(0);
    max_index = m_max_send.fetch_add(0);
    break;
  case 2:
    reg_index = m_flow_head.fetch_add(0);
    max_index = m_max_flow.fetch_add(0);
    break;
  case 3:
    reg_index = m_push_tail.fetch_add(0);
    max_index = m_send_head.fetch_add(0);
    break;
  default:
    reg_index = -1;
    max_index = -1;
    break;

  }

  if ((ri == reg_index) && (rx == max_index))
  {
    return true;
  }
  fprintf(stdout, "error: _expect_region( %d, [%d..%d) ) but [%d..%d)\n",
    reg, ri, rx,
    reg_index, max_index);
  return false;
}

/// <summary>
/// Internal: sets <paramref name="reserved_index"/> and increments <paramref name="write_index"/> on success.
/// </summary>
/// <param name="tag"> Debugging display name of this region of the queue. </param>
/// <param name="read_index"> A reference to the atomic variable this region would read from. </param>
/// <param name="write_index"> A reference to the atomic variable this region would write to. </param>
/// <param name="reserved_index"> Set on success to the index it is safe to write to. </param>
/// <returns> The return value is <c>true</c> if <paramref name="reserved_index"/> is valid;
/// and <c>false</c> if this region of the queue is full.
/// </returns>
bool PendingMessageQueue::_reserve_for_push(
  const std::string& tag,
  std::atomic<size_t>& read_index,
  std::atomic<size_t>& write_index,
  /*[out]*/ size_t& reserved_index
)
{
  size_t currentWriteIndex;
  size_t currentReadIndex;

  do
  {
    currentWriteIndex = write_index.fetch_add(0);
    currentReadIndex = read_index.fetch_add(0);
    if (_to_index(currentWriteIndex + 1) ==
      _to_index(currentReadIndex))
    {
      fprintf(stdout, "error: _reserve_for_push %s is full\n", tag.c_str());
      _dump(false);
      return false;
    }
  } while (!write_index.compare_exchange_strong(currentWriteIndex, currentWriteIndex + 1));

  reserved_index = currentWriteIndex;
  return true;
}
/// <summary>
/// Internal: increments <paramref name="read_limit"/> to include the <paramref name="reserved_index"/>.
/// </summary>
/// <param name="tag"> Debugging display name of this region of the queue. </param>
/// <param name="read_limit"> A reference to the atomic variable this region would stop reading from. </param>
/// <param name="reserved_index"> The index returned by <see cref="_reserve_for_push"/>. </param>
/// <returns> The return value is <c>true</c> when <paramref name="read_limit"/> has been updated.
/// </returns>
bool PendingMessageQueue::_update_after_push(
  const std::string& tag,
  std::atomic<size_t>& read_limit,
  size_t reserved_index
)
{
  // update the maximum read index after saving the data. It wouldn't fail if there is only one thread
  // inserting in the queue. It might fail if there are more than 1 producer threads because this
  // operation has to be done in the same order as the previous CES
  //
  size_t old_read_limit = read_limit.fetch_add(0);
  while (!read_limit.compare_exchange_strong(old_read_limit, (reserved_index + 1)))
  {
    // this is a good place to yield the thread in case there are more
    // software threads than hardware processors and you have more
    // than 1 producer thread
    // have a look at sched_yield (POSIX.1b)
    std::this_thread::yield();
    //sched_yield();
  }
  return true;
}
/// <summary>
/// Internal: sets <paramref name="reserved_index"/> and increments <paramref name="read_index"/> on success.
/// </summary>
/// <param name="tag"> Debugging display name of this region of the queue. </param>
/// <param name="read_index"> A reference to the atomic variable this region would read from. </param>
/// <param name="read_limit"> A reference to the atomic variable this region would stop reading from. </param>
/// <param name="reserved_index"> Set on success to the index it is safe to write to. </param>
/// <returns> The return value is <c>true</c> if <paramref name="reserved_index"/> is valid;
/// and <c>false</c> if this region of the queue is empty.
/// </returns>
bool PendingMessageQueue::_reserve_for_pop(
  const std::string& tag,
  std::atomic<size_t>& read_index,
  std::atomic<size_t>& read_limit,
  /*[out]*/ size_t& reserved_index
)
{
  // to ensure thread-safety when there is more than 1 producer thread
  // a second index is defined (m_maximumReadIndex)
  size_t currentPopIndex = read_index.fetch_add(0);
  size_t currentPopLimit = read_limit.fetch_add(0);

  if (_to_index(currentPopIndex) == _to_index(currentPopLimit))
  {
    // the queue region is empty or
    // a producer thread has allocate space in the queue but is
    // waiting to commit the data into it
    //fprintf(stdout, "warning: _pop_from_region %s is empty\n", tag.c_str());
    return false;
  }
  reserved_index = currentPopIndex;
  return true;
}
/// <summary>
/// Internal: increments <paramref name="read_index"/> past <paramref name="reserved_index"/>.
/// </summary>
/// <param name="tag"> Debugging display name of this region of the queue. </param>
/// <param name="read_index"> A reference to the atomic variable this region just read from. </param>
/// <param name="reserved_index"> The index returned by <see cref="_reserve_for_pop"/>. </param>
/// <returns> The return value is <c>true</c> if <paramref name="read_index"/> has been updated,
/// <c>false</c> if the read index changed before we were ready for it to.
/// </returns>
bool PendingMessageQueue::_update_after_pop(
  const std::string& tag,
  std::atomic<size_t>& read_index,
  size_t reserved_index
)
{
  // try to perfrom now the CAS operation on the read index. If we succeed
  // a_data already contains what m_readIndex pointed to before we
  // increased it
  size_t old_read_index = reserved_index;
  if (read_index.compare_exchange_strong(old_read_index, (reserved_index + 1)))
  {
    return true;
  }
  // possibvly some other thread ALSO popped.
  fprintf(stdout, "warning: %s: failed to increment read_index\n", tag.c_str());
  return false;
}

/// <summary>
/// Internal: pushes to a region of the queue, updates indexes accordingly.
/// </summary>
/// <param name="tag"> Debugging display name of this region of the queue. </param>
/// <param name="read_index"> A reference to the atomic variable this region just read from. </param>
/// <param name="write_index"> A reference to the atomic variable this region would write to. </param>
/// <param name="read_limit"> A reference to the atomic variable this region would stop reading from. </param>
/// <param name="pm"> A <c>shared_ptr</c> to the <see cref="PendingMessage"/> to be pushed. </param>
/// <returns> The return value is <c>true</c> if the <paramref name="pm"/> was pushed,
/// and <c>false</c> if the queue is full. </returns>
bool PendingMessageQueue::_push_to_region(
  const std::string& tag,
  std::atomic<size_t>& read_index,
  std::atomic<size_t>& write_index,
  std::atomic<size_t>& read_limit,
  PendingMessagePtr& pm)
{
  size_t currentWriteIndex;

  if (!pm)
  {
    fprintf(stdout, "error: _push_to_region( '%s', ..., pm) arg is empty!\n", tag.c_str());
  }

  if (!_reserve_for_push(tag, read_index, write_index, currentWriteIndex))
  {
    // queue region is full
    return false;
  }

  // We know now that this index is reserved for us. Use it to save the data
  fprintf(stdout, "debug: _push_to_region %s index %d .id = %d\n", tag.c_str(), _to_index(currentWriteIndex), pm ? pm->id : -1);
  std::swap(m_data[_to_index(currentWriteIndex)], pm);

  if (!_update_after_push(tag, read_limit, currentWriteIndex))
  {
    fprintf(stdout, "error: _push_to_region( '%s', ..., pm) failed to update read_limit!\n", tag.c_str());
    _dump(false);
  }

  return true;
}
/// <summary>
/// Internal: pops from a region of the queue, updates indexes accordingly.
/// </summary>
/// <param name="tag"> Debugging display name of this region of the queue. </param>
/// <param name="read_index"> A reference to the atomic variable this region just read from. </param>
/// <param name="read_limit"> A reference to the atomic variable this region would stop reading from. </param>
/// <param name="pm"> A <c>shared_ptr</c> to the <see cref="PendingMessage"/> set to what was in the queue. </param>
/// <returns> The return value is <c>true</c> if the <paramref name="pm"/> was popped,
/// and <c>false</c> if the queue is empty. </returns>
bool PendingMessageQueue::_pop_from_region(
  const std::string& tag,
  std::atomic<size_t>& read_index,
  std::atomic<size_t>& read_limit,
  PendingMessagePtr& pm)
{
  size_t currentPopIndex;
  if (!!pm)
  {
    fprintf(stdout, "error: _pop_from_region( '%s', ..., pm) out arg not empty! releasing: pm.id = %d\n", tag.c_str(), pm ? pm->id : (size_t)-1);
    // so release it!
    PendingMessagePtr nullPtr;
    std::swap(pm, nullPtr);
  }
  do
  {
    if (!_reserve_for_pop(tag, read_index, read_limit, currentPopIndex))
    {
      // the queue region is empty or
      // a producer thread has allocate space in the queue but is
      // waiting to commit the data into it
      //fprintf(stdout, "warning: _pop_from_region %s is empty\n", tag.c_str());
      return false;
    }

    // retrieve the data from the queue
    std::swap(pm, m_data[_to_index(currentPopIndex)]);

    if (!!pm)
    {
      if (_update_after_pop(tag, read_index, currentPopIndex))
      {
        fprintf(stdout, "debug: _pop_from_region %s index %d .id = %d\n", tag.c_str(), _to_index(currentPopIndex), pm ? pm->id : -1);
        return true;
      }
    }
    // it failed retrieving the element off the queue. Someone else must
    // have read the element stored at _to_index(currentPopIndex)
    // before we could perform the CAS operation
  } while (true); // keep looping to try again!

  // Something went wrong. it shouldn't be possible to reach here
  //assert(0);

  // Add this return statement to avoid compiler warnings
  return false;
}

/// <summary>
/// Pushes <paramref name="pm"/> to the queue.
/// </summary>
/// <param name="pm"> A <c>shared_ptr</c> to the <see cref="PendingMessage"/> to be pushed. </param>
/// <returns> The return value is <c>true</c> if the <paramref name="pm"/> was pushed,
/// and <c>false</c> if the queue is full. </returns>
bool PendingMessageQueue::push(PendingMessagePtr& pm)
{
  return _push_to_region("2", m_flow_head, m_push_tail, m_max_flow, pm);
}
/// <summary>
/// Calls <paramref name="func"/> on the head of the <c>needs flow control</c> region of the queue.
/// </summary>
/// <param name="func"> A <see cref="FlowControlTestFunctor"/> used to test, and possibly modify,
/// the <see cref="PendingMessage"/> at the head of the <c>needs flow control</c> region of the queue. </param>
/// <returns> The return value is <c>true</c> if there was a <see cref="PendingMessage"/> to test,
/// and <c>false</c> if the <c>needs flow control</c> region is empty. </returns>
/// <remarks>
/// <c>CAVEAT</c>: The <paramref name="func"/> may modify the internals of the <see cref="PendingMessage"/>
/// if it passes the test.
/// </remarks>
bool PendingMessageQueue::test(FlowControlTestFunctor func)
{
  size_t test_index;
  bool ok;
  if (_reserve_for_pop("2", m_flow_head, m_max_flow, test_index))
  {
    ok = _update_after_pop("2", m_flow_head, test_index);
    fprintf(stdout, "debug: test [2]->1 index %d .id = %d\n", _to_index(test_index), m_data[_to_index(test_index)] ? m_data[_to_index(test_index)]->id : -1);
    if (!!m_data[_to_index(test_index)])
    {
      if (!!func)
      {
        // NOTE: this function may alter the internal of pm.
        if (!func(m_data[_to_index(test_index)]))
        {
          // NOTE: this record failed the flow control rules.
          // TODO: log, drop and notify.
          PendingMessagePtr nullPtr;
          std::swap(m_data[_to_index(test_index)], nullPtr);
        }
      }
      _update_after_push("1", m_max_send, test_index);
      fprintf(stdout, "debug: test 2->[1] index %d .id = %d\n", _to_index(test_index), m_data[_to_index(test_index)] ? m_data[_to_index(test_index)]->id : -1);
      return true;
    }
    else
    {
      fprintf(stdout, "error: nullptr in flow region 2[%d]\n", _to_index(test_index));
      _dump(false);
    }
  }
  return false;
}
/// <summary>
/// Pops <paramref name="pm"/> from the queue.
/// </summary>
/// <param name="pm"> A <c>shared_ptr</c> to the <see cref="PendingMessage"/> that was popped. </param>
/// <returns> The return value is <c>true</c> if the <paramref name="pm"/> was popped,
/// and <c>false</c> if the queue is empty. </returns>
bool PendingMessageQueue::pop(PendingMessagePtr& pm)
{
  while (_pop_from_region("1", m_send_head, m_max_send, pm))
  {
    // skip any nullptr that happen to be in the queue.
    if (!!pm)
    {
      return true;
    }
  }
  return false;
}
size_t PendingMessageQueue::pop_bulk(PendingMessagePtr* items, size_t max_items, size_t max_ms /*= (size_t)~0*/)
{
  PendingMessagePtr* itemNext = items;
  std::size_t result = 0;
  std::size_t need = max_items;
  bool got;
  double maxDelta = max_ms;
  SystemTime started = getSystemTime();
  for (std::size_t i = 0; (need != 0) && (i < max_items) && (getTimeDelta(started) < maxDelta); ++i)
  {
    got = pop(*itemNext);
    if (got)
    {
      result += 1;
      itemNext += 1;
      need -= 1;
    }
    if (need != 0)
    {
      sleep(37);
    }
  }
  return result;
}
