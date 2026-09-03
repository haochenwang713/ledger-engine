// ---------------------------------------------------------------------------
// Stage 5b —— 佇列與執行緒池的測試。
//
// 這一層的測試比程式碼本身更重要，理由是併發 bug 不會自己報錯：
//   - 忘記 notify_all → 關機時 join() 永遠回不來（表現為 hang，不是錯誤）
//   - 佇列滿時阻塞     → event loop 停擺（表現為 TPS 掉到零，不是錯誤）
//   - weak_ptr 用成裸指標 → use-after-free（表現為偶發崩潰，不是錯誤）
//
// 所以下面每一條「不該卡住」的測試都帶明確的時間上界。逾時就是失敗，
// 而不是讓 ctest 掛在那裡等三十分鐘。
//
// Test structure: Arrange-Act-Assert. In the timing tests the Act phase is
// wrapped in millisFor(), so "how long the action took" is itself one of the
// assertions. See instruction.md for the convention.
// ---------------------------------------------------------------------------

#include <ledger/concurrent/BlockingQueue.h>
#include <ledger/concurrent/Task.h>
#include <ledger/concurrent/ThreadPool.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace ledger;
using namespace ledger::concurrent;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

/// 一段程式碼花了多少毫秒。
template <typename Fn>
std::int64_t millisFor(Fn&& fn) {
  const auto start = Clock::now();
  fn();
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// ---------------------------------------------------------------------------
// 測試用的 ResponseSink：記下收到的每個 reqId。
//
// 用假的 sink 而不是真的 Connection，是 Task 持有抽象介面的直接好處 ——
// 這整個檔案不需要 socket，也不需要 Linux。
// ---------------------------------------------------------------------------
class RecordingSink : public ResponseSink {
 public:
  void deliver(const proto::ResponseEnvelope& resp, proto::CodecTag codec) override {
    std::lock_guard lock(mutex_);
    ids_.push_back(resp.reqId);
    lastCodec_ = codec;
  }

  [[nodiscard]] std::size_t count() const {
    std::lock_guard lock(mutex_);
    return ids_.size();
  }

  [[nodiscard]] std::vector<std::uint32_t> ids() const {
    std::lock_guard lock(mutex_);
    return ids_;
  }

  [[nodiscard]] proto::CodecTag lastCodec() const {
    std::lock_guard lock(mutex_);
    return lastCodec_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::uint32_t> ids_;
  proto::CodecTag lastCodec_{proto::CodecTag::Binary};
};

/// 最簡單的 handler：原樣回一個 Pong，並累加呼叫次數。
class PongHandler : public RequestHandler {
 public:
  PongHandler(std::atomic<int>* calls, std::size_t index) : calls_(calls), index_(index) {}

  proto::ResponseEnvelope handle(const proto::RequestEnvelope& req) override {
    if (calls_ != nullptr) {
      calls_->fetch_add(1, std::memory_order_relaxed);
    }
    return proto::ResponseEnvelope{req.reqId, proto::PongResp{}};
  }

  [[nodiscard]] std::size_t index() const noexcept { return index_; }

 private:
  std::atomic<int>* calls_;
  std::size_t index_;
};

/// 每筆都睡一下的 handler，用來製造「佇列來得及塞滿」的狀況。
class SlowHandler : public RequestHandler {
 public:
  explicit SlowHandler(std::chrono::milliseconds delay) : delay_(delay) {}

  proto::ResponseEnvelope handle(const proto::RequestEnvelope& req) override {
    std::this_thread::sleep_for(delay_);
    return proto::ResponseEnvelope{req.reqId, proto::PongResp{}};
  }

 private:
  std::chrono::milliseconds delay_;
};

HandlerFactory pongFactory(std::atomic<int>* calls) {
  return [calls](std::size_t index) -> std::unique_ptr<RequestHandler> {
    return std::make_unique<PongHandler>(calls, index);
  };
}

Task makeTask(const ResponseSinkPtr& sink, std::uint32_t reqId) {
  return Task{sink, proto::RequestEnvelope{reqId, proto::PingReq{}}, proto::CodecTag::Binary};
}

}  // namespace

// ===========================================================================
// BlockingQueue —— 基本行為
// ===========================================================================

TEST(BlockingQueue, PushPopPreservesOrder) {
  // Arrange
  BlockingQueue<int> q{16};

  // Act
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(q.tryPush(i));
  }

  // Assert
  EXPECT_EQ(q.size(), 5U);
  for (int i = 0; i < 5; ++i) {
    const auto v = q.tryPop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, i);
  }
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.totalPushed(), 5U);
  EXPECT_EQ(q.totalPopped(), 5U);
}

TEST(BlockingQueue, CapacityIsEnforced) {
  // Arrange
  BlockingQueue<int> q{3};

  // Act & Assert —— 第四筆必須被拒絕。
  EXPECT_TRUE(q.tryPush(1));
  EXPECT_TRUE(q.tryPush(2));
  EXPECT_TRUE(q.tryPush(3));
  EXPECT_FALSE(q.tryPush(4));  // 滿了
  EXPECT_EQ(q.size(), 3U);
  EXPECT_EQ(q.totalRejected(), 1U);
}

// ★ 這是整個檔案最重要的一條測試之一。
//
//   IO 執行緒在佇列滿時若阻塞，整個 event loop 停擺 —— 所有連線一起死。
//   所以 tryPush 必須「立刻」回 false，不是「很快」回 false。
TEST(BlockingQueue, TryPushNeverBlocksWhenFull) {
  // Arrange —— 容量 1，先塞滿。
  BlockingQueue<int> q{1};
  EXPECT_TRUE(q.tryPush(1));

  // Act
  const auto elapsed = millisFor([&] {
    for (int i = 0; i < 10000; ++i) {
      EXPECT_FALSE(q.tryPush(i));
    }
  });

  // Assert —— 一萬次被拒絕的推入應該是微秒等級的事。給 500ms 的寬鬆上界，
  // 任何形式的阻塞都會遠遠超過它。
  EXPECT_TRUE(elapsed < 500) << "tryPush 疑似阻塞了，耗時 " << elapsed << " ms";
  EXPECT_EQ(q.totalRejected(), 10000U);
}

TEST(BlockingQueue, TryPopOnEmptyReturnsNullopt) {
  // Arrange
  BlockingQueue<int> q{4};

  // Act & Assert
  EXPECT_FALSE(q.tryPop().has_value());
}

// ===========================================================================
// BlockingQueue —— 阻塞與喚醒
// ===========================================================================

TEST(BlockingQueue, PopBlocksUntilItemArrives) {
  // Arrange
  BlockingQueue<int> q{4};
  std::latch consumerReady{1};
  std::optional<int> got;

  std::jthread consumer([&] {
    consumerReady.count_down();
    got = q.pop();
  });

  consumerReady.wait();
  std::this_thread::sleep_for(50ms);  // 確保 consumer 真的睡在 pop 上

  // Act
  EXPECT_TRUE(q.tryPush(99));
  consumer.join();

  // Assert
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, 99);
}

TEST(BlockingQueue, PushBlocksUntilSpaceAppears) {
  // Arrange —— 容量 1，先塞滿，再讓另一條執行緒卡在 push 上。
  BlockingQueue<int> q{1};
  EXPECT_TRUE(q.tryPush(1));

  std::latch producerReady{1};
  bool pushed = false;

  std::jthread producer([&] {
    producerReady.count_down();
    pushed = q.push(2);  // 佇列滿，會阻塞
  });

  producerReady.wait();
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(q.size(), 1U);  // 還沒進去

  // Act —— 騰出空位
  EXPECT_TRUE(q.tryPop().has_value());
  producer.join();

  // Assert
  EXPECT_TRUE(pushed);
}

// ★ 關機能否完成的關鍵測試。
//
//   close() 若用 notify_one 而不是 notify_all，只有一條執行緒會醒來，
//   其餘的永遠睡著 —— join() 就永遠回不來。這種 bug 表現為「程式關不掉」，
//   不會有任何錯誤訊息，而且只在關機路徑出現。
TEST(BlockingQueue, CloseWakesEveryBlockedConsumer) {
  // Arrange —— 八條執行緒全部睡在 pop() 上。
  constexpr int kConsumers = 8;
  BlockingQueue<int> q{16};

  std::latch allBlocked{kConsumers};
  std::atomic<int> woke{0};
  std::vector<std::jthread> consumers;

  for (int i = 0; i < kConsumers; ++i) {
    consumers.emplace_back([&] {
      allBlocked.count_down();
      const auto v = q.pop();
      if (!v) {
        woke.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  allBlocked.wait();
  std::this_thread::sleep_for(50ms);  // 讓它們確實睡進 pop

  // Act
  const auto elapsed = millisFor([&] {
    q.close();
    for (auto& c : consumers) {
      c.join();
    }
  });

  // Assert —— 八條全部醒來，而且很快。
  EXPECT_EQ(woke.load(), kConsumers);
  EXPECT_TRUE(elapsed < 2000) << "close() 之後 join 花了 " << elapsed << " ms，疑似漏了 notify_all";
}

TEST(BlockingQueue, CloseWakesBlockedProducer) {
  // Arrange
  BlockingQueue<int> q{1};
  EXPECT_TRUE(q.tryPush(1));

  std::latch ready{1};
  bool result = true;

  std::jthread producer([&] {
    ready.count_down();
    result = q.push(2);  // 阻塞
  });

  ready.wait();
  std::this_thread::sleep_for(50ms);

  // Act
  const auto elapsed = millisFor([&] {
    q.close();
    producer.join();
  });

  // Assert
  EXPECT_FALSE(result) << "佇列關閉後 push() 應該回 false";
  EXPECT_TRUE(elapsed < 2000) << "close() 沒有喚醒阻塞中的 push()";
}

// ★ drain 語意：關閉之後，已經在佇列裡的東西仍然拿得出來。
//
//   那些是「已收下、client 正在等回應」的請求。直接丟掉的話，
//   那些 client 只能等自己 timeout。
TEST(BlockingQueue, CloseStillDrainsRemainingItems) {
  // Arrange
  BlockingQueue<int> q{16};
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(q.tryPush(i));
  }

  // Act
  q.close();

  // Assert
  EXPECT_TRUE(q.closed());
  EXPECT_FALSE(q.tryPush(99));  // 不再收新的

  for (int i = 0; i < 5; ++i) {
    const auto v = q.pop();
    ASSERT_TRUE(v.has_value()) << "第 " << i << " 個元素應該還在";
    EXPECT_EQ(*v, i);
  }
  EXPECT_FALSE(q.pop().has_value());  // 排空之後才回 nullopt
}

// ★ stop_token 與 close() 的差別：前者不排空。
TEST(BlockingQueue, StopTokenAbandonsRemainingItems) {
  // Arrange
  BlockingQueue<int> q{16};
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(q.tryPush(i));
  }
  std::stop_source source;

  // Act
  source.request_stop();

  // Assert —— 即使佇列裡還有五個元素，已被請求停止就不再拿新工作。
  EXPECT_FALSE(q.pop(source.get_token()).has_value());
  EXPECT_EQ(q.size(), 5U) << "元素應該還留在佇列裡，只是不再被取用";
}

TEST(BlockingQueue, StopTokenWakesBlockedConsumer) {
  // Arrange
  BlockingQueue<int> q{4};
  std::stop_source source;
  std::latch ready{1};
  bool gotNullopt = false;

  std::jthread consumer([&] {
    ready.count_down();
    gotNullopt = !q.pop(source.get_token()).has_value();
  });

  ready.wait();
  std::this_thread::sleep_for(50ms);

  // Act
  const auto elapsed = millisFor([&] {
    source.request_stop();
    consumer.join();
  });

  // Assert
  EXPECT_TRUE(gotNullopt);
  EXPECT_TRUE(elapsed < 2000) << "request_stop() 沒有喚醒阻塞中的 pop()";
}

// ★ TSan 的主戰場：多生產者、多消費者、每個元素恰好被拿走一次。
TEST(BlockingQueue, MpmcDeliversEveryItemExactlyOnce) {
  // Arrange
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 5000;
  constexpr int kTotal = kProducers * kPerProducer;

  BlockingQueue<int> q{256};  // 刻意小於總量，強迫生產者反覆等待
  std::vector<std::vector<int>> received(kConsumers);
  std::atomic<int> consumed{0};

  // Act —— 四個生產者、四個消費者，全部跑完再收工。
  std::vector<std::jthread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&, c] {
      for (;;) {
        const auto v = q.pop();
        if (!v) {
          break;
        }
        received[static_cast<std::size_t>(c)].push_back(*v);
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::vector<std::jthread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) {
        // 用阻塞版 push，確保沒有任何一筆被丟掉。
        EXPECT_TRUE(q.push(p * kPerProducer + i));
      }
    });
  }

  for (auto& p : producers) {
    p.join();
  }
  q.close();
  for (auto& c : consumers) {
    c.join();
  }

  // Assert
  EXPECT_EQ(consumed.load(), kTotal);
  EXPECT_EQ(q.totalPushed(), static_cast<std::uint64_t>(kTotal));
  EXPECT_EQ(q.totalPopped(), static_cast<std::uint64_t>(kTotal));

  // 每個值恰好出現一次 —— 不重複、不遺漏。
  std::unordered_set<int> seen;
  for (const auto& perConsumer : received) {
    for (const int v : perConsumer) {
      EXPECT_TRUE(seen.insert(v).second) << "值 " << v << " 被取走了兩次";
    }
  }
  EXPECT_EQ(seen.size(), static_cast<std::size_t>(kTotal));
}

// ===========================================================================
// ThreadPool
// ===========================================================================

TEST(ThreadPool, ProcessesEverySubmittedTask) {
  // Arrange
  constexpr int kTasks = 500;
  std::atomic<int> calls{0};
  auto sink = std::make_shared<RecordingSink>();

  // Act
  {
    ThreadPool pool{"test", 4, 1024, pongFactory(&calls)};
    for (int i = 0; i < kTasks; ++i) {
      EXPECT_TRUE(pool.submit(makeTask(sink, static_cast<std::uint32_t>(i))));
    }
    pool.shutdown();  // 排空
  }

  // Assert
  EXPECT_EQ(calls.load(), kTasks);
  EXPECT_EQ(sink->count(), static_cast<std::size_t>(kTasks));

  // reqId 必須原樣回來 —— 20 條 worker 平行處理，順序會亂，
  // 但每個 id 都要出現恰好一次。這正是 reqId 存在的理由。
  std::unordered_set<std::uint32_t> seen;
  for (const auto id : sink->ids()) {
    EXPECT_TRUE(seen.insert(id).second) << "reqId " << id << " 重複了";
  }
  EXPECT_EQ(seen.size(), static_cast<std::size_t>(kTasks));
}

TEST(ThreadPool, EachWorkerGetsItsOwnHandler) {
  // ★ Stage 6 的前提：每個 worker 一條 DB 連線。
  //   工廠必須恰好被呼叫 workerCount 次，而且 index 各不相同。

  // Arrange
  constexpr std::size_t kWorkers = 6;
  std::mutex mutex;
  std::set<std::size_t> indices;
  std::atomic<int> factoryCalls{0};

  // Act —— 光是建立 pool 就會呼叫工廠。
  {
    ThreadPool pool{
        "test", kWorkers, 64, [&](std::size_t index) -> std::unique_ptr<RequestHandler> {
          factoryCalls.fetch_add(1, std::memory_order_relaxed);
          {
            std::lock_guard lock(mutex);
            indices.insert(index);
          }
          return std::make_unique<PongHandler>(nullptr, index);
        }};
    pool.shutdown();
  }

  // Assert
  EXPECT_EQ(factoryCalls.load(), static_cast<int>(kWorkers));
  EXPECT_EQ(indices.size(), kWorkers);
  EXPECT_TRUE(indices.count(0) == 1);
  EXPECT_TRUE(indices.count(kWorkers - 1) == 1);
}

TEST(ThreadPool, SubmitRejectsWhenQueueIsFull) {
  // Arrange —— 一條 worker、每筆睡 50ms、佇列只有 4 格，一定塞得爆。
  auto sink = std::make_shared<RecordingSink>();
  ThreadPool pool{"test", 1, 4, [](std::size_t) -> std::unique_ptr<RequestHandler> {
                    return std::make_unique<SlowHandler>(50ms);
                  }};
  int accepted = 0;
  int refused = 0;

  // Act
  const auto elapsed = millisFor([&] {
    for (int i = 0; i < 200; ++i) {
      if (pool.submit(makeTask(sink, static_cast<std::uint32_t>(i)))) {
        ++accepted;
      } else {
        ++refused;
      }
    }
  });

  // Assert —— 重點不是拒絕了幾筆，而是「提交 200 次沒有卡住」。
  // 200 筆若全被接受並處理完要 10 秒；submit 若會阻塞就會接近那個數字。
  EXPECT_TRUE(refused > 0) << "佇列應該要滿才對";
  EXPECT_TRUE(elapsed < 1000) << "submit 疑似阻塞了，耗時 " << elapsed << " ms";
  EXPECT_EQ(pool.rejected(), static_cast<std::uint64_t>(refused));

  pool.abort();  // 不等剩下的做完
}

TEST(ThreadPool, ShutdownDrainsPendingTasks) {
  // Arrange
  constexpr int kTasks = 40;
  std::atomic<int> calls{0};
  auto sink = std::make_shared<RecordingSink>();
  ThreadPool pool{"test", 2, 256, pongFactory(&calls)};
  for (int i = 0; i < kTasks; ++i) {
    EXPECT_TRUE(pool.submit(makeTask(sink, static_cast<std::uint32_t>(i))));
  }

  // Act
  pool.shutdown();

  // Assert —— shutdown() 回來時，佇列裡的每一筆都必須已經處理完 ——
  // 它們是「client 還在等回應」的工作。
  EXPECT_EQ(calls.load(), kTasks);
  EXPECT_EQ(pool.completed(), static_cast<std::uint64_t>(kTasks));
  EXPECT_EQ(pool.queueSize(), 0U);
}

TEST(ThreadPool, AbortDoesNotWaitForPendingTasks) {
  // Arrange —— 一條 worker、每筆 100ms、丟 50 筆進去，全做完要 5 秒。
  auto sink = std::make_shared<RecordingSink>();
  ThreadPool pool{"test", 1, 256, [](std::size_t) -> std::unique_ptr<RequestHandler> {
                    return std::make_unique<SlowHandler>(100ms);
                  }};
  for (int i = 0; i < 50; ++i) {
    EXPECT_TRUE(pool.submit(makeTask(sink, static_cast<std::uint32_t>(i))));
  }
  std::this_thread::sleep_for(50ms);

  // Act
  const auto elapsed = millisFor([&] { pool.abort(); });

  // Assert —— abort() 必須遠快於 5 秒。
  EXPECT_TRUE(elapsed < 2000) << "abort() 花了 " << elapsed << " ms，它不該等佇列排空";
  EXPECT_TRUE(pool.completed() < 50U) << "abort() 不應該把全部做完";
}

// ★ W2 不變式：worker 做完時連線可能已經不在。
TEST(ThreadPool, DiscardsResultWhenSinkIsGone) {
  // Arrange
  ThreadPool pool{"test", 1, 64, [](std::size_t) -> std::unique_ptr<RequestHandler> {
                    return std::make_unique<SlowHandler>(150ms);
                  }};
  auto liveSink = std::make_shared<RecordingSink>();
  auto doomedSink = std::make_shared<RecordingSink>();

  // 第一筆會占住那條唯一的 worker 150ms。
  EXPECT_TRUE(pool.submit(makeTask(liveSink, 1)));
  // 第二筆在佇列裡等。
  EXPECT_TRUE(pool.submit(makeTask(doomedSink, 2)));
  std::this_thread::sleep_for(20ms);

  // Act —— 趁第二筆還沒被處理，把它的 sink 銷毀（模擬 client 斷線）。
  doomedSink.reset();
  pool.shutdown();

  // Assert
  EXPECT_EQ(pool.droppedNoSink(), 1U) << "sink 已失效的工作應該被丟棄";
  EXPECT_EQ(liveSink->count(), 1U);
}

TEST(ThreadPool, ConcurrentSubmitFromManyThreads) {
  // IO 執行緒實際上只有一條，但這條測試確保 submit 本身是執行緒安全的，
  // 也讓 TSan 有機會在 submit 路徑上找 race。

  // Arrange
  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;
  std::atomic<int> calls{0};
  auto sink = std::make_shared<RecordingSink>();
  ThreadPool pool{"test", 4, 8192, pongFactory(&calls)};

  // Act
  {
    std::vector<std::jthread> submitters;
    for (int t = 0; t < kThreads; ++t) {
      submitters.emplace_back([&, t] {
        for (int i = 0; i < kPerThread; ++i) {
          const auto id = static_cast<std::uint32_t>(t * kPerThread + i);
          while (!pool.submit(makeTask(sink, id))) {
            std::this_thread::yield();  // 佇列滿就讓一下，不丟棄
          }
        }
      });
    }
  }  // jthread 解構會 join

  pool.shutdown();

  // Assert
  EXPECT_EQ(calls.load(), kThreads * kPerThread);
  EXPECT_EQ(pool.completed(), static_cast<std::uint64_t>(kThreads * kPerThread));
}

TEST(ThreadPool, MeasuresQueueWaitTime) {
  // 排隊時間與處理時間是兩回事。過載時前者才是延遲主因，
  // 分開量才不會把「排隊很久」誤診成「處理很慢」。

  // Arrange
  auto sink = std::make_shared<RecordingSink>();
  ThreadPool pool{"test", 1, 64, [](std::size_t) -> std::unique_ptr<RequestHandler> {
                    return std::make_unique<SlowHandler>(30ms);
                  }};

  // Act
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(pool.submit(makeTask(sink, static_cast<std::uint32_t>(i))));
  }
  pool.shutdown();

  // Assert
  EXPECT_EQ(pool.completed(), 5U);
  // 第 5 筆得等前面四筆各 30ms，所以累計排隊時間必然大於零。
  EXPECT_TRUE(pool.totalQueueWaitMicros() > 0U);
}

TEST(ThreadPool, DestructorShutsDownCleanly) {
  // 沒有明確呼叫 shutdown()，解構子也必須把事情收乾淨且不 hang。

  // Arrange
  std::atomic<int> calls{0};
  auto sink = std::make_shared<RecordingSink>();

  // Act
  const auto elapsed = millisFor([&] {
    ThreadPool pool{"test", 4, 256, pongFactory(&calls)};
    for (int i = 0; i < 100; ++i) {
      EXPECT_TRUE(pool.submit(makeTask(sink, static_cast<std::uint32_t>(i))));
    }
  });  // ← 解構子在這裡執行

  // Assert
  EXPECT_TRUE(elapsed < 3000) << "解構子花了 " << elapsed << " ms，疑似 hang 住";
  EXPECT_EQ(calls.load(), 100) << "解構子應該走優雅關機（排空）";
}

TEST(ThreadPool, ShutdownIsIdempotent) {
  // Arrange
  std::atomic<int> calls{0};
  ThreadPool pool{"test", 2, 64, pongFactory(&calls)};

  // Act
  pool.shutdown();
  pool.shutdown();  // 第二次不該爆炸也不該卡住
  pool.abort();     // 已經關了，也不該爆炸

  // Assert
  EXPECT_EQ(pool.completed(), 0U);
}

TEST(ThreadPool, CodecTagSurvivesRoundTrip) {
  // Task 帶著 CodecTag 跑完整趟，回程時 sink 才知道要用哪種編碼。
  // 少了它，worker 就得知道自己在服務哪個 port —— 那是不該有的耦合。

  // Arrange
  auto sink = std::make_shared<RecordingSink>();
  std::atomic<int> calls{0};

  // Act —— 用 Json tag 送一筆進去。
  {
    ThreadPool pool{"test", 1, 16, pongFactory(&calls)};
    Task task{sink, proto::RequestEnvelope{7, proto::PingReq{}}, proto::CodecTag::Json};
    EXPECT_TRUE(pool.submit(std::move(task)));
    pool.shutdown();
  }

  // Assert
  EXPECT_EQ(sink->count(), 1U);
  EXPECT_EQ(sink->lastCodec(), proto::CodecTag::Json);
}
