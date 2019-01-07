#pragma warning(push)
#pragma warning(disable : 4127)
#include "../../concurrentqueue.h"
#pragma warning(pop)

#include <vector>
#include <atomic>		// Requires C++11. Sorry VS2010.

typedef struct {
  std::string id;
  // ...
} Host;
typedef struct {
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  std::string prerelease;
  uint32_t build;
} VersionDetails;
typedef struct {
  std::string id;
  std::string name;
  std::string version;
  std::shared_ptr<VersionDetails> versionDetails;
} Sdk;
typedef struct {
  std::string id;
  std::string name;
  std::string version;
  std::shared_ptr<VersionDetails> versionDetails;
  std::shared_ptr<Sdk> sdk;
} Program;
typedef struct {
  std::string sessionId;
  std::shared_ptr<Host> host;
  std::shared_ptr<Program> program;
} Context;
typedef struct {
  std::size_t id;
  std::string packageName;
  std::string messageName;
  std::vector<unsigned char> messageBytes;
  std::shared_ptr<Context> customContext;
} PendingMessage;
typedef PendingMessage* PendingMessagePtr;

/// <summary>
/// This  queue class is derived from <c>moodycamel::ConcurrentQueue</c> to manage
/// items without the use of <c>Mutex</c> or other locks, but int he style of a <c>std::queue</c>.
/// <para> It is a multi-producer, multi-consumer queue. </para>
/// </summary>
/// <remarks>
/// See: moodycamel::ConcurrentQueue: https://github.com/cameron314/concurrentqueue
/// </remarks>
class PendingMessageQueue : public moodycamel::ConcurrentQueue<PendingMessagePtr>
{
public:
    PendingMessageQueue()
    {
      std::atomic_init(&m_totalCount, 0U);
      std::atomic_init(&m_highWaterMark, 0U);
      std::atomic_init(&m_recentTop, PendingMessagePtr());
    }
    ~PendingMessageQueue()
    {
        while (!empty())
        {
            pop();
        }
    }

private:
    std::atomic<int32_t> m_totalCount;
    std::atomic<int32_t> m_highWaterMark;
    std::atomic<PendingMessagePtr> m_recentTop;

public:
    // standard queue methods NOT part of concurrentqueue
    //
    /// <summary>
    /// Predicate to see if the queue is currently empty.
    /// <para><c>CAVEAT</c>: This is a momentary fact, and the answer may be different
    /// at <c>any</c> moment.</para>
    /// </sumnmary>
    /// <returns>
    /// The return is <c>true</c> if the queue is currently empty, else <c>false</c>.
    /// </returns>
    bool empty() const { return size() == 0; }
    /// <summary>
    /// Gets the current number of items that are in this queue.
    /// </summary>
    size_t size() const { return std::atomic_load(&m_totalCount); }
    /// <summary>
    /// Gets the maximum number of items that have been in this queue.
    /// </summary>
    size_t highWaterMark() const { return std::atomic_load(&m_highWaterMark); }
    /// <summary>
    /// Pushes the <paramref name="pm"/> onto the queue.
    /// </summary>
    /// <param name="pm"> The <c>PendingMessagePtr</c> to be pushed onto the queue.
    void push(PendingMessagePtr pm)
    {
      if (enqueue(pm))
      {
        int32_t oldTotal = std::atomic_fetch_add(&m_totalCount, 1);
        int32_t oldHigh = std::atomic_load(&m_highWaterMark);
        if ((oldTotal + 1) > oldHigh)
        {
          std::atomic_store(&m_highWaterMark, oldTotal + 1);
        }
      }
      else
      {
        // TODO: report error!
      }
    }
    /// <summary>
    /// Retrieves the current first item in the queue.
    /// If the queue has not yet been popped, then it will return the same item again.
    /// If the queue is empty, then a default constructed instance is returned.
    /// Check its validity before using it.
    /// </summary>
    /// <returns>
    /// The <c>PendingMessagePtr</c> that refers to the first item in the queue.
    /// <para><c>CAVEAT</c>: It may be NULL, check validity before use.</para>
    /// </returns>
    PendingMessagePtr top()
    {
        PendingMessagePtr item;
        item = std::atomic_load(&m_recentTop);
        if (item == nullptr)
        {
            if (try_dequeue(item))
            {
              std::atomic_store(&m_recentTop, item);
            }
        }
        return item;
    }
    /// <summary>
    /// Officially removes the <see cref="top"/> item in the queue.
    /// And decrements the total current count of items in the queue.
    /// </summary>
    void pop()
    {
        PendingMessagePtr null_item = nullptr;
        std::atomic_store(&m_recentTop, null_item);
#pragma warning(push)
#pragma warning(disable : 4189) // local variable is initialized but not referenced
        int32_t oldTotal = std::atomic_fetch_add(&m_totalCount, -1);
        assert(oldTotal > 0);
#pragma warning(pop)
    }
};
