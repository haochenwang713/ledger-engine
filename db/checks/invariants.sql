-- ===========================================================================
-- invariants.sql —— 對現有資料做不變式健檢
--
-- 執行：  make db-check
--
-- 這跟 constraint_tests.sql 是兩件不同的事：
--   constraint_tests.sql 問「壞資料寫得進去嗎？」（測 schema）
--   invariants.sql       問「現在資料庫裡的資料是對的嗎？」（測資料）
--
-- 為什麼兩個都需要：約束只擋得住它認識的錯誤形狀。
-- lost update 寫進去的每一列單獨看都完全合法 —— 餘額是正的、
-- 分錄是平的、幣別是對的。它壞在「餘額快照跟分錄事實對不上」，
-- 那是跨表的關係，必須主動去比對才看得出來。
--
-- Stage 7 併發測試跑完之後，這個腳本就是判定「有沒有 race condition」
-- 的裁判 —— 特別是 I2。
-- ===========================================================================

\pset pager off
\set ON_ERROR_STOP on

\echo ''
\echo '════════════════════════════════════════════════════════════════════'
\echo ' 不變式健檢'
\echo '════════════════════════════════════════════════════════════════════'
\echo ''

-- ---------------------------------------------------------------------------
-- I1：每筆交易的分錄恰好兩筆且相加為零
--
-- 這條由 entries_must_balance 觸發器在寫入時強制，所以正常情況必然通過。
-- 這裡重跑一次是為了防「有人 ALTER TABLE ... DISABLE TRIGGER 之後忘了開回來」
-- 這類情況 —— 約束存在不等於約束生效。
-- ---------------------------------------------------------------------------
\echo '── I1  每筆交易恰好兩筆分錄且相加為零 ─────────────────────────────'
SELECT e.transaction_id,
       count(*)    AS leg_count,
       sum(amount) AS leg_sum
  FROM entries e
 GROUP BY e.transaction_id
HAVING count(*) <> 2 OR sum(amount) <> 0
 ORDER BY e.transaction_id;
\echo '   （空集合 = 通過）'
\echo ''

-- ---------------------------------------------------------------------------
-- I2：帳戶餘額必須等於該帳戶所有分錄的總和
--
-- ★ 這是抓 lost update 的殺手鐧，整份腳本裡最重要的一條。
--
-- lost update 長什麼樣：兩個執行緒同時讀到餘額 1000，各自算出 900 和 950，
-- 各自寫回去。分錄有兩筆（事實是對的），但 balance 只反映了後寫的那一次
-- —— 有一筆扣款憑空消失了。
-- 單看 accounts 表完全正常；只有跟 entries 對照才看得出來。
-- ---------------------------------------------------------------------------
\echo '── I2  餘額快照 = 分錄總和（抓 lost update）★ ─────────────────────'
SELECT a.id,
       a.currency,
       a.balance                     AS snapshot,
       coalesce(sum(e.amount), 0)    AS recomputed,
       a.balance - coalesce(sum(e.amount), 0) AS drift
  FROM accounts a
  LEFT JOIN entries e ON e.account_id = a.id
 GROUP BY a.id, a.currency, a.balance
HAVING a.balance <> coalesce(sum(e.amount), 0)
 ORDER BY a.id;
\echo '   （空集合 = 通過）'
\echo ''

-- ---------------------------------------------------------------------------
-- I3：每個幣別的錢總量守恆
--
-- 因為每筆交易相加為零，所有帳戶餘額加起來也必須是零 —— 包含系統帳戶
-- 那些負餘額在內。系統帳戶負了多少，就代表市面上流通多少錢。
--
-- 多幣別要分開算：把美金和日圓的數字加在一起沒有任何意義。
-- ---------------------------------------------------------------------------
\echo '── I3  每幣別總量守恆（含系統帳戶必須為零）────────────────────────'
SELECT currency,
       sum(balance) FILTER (WHERE NOT allow_negative) AS in_circulation,
       sum(balance) FILTER (WHERE allow_negative)     AS system_accounts,
       sum(balance)                                   AS must_be_zero,
       CASE WHEN sum(balance) = 0 THEN 'PASS' ELSE 'FAIL' END AS result
  FROM accounts
 GROUP BY currency
 ORDER BY currency;
\echo ''

-- ---------------------------------------------------------------------------
-- I4：使用者帳戶不得為負（防 double-spending）
--
-- 由 accounts_balance_non_negative 這條 CHECK 強制。
-- ---------------------------------------------------------------------------
\echo '── I4  使用者帳戶餘額非負（防 double-spending）────────────────────'
SELECT id, owner_id, currency, balance
  FROM accounts
 WHERE NOT allow_negative AND balance < 0
 ORDER BY id;
\echo '   （空集合 = 通過）'
\echo ''

-- ---------------------------------------------------------------------------
-- I5：冪等鍵唯一
--
-- 由 UNIQUE 約束強制。這裡查一次是為了確認索引存在且有效。
-- ---------------------------------------------------------------------------
\echo '── I5  冪等鍵唯一（重送不會扣兩次）───────────────────────────────'
SELECT idempotency_key, count(*) AS occurrences
  FROM transactions
 GROUP BY idempotency_key
HAVING count(*) > 1;
\echo '   （空集合 = 通過）'
\echo ''

-- ---------------------------------------------------------------------------
-- 額外：balance_after 稽核欄位的一致性
--
-- 每筆分錄記錄了寫入當下該帳戶的餘額。依 id 順序重播該帳戶的所有分錄，
-- 累加值應該等於每一筆的 balance_after。
-- 對不上代表分錄的寫入順序與餘額更新順序不一致 —— 併發 bug 的另一種形狀。
-- ---------------------------------------------------------------------------
\echo '── 額外  balance_after 稽核欄位可重播 ─────────────────────────────'
WITH replayed AS (
  SELECT id,
         account_id,
         balance_after,
         sum(amount) OVER (PARTITION BY account_id ORDER BY id
                           ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running
    FROM entries
)
SELECT id AS entry_id, account_id, balance_after, running,
       balance_after - running AS drift
  FROM replayed
 WHERE balance_after <> running
 ORDER BY account_id, id;
\echo '   （空集合 = 通過）'
\echo ''

-- ---------------------------------------------------------------------------
-- 總判定
-- ---------------------------------------------------------------------------
DO $$
DECLARE
  v_i1 INTEGER; v_i2 INTEGER; v_i3 INTEGER; v_i4 INTEGER; v_i5 INTEGER;
  v_total INTEGER;
BEGIN
  SELECT count(*) INTO v_i1 FROM (
    SELECT 1 FROM entries GROUP BY transaction_id
     HAVING count(*) <> 2 OR sum(amount) <> 0) x;

  SELECT count(*) INTO v_i2 FROM (
    SELECT 1 FROM accounts a LEFT JOIN entries e ON e.account_id = a.id
     GROUP BY a.id, a.balance
    HAVING a.balance <> coalesce(sum(e.amount), 0)) x;

  SELECT count(*) INTO v_i3 FROM (
    SELECT 1 FROM accounts GROUP BY currency HAVING sum(balance) <> 0) x;

  SELECT count(*) INTO v_i4 FROM accounts
   WHERE NOT allow_negative AND balance < 0;

  SELECT count(*) INTO v_i5 FROM (
    SELECT 1 FROM transactions GROUP BY idempotency_key HAVING count(*) > 1) x;

  v_total := v_i1 + v_i2 + v_i3 + v_i4 + v_i5;

  RAISE NOTICE '';
  RAISE NOTICE 'I1 交易平衡      違規 % 筆', v_i1;
  RAISE NOTICE 'I2 餘額可重算    違規 % 筆   ★ lost update 偵測', v_i2;
  RAISE NOTICE 'I3 幣別總量守恆  違規 % 種', v_i3;
  RAISE NOTICE 'I4 餘額非負      違規 % 筆', v_i4;
  RAISE NOTICE 'I5 冪等鍵唯一    違規 % 筆', v_i5;
  RAISE NOTICE '';

  IF v_total > 0 THEN
    RAISE EXCEPTION '不變式健檢失敗：共 % 項違規', v_total;
  END IF;

  RAISE NOTICE '✓ 全部不變式通過 —— 帳本是平的';
END;
$$;
