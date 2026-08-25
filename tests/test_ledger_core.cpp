// ---------------------------------------------------------------------------
// LedgerCore 的單執行緒行為測試。
//
// 這裡測的是「規則對不對」：拒絕該拒絕的、接受該接受的、數字算得對。
// 併發正確性是另一個檔案（test_concurrency.cpp）的事。
// ---------------------------------------------------------------------------

#include <ledger/core/AccountRegistry.h>
#include <ledger/core/Journal.h>
#include <ledger/core/LedgerCore.h>

#include <gtest/gtest.h>

namespace ledger {
namespace {

// 沿用設計文件與 db/seeds/dev_seed.sql 裡的例子，方便對照：
//   Alice(1001) USD $1,200.00  轉 $50.00 給  Bob(2002) USD $420.00
constexpr AccountId kAlice = 1001;
constexpr AccountId kBob = 2002;
constexpr AccountId kCarolJpy = 3003;

class LedgerCoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    core_ = std::make_unique<LedgerCore>(registry_, journal_);

    ASSERT_TRUE(registry_.createWithId(kAlice, 101, Currency::USD, 120000).ok());
    ASSERT_TRUE(registry_.createWithId(kBob, 102, Currency::USD, 42000).ok());
    ASSERT_TRUE(registry_.createWithId(kCarolJpy, 103, Currency::JPY, 500000).ok());
  }

  TransferRequest usdTransfer(AccountId from, AccountId to, Amount amount, std::string key = "k") {
    return TransferRequest{std::move(key), from, to, amount, Currency::USD};
  }

  Amount balanceOf(AccountId id) { return registry_.find(id)->balance(); }

  AccountRegistry registry_;
  Journal journal_;
  std::unique_ptr<LedgerCore> core_;
};

// === 正常路徑 ==============================================================

TEST_F(LedgerCoreTest, TransfersMoneyAndReportsNewBalances) {
  const auto result = core_->transfer(usdTransfer(kAlice, kBob, 5000, "req-a3f9-01"));

  ASSERT_TRUE(result.ok()) << toString(result.error());
  EXPECT_EQ(result.value().fromBalance, 115000);  // 1,200.00 - 50.00
  EXPECT_EQ(result.value().toBalance, 47000);     //   420.00 + 50.00

  EXPECT_EQ(balanceOf(kAlice), 115000);
  EXPECT_EQ(balanceOf(kBob), 47000);
}

TEST_F(LedgerCoreTest, WritesExactlyTwoBalancedEntries) {
  ASSERT_TRUE(core_->transfer(usdTransfer(kAlice, kBob, 5000)).ok());

  EXPECT_EQ(journal_.transactionCount(), 1u);
  EXPECT_EQ(journal_.entryCount(), 2u);

  const auto aliceEntries = journal_.entriesFor(kAlice);
  const auto bobEntries = journal_.entriesFor(kBob);
  ASSERT_EQ(aliceEntries.size(), 1u);
  ASSERT_EQ(bobEntries.size(), 1u);

  EXPECT_EQ(aliceEntries[0].amount, -5000);                     // 借方
  EXPECT_EQ(bobEntries[0].amount, 5000);                        // 貸方
  EXPECT_EQ(aliceEntries[0].amount + bobEntries[0].amount, 0);  // ★ I1

  // balance_after 是稽核欄位，要記錄寫入當下的餘額
  EXPECT_EQ(aliceEntries[0].balanceAfter, 115000);
  EXPECT_EQ(bobEntries[0].balanceAfter, 47000);
}

TEST_F(LedgerCoreTest, AllowsSpendingTheEntireBalance) {
  // 邊界：剛好把錢花光應該成功，不能因為「>=」寫成「>」而被擋
  ASSERT_TRUE(core_->transfer(usdTransfer(kAlice, kBob, 120000)).ok());
  EXPECT_EQ(balanceOf(kAlice), 0);
}

// === 拒絕路徑 ==============================================================

// ★ 這一條就是防 double-spending
TEST_F(LedgerCoreTest, RejectsOverdraft) {
  const auto result = core_->transfer(usdTransfer(kAlice, kBob, 120001));

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error(), ErrorCode::InsufficientFunds);

  // 失敗必須完全沒有副作用
  EXPECT_EQ(balanceOf(kAlice), 120000);
  EXPECT_EQ(balanceOf(kBob), 42000);
  EXPECT_EQ(journal_.entryCount(), 0u);
}

// ★ 這一條若沒擋，執行緒會對同一把 mutex 鎖兩次而卡死自己
TEST_F(LedgerCoreTest, RejectsSelfTransferBeforeTakingLocks) {
  const auto result = core_->transfer(usdTransfer(kAlice, kAlice, 100));
  EXPECT_EQ(result.error(), ErrorCode::SelfTransfer);
  EXPECT_EQ(balanceOf(kAlice), 120000);
}

TEST_F(LedgerCoreTest, RejectsCrossCurrencyTransfer) {
  // USD 帳戶轉給 JPY 帳戶 —— 對應 Stage 2 schema 的複合外鍵所擋下的情況
  TransferRequest req{"k", kAlice, kCarolJpy, 5000, Currency::USD};
  EXPECT_EQ(core_->transfer(req).error(), ErrorCode::CurrencyMismatch);
}

TEST_F(LedgerCoreTest, RejectsRequestWhoseCurrencyDisagreesWithAccounts) {
  // 兩腳都是 USD 帳戶，但請求標成 JPY
  TransferRequest req{"k", kAlice, kBob, 5000, Currency::JPY};
  EXPECT_EQ(core_->transfer(req).error(), ErrorCode::CurrencyMismatch);
}

TEST_F(LedgerCoreTest, RejectsNonPositiveAmounts) {
  EXPECT_EQ(core_->transfer(usdTransfer(kAlice, kBob, 0)).error(), ErrorCode::InvalidAmount);
  EXPECT_EQ(core_->transfer(usdTransfer(kAlice, kBob, -100)).error(), ErrorCode::InvalidAmount);
}

TEST_F(LedgerCoreTest, RejectsUnknownAccounts) {
  EXPECT_EQ(core_->transfer(usdTransfer(kAlice, 999999, 100)).error(), ErrorCode::AccountNotFound);
  EXPECT_EQ(core_->transfer(usdTransfer(999999, kBob, 100)).error(), ErrorCode::AccountNotFound);
}

TEST_F(LedgerCoreTest, RejectsOverflowInsteadOfWrapping) {
  // 系統帳戶允許負餘額，所以可以轉出天文數字而不觸發 InsufficientFunds
  // —— 這樣才測得到溢位那條路徑。
  ASSERT_TRUE(registry_.createWithId(1, 0, Currency::USD, 0, /*allowNegative=*/true).ok());
  ASSERT_TRUE(
      registry_.createWithId(2, 999, Currency::USD, std::numeric_limits<Amount>::max() - 10).ok());

  TransferRequest req{"k", 1, 2, 1000, Currency::USD};
  EXPECT_EQ(core_->transfer(req).error(), ErrorCode::AmountOverflow);
}

// === 系統帳戶 ==============================================================

TEST_F(LedgerCoreTest, SystemAccountsMayGoNegative) {
  ASSERT_TRUE(registry_.createWithId(1, 0, Currency::USD, 0, /*allowNegative=*/true).ok());

  // 錢從系統帳戶流入使用者帳戶。這是資金進入系統的唯一合法方式。
  TransferRequest req{"seed", 1, kAlice, 999999, Currency::USD};
  ASSERT_TRUE(core_->transfer(req).ok());

  EXPECT_EQ(balanceOf(1), -999999);                               // 系統帳戶負了
  EXPECT_EQ(balanceOf(kAlice), 1119999);                          // 使用者收到了
  EXPECT_EQ(core_->totalBalance(Currency::USD), 120000 + 42000);  // 總量沒變 ★
}

// === Registry ==============================================================

TEST_F(LedgerCoreTest, RegistryEnforcesOneAccountPerOwnerAndCurrency) {
  // Alice 已經有 USD 帳戶了
  EXPECT_EQ(registry_.create(101, Currency::USD).error(), ErrorCode::DuplicateAccount);
  // 但可以有 TWD 帳戶
  EXPECT_TRUE(registry_.create(101, Currency::TWD).ok());
}

TEST_F(LedgerCoreTest, AccountPointersStayValidAcrossManyInsertions) {
  // AccountRegistry 的核心前提：Account* 一旦取得就永遠有效，
  // 即使 unordered_map 因為插入而 rehash。
  Account* alice = registry_.find(kAlice);
  ASSERT_NE(alice, nullptr);

  for (OwnerId i = 0; i < 2000; ++i) {
    ASSERT_TRUE(registry_.create(10000 + i, Currency::EUR).ok());
  }

  // 若 Account 不是存在 unique_ptr 裡，這裡就是 use-after-free
  EXPECT_EQ(alice->id(), kAlice);
  EXPECT_EQ(alice->balance(), 120000);
  EXPECT_EQ(registry_.find(kAlice), alice);  // 位址沒變
}

// === 不變式 ================================================================

TEST_F(LedgerCoreTest, InvariantsHoldAfterManyTransfers) {
  for (int i = 0; i < 200; ++i) {
    const bool forward = (i % 2 == 0);
    const auto req = forward ? usdTransfer(kAlice, kBob, 100, "k" + std::to_string(i))
                             : usdTransfer(kBob, kAlice, 100, "k" + std::to_string(i));
    ASSERT_TRUE(core_->transfer(req).ok());
  }

  EXPECT_TRUE(journal_.allTransactionsBalanced());                // I1
  EXPECT_TRUE(core_->verifyInvariants());                         // I1 + I2
  EXPECT_EQ(core_->totalBalance(Currency::USD), 120000 + 42000);  // I3
  EXPECT_EQ(journal_.totalEntryAmount(Currency::USD), 0);         // 分錄總和為 0
  EXPECT_EQ(core_->transferCount(), 200u);
}

TEST_F(LedgerCoreTest, RejectedTransfersLeaveNoTrace) {
  ASSERT_FALSE(core_->transfer(usdTransfer(kAlice, kBob, 999999999)).ok());
  ASSERT_FALSE(core_->transfer(usdTransfer(kAlice, kAlice, 100)).ok());
  ASSERT_FALSE(core_->transfer(usdTransfer(kAlice, 424242, 100)).ok());

  EXPECT_EQ(journal_.transactionCount(), 0u);
  EXPECT_EQ(core_->transferCount(), 0u);
  EXPECT_EQ(core_->rejectedCount(), 3u);
  EXPECT_TRUE(core_->verifyInvariants());
}

}  // namespace
}  // namespace ledger
