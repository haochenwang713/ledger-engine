// ---------------------------------------------------------------------------
// 併發正確性測試 —— 整個專案最重要的一個檔案。
//
// 前面所有的設計（每帳戶一把 shared_mutex、排序取鎖、臨界區涵蓋
// 檢查到寫入）存在的唯一理由，就是為了通過這裡的測試。
//
// 併發 bug 的本質是「大部分時候不會發生」。所以這裡的策略是：
//   1. 製造大量真實的競爭（多執行緒搶少量帳戶）
//   2. 事後用「數學上必然成立的性質」去驗證，而不是去看某次執行的結果
//
// 那個性質就是不變式。它們不管執行緒怎麼交錯都必須成立，
// 所以一旦違反，就一定是有 race condition —— 沒有其他解釋。
//
// 跑法：
//   make test           一般建置
//   make tsan           ThreadSanitizer —— 會抓到「還沒造成錯誤的」資料競爭
//   make asan           AddressSanitizer —— 會抓到 use-after-free
// ---------------------------------------------------------------------------

#include <ledger/core/AccountRegistry.h>
#include <ledger/core/Journal.h>
#include <ledger/core/LedgerCore.h>

#include <gtest/gtest.h>

#include <atomic>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace ledger {
namespace {

constexpr int kThreadCount = 32;
constexpr Amount kInitialBalance = 1'000'000;

/// 建一組帳戶，全部同幣別、同初始餘額。
std::vector<AccountId> makeAccounts(AccountRegistry& registry,
                                    int count,
                                    Currency ccy,
                                    Amount initial) {
  std::vector<AccountId> ids;
  ids.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    auto created = registry.create(static_cast<OwnerId>(i), ccy, initial);
    EXPECT_TRUE(created.ok());
    ids.push_back(created.value()->id());
  }
  return ids;
}

// ===========================================================================
// I3：每幣別總量守恆
//
// ★ 這是整個專案最重要的一條測試。
//
// 為什麼它抓得到 lost update：
//   每筆轉帳是「A 減 x、B 加 x」，總和變化為零。所以不論多少執行緒
//   用什麼順序交錯執行，跑完之後總額「必須」與初始值一模一樣。
//
//   如果檢查餘額和扣款之間鎖沒抓好，就會出現兩個執行緒都讀到舊值、
//   後寫的覆蓋先寫的 —— 有一筆扣款憑空消失，總額因此變多。
//
//   這條斷言沒有任何模糊空間：差一塊錢都是 bug。
// ===========================================================================
TEST(Concurrency, TotalMoneyIsConserved) {
  AccountRegistry registry;
  Journal journal;
  LedgerCore core(registry, journal);

  // 帳戶數刻意壓低。帳戶越少，兩個執行緒撞上同一組帳戶的機率越高，
  // 競爭越激烈，藏起來的 race 越容易被逼出來。
  constexpr int kAccountCount = 20;
  constexpr int kTransfersPerThread = 3'000;

  const auto ids = makeAccounts(registry, kAccountCount, Currency::USD, kInitialBalance);
  const Amount expectedTotal = kInitialBalance * kAccountCount;
  ASSERT_EQ(core.totalBalance(Currency::USD), expectedTotal);

  std::atomic<int> succeeded{0};
  std::atomic<int> rejected{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([&, t] {
      // 每個執行緒用固定但不同的種子 —— 可重現，又能讓各執行緒走不同路徑。
      std::mt19937 rng(static_cast<std::uint32_t>(t) * 7919U + 13U);
      std::uniform_int_distribution<std::size_t> pick(0, ids.size() - 1);
      std::uniform_int_distribution<Amount> amount(1, 5000);

      for (int i = 0; i < kTransfersPerThread; ++i) {
        const std::size_t a = pick(rng);
        std::size_t b = pick(rng);
        if (a == b) {
          b = (b + 1) % ids.size();  // 自己轉自己會被拒絕，這裡先避開
        }

        TransferRequest req{
            "t" + std::to_string(t) + "-" + std::to_string(i),
            ids[a],
            ids[b],
            amount(rng),
            Currency::USD,
        };

        if (core.transfer(req).ok()) {
          succeeded.fetch_add(1, std::memory_order_relaxed);
        } else {
          rejected.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // --- 判決 ---------------------------------------------------------------

  // I3：總量守恆。差一塊錢都是 race condition。
  EXPECT_EQ(core.totalBalance(Currency::USD), expectedTotal)
      << "總額變了 —— 有 lost update。成功 " << succeeded.load() << " 筆，被拒 " << rejected.load()
      << " 筆。";

  // I1 + I2：交易平衡，且每個帳戶的餘額都能從分錄重算出來。
  EXPECT_TRUE(core.verifyInvariants()) << "餘額快照與分錄對不上 —— lost update。";

  // I4：沒有任何使用者帳戶變成負的。
  for (AccountId id : ids) {
    EXPECT_GE(registry.find(id)->balance(), 0) << "帳戶 " << id << " 出現負餘額";
  }

  // 分錄總和必須為零 —— 每筆交易一負一正互相抵銷。
  EXPECT_EQ(journal.totalEntryAmount(Currency::USD), 0);

  // 健全性檢查：這次執行真的有做到工作，不是全部被拒絕之後空歡喜。
  EXPECT_GT(succeeded.load(), kThreadCount * kTransfersPerThread / 2)
      << "成功的筆數太少，這次測試沒有真的施加壓力";
  EXPECT_EQ(static_cast<int>(core.transferCount()), succeeded.load());
}

// ===========================================================================
// 死鎖：A→B 與 B→A 同時大量發生
//
// 這是排序取鎖要解決的那個場景。天真做法（先鎖來源再鎖目的）在這裡
// 會在幾毫秒內卡死；排序法會跑完。
//
// 注意這個測試「沒有斷言死鎖不會發生」—— 死鎖的表現是測試永遠不結束，
// 由 ctest 的逾時機制判定失敗。這是這類測試的常態。
// ===========================================================================
TEST(Concurrency, OppositeDirectionTransfersDoNotDeadlock) {
  AccountRegistry registry;
  Journal journal;
  LedgerCore core(registry, journal);

  // 只用兩個帳戶 —— 保證每一次轉帳都在爭用同一對鎖。
  // 這是能製造出來的最惡劣競爭情境。
  const auto ids = makeAccounts(registry, 2, Currency::USD, kInitialBalance);
  const AccountId a = ids[0];
  const AccountId b = ids[1];

  constexpr int kRounds = 20'000;
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([&, t] {
      // 半數執行緒做 A→B，另一半做 B→A。
      const bool forward = (t % 2 == 0);
      for (int i = 0; i < kRounds; ++i) {
        TransferRequest req{
            "d" + std::to_string(t) + "-" + std::to_string(i),
            forward ? a : b,
            forward ? b : a,
            1,
            Currency::USD,
        };
        (void)core.transfer(req);
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // 能執行到這一行，就代表沒有死鎖。
  EXPECT_EQ(core.totalBalance(Currency::USD), kInitialBalance * 2);
  EXPECT_TRUE(core.verifyInvariants());
}

// ===========================================================================
// 超額扣款：所有人搶同一個餘額有限的帳戶
//
// 這是 double-spending 的直接測試。一個帳戶只有 1000 元，
// 32 個執行緒同時想各拿 100 元。成功的筆數必須「恰好」是 10 筆。
//
// 如果餘額檢查與扣款之間鎖沒抓好，成功筆數會超過 10，
// 帳戶會變成負的 —— 那就是憑空生出了錢。
// ===========================================================================
TEST(Concurrency, ConcurrentWithdrawalsCannotOverdraw) {
  AccountRegistry registry;
  Journal journal;
  LedgerCore core(registry, journal);

  constexpr Amount kPot = 1000;
  constexpr Amount kSlice = 100;
  constexpr int kExpectedWinners = static_cast<int>(kPot / kSlice);  // 恰好 10

  auto potAccount = registry.create(1, Currency::USD, kPot);
  ASSERT_TRUE(potAccount.ok());
  const AccountId pot = potAccount.value()->id();

  // 每個執行緒有自己的收款帳戶，避免收款端也變成爭用點，
  // 好讓這個測試專注在「來源帳戶會不會被超額扣款」這一件事上。
  std::vector<AccountId> sinks;
  for (int t = 0; t < kThreadCount; ++t) {
    auto sink = registry.create(static_cast<OwnerId>(100 + t), Currency::USD, 0);
    ASSERT_TRUE(sink.ok());
    sinks.push_back(sink.value()->id());
  }

  std::atomic<int> winners{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([&, t] {
      // 每個執行緒試很多次，確保 1000 元一定會被領光，
      // 而且領光之後還會繼續有人來試 —— 那正是最容易出錯的時刻。
      for (int i = 0; i < 50; ++i) {
        TransferRequest req{
            "w" + std::to_string(t) + "-" + std::to_string(i),
            pot,
            sinks[static_cast<std::size_t>(t)],
            kSlice,
            Currency::USD,
        };
        if (core.transfer(req).ok()) {
          winners.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // ★ 「恰好 10 筆」。多一筆就是憑空生錢，少一筆就是錢被吃掉。
  EXPECT_EQ(winners.load(), kExpectedWinners);
  EXPECT_EQ(registry.find(pot)->balance(), 0);
  EXPECT_GE(registry.find(pot)->balance(), 0) << "來源帳戶變成負的 —— 超額扣款";
  EXPECT_TRUE(core.verifyInvariants());
}

// ===========================================================================
// 對帳快照：轉帳持續進行時，audit() 看到的必須是一致的狀態
//
// 這測的是 auditMutex_ 那個「轉帳拿 shared、對帳拿 unique」的反直覺用法。
// 少了它，對帳會逐一鎖帳戶去讀，中途可能讀到 A 的新值配 B 的舊值，
// 於是誤報帳目不平 —— 系統明明是對的。
//
// ⚠ 這個測試也暴露了 std::shared_mutex 的一個真實限制：寫者可能餓死。
//   libstdc++ 的實作底層是 pthread_rwlock，預設是「讀者優先」——
//   只要一直有讀者進來，寫者就永遠排不到。
//
//   這不是紙上談兵，是這個測試實測出來的：
//     工作執行緒用「緊迫迴圈」不停轉帳時 → 對帳執行緒 50 秒內一次都排不進去
//     工作執行緒每筆之間 yield() 一次時   → 對帳完成約 6000 次
//
//   差別只有一行 yield。這說明 auditMutex_ 的寫者對「讀者密度」極度敏感，
//   Stage 8 壓測時要把這件事量化，並在 README 誠實寫出來。
// ===========================================================================
TEST(Concurrency, AuditSeesConsistentSnapshotDuringTraffic) {
  AccountRegistry registry;
  Journal journal;
  LedgerCore core(registry, journal);

  constexpr int kAccountCount = 8;
  constexpr int kTransfersPerThread = 2'000;
  const auto ids = makeAccounts(registry, kAccountCount, Currency::USD, kInitialBalance);
  const Amount expectedTotal = kInitialBalance * kAccountCount;

  std::atomic<int> workersRunning{kThreadCount};
  std::atomic<int> auditsDuringTraffic{0};
  std::atomic<int> inconsistentAudits{0};

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreadCount; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937 rng(static_cast<std::uint32_t>(t) + 1U);
      std::uniform_int_distribution<std::size_t> pick(0, ids.size() - 1);

      for (int i = 0; i < kTransfersPerThread; ++i) {
        const std::size_t a = pick(rng);
        const std::size_t b = (a + 1) % ids.size();
        TransferRequest req{
            "a" + std::to_string(t) + "-" + std::to_string(i),
            ids[a],
            ids[b],
            10,
            Currency::USD,
        };
        (void)core.transfer(req);

        // 讓出時間片。真實流量本來就有間隙，而且這給對帳執行緒
        // 一個擠進去的機會 —— 沒有這行，讀者優先的實作下它幾乎排不到。
        std::this_thread::yield();
      }
      workersRunning.fetch_sub(1, std::memory_order_relaxed);
    });
  }

  // 轉帳進行中不斷嘗試對帳
  std::thread auditor([&] {
    while (workersRunning.load(std::memory_order_relaxed) > 0) {
      const AuditSnapshot snapshot = core.audit();
      auditsDuringTraffic.fetch_add(1, std::memory_order_relaxed);
      if (snapshot.totalPerCurrency[static_cast<std::size_t>(Currency::USD)] != expectedTotal) {
        inconsistentAudits.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  auditor.join();
  for (auto& worker : workers) {
    worker.join();
  }

  // ★ 核心斷言：每一次成功的對帳都必須看到正確的總額。
  //   一次都不能錯 —— 對帳讀到不一致的快照，就是 auditMutex_ 沒發揮作用。
  EXPECT_EQ(inconsistentAudits.load(), 0) << auditsDuringTraffic.load() << " 次對帳中有 "
                                          << inconsistentAudits.load() << " 次讀到不一致的快照";

  // 對帳擠進去的次數本身是一個觀測值，不是正確性條件。
  // 讀者優先的實作下這個數字可能很小 —— 那正是寫者飢餓的證據。
  std::cout << "[          ] 轉帳進行中完成了 " << auditsDuringTraffic.load()
            << " 次對帳（數字偏低代表 shared_mutex 寫者飢餓）\n";

  EXPECT_EQ(core.totalBalance(Currency::USD), expectedTotal);
  EXPECT_TRUE(core.verifyInvariants());
}

// ===========================================================================
// 開戶與轉帳同時進行
//
// AccountRegistry 的 map 在插入時可能 rehash。這個測試確認
// 「rehash 進行中，另一個執行緒手上的 Account* 仍然有效」。
// 這條路徑上的錯誤是 use-after-free —— ASan 會抓到。
// ===========================================================================
TEST(Concurrency, RegistryGrowthDoesNotInvalidatePointers) {
  AccountRegistry registry;
  Journal journal;
  LedgerCore core(registry, journal);

  const auto ids = makeAccounts(registry, 4, Currency::USD, kInitialBalance);
  std::atomic<bool> stop{false};

  // 一邊不停開新帳戶，逼 unordered_map 反覆 rehash
  std::thread creator([&] {
    for (OwnerId i = 0; i < 20'000; ++i) {
      (void)registry.create(1'000'000 + i, Currency::EUR);
    }
    stop.store(true, std::memory_order_relaxed);
  });

  // 一邊不停用舊帳戶轉帳
  std::vector<std::thread> workers;
  for (int t = 0; t < 8; ++t) {
    workers.emplace_back([&, t] {
      int i = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        TransferRequest req{
            "g" + std::to_string(t) + "-" + std::to_string(i++),
            ids[0],
            ids[1],
            1,
            Currency::USD,
        };
        (void)core.transfer(req);
      }
    });
  }

  creator.join();
  for (auto& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(core.totalBalance(Currency::USD), kInitialBalance * 4);
  EXPECT_TRUE(core.verifyInvariants());
}

}  // namespace
}  // namespace ledger
