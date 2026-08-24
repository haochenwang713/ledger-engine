-- ===========================================================================
-- constraint_tests.sql —— 驗證資料庫真的擋得住壞資料
--
-- 執行：  make db-test        （會先 db-reset + db-seed）
--
-- 這個檔案的用途不是「測試我們的程式」，而是「測試 schema 本身」。
-- 每一項都故意寫入違規資料，然後確認資料庫拒絕它。
--
-- 為什麼這件事值得單獨測：
--   應用層的檢查會被繞過 —— 一個手滑的 psql、一支修資料的腳本、
--   一個忘了檢查的新程式碼路徑。資料庫層的約束是最後一道防線，
--   而「以為有約束但其實沒生效」是最糟的情況：你會以為自己有保護。
--
-- 任何一項失敗，整個腳本會以非零狀態結束，CI 就會紅燈。
-- ===========================================================================

\set ON_ERROR_STOP on
\pset pager off

BEGIN;

CREATE TEMP TABLE test_results (
  seq      SERIAL,
  name     TEXT,
  passed   BOOLEAN,
  detail   TEXT
);

-- ---------------------------------------------------------------------------
-- expect_reject(label, stmt)
--   執行 stmt，預期它「失敗」。成功執行 = 測試不通過。
--
--   每次呼叫都包在自己的 subtransaction（BEGIN ... EXCEPTION 會隱含建立
--   savepoint），所以一項失敗不會污染後續測試。
-- ---------------------------------------------------------------------------
CREATE FUNCTION pg_temp.expect_reject(p_label TEXT, p_stmt TEXT) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
  BEGIN
    EXECUTE p_stmt;
    -- 延後檢查的約束要在這裡強制觸發，否則它們要等到 COMMIT 才驗證，
    -- 而我們在 subtransaction 裡永遠等不到那一刻。
    SET CONSTRAINTS ALL IMMEDIATE;

    INSERT INTO test_results (name, passed, detail)
    VALUES (p_label, FALSE, '⚠ 這筆違規資料竟然被接受了');
  EXCEPTION WHEN OTHERS THEN
    INSERT INTO test_results (name, passed, detail)
    VALUES (p_label, TRUE, SQLSTATE || ' ' || left(SQLERRM, 70));
  END;
END;
$$;

-- ---------------------------------------------------------------------------
-- expect_accept(label, stmt)
--   正面測試：預期它「成功」。用來確認約束沒有寬到擋掉合法資料。
-- ---------------------------------------------------------------------------
CREATE FUNCTION pg_temp.expect_accept(p_label TEXT, p_stmt TEXT) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
  BEGIN
    EXECUTE p_stmt;
    SET CONSTRAINTS ALL IMMEDIATE;

    INSERT INTO test_results (name, passed, detail)
    VALUES (p_label, TRUE, 'accepted');
  EXCEPTION WHEN OTHERS THEN
    INSERT INTO test_results (name, passed, detail)
    VALUES (p_label, FALSE, '⚠ 合法資料被擋: ' || SQLSTATE || ' ' || left(SQLERRM, 60));
  END;
END;
$$;


-- ===========================================================================
-- 群組一：accounts —— 防超額扣款
-- ===========================================================================

SELECT pg_temp.expect_reject(
  '使用者帳戶餘額不得為負 (I4 / 防 double-spending)',
  $$UPDATE accounts SET balance = -1 WHERE id = 1001$$);

SELECT pg_temp.expect_accept(
  '系統帳戶允許負餘額（錢由此流入系統）',
  $$UPDATE accounts SET balance = -999 WHERE id = 1$$);

SELECT pg_temp.expect_reject(
  '帳戶狀態只能是 0 或 1',
  $$UPDATE accounts SET status = 7 WHERE id = 1001$$);

SELECT pg_temp.expect_reject(
  '同一 owner 同一幣別不得有兩個帳戶',
  $$INSERT INTO accounts (owner_id, currency) VALUES (101, 'USD')$$);

SELECT pg_temp.expect_reject(
  '不存在的幣別代碼',
  $$INSERT INTO accounts (owner_id, currency) VALUES (999, 'XXX')$$);


-- ===========================================================================
-- 群組二：currencies —— 參照資料的完整性
-- ===========================================================================

SELECT pg_temp.expect_reject(
  '幣別代碼必須是三個大寫字母',
  $$INSERT INTO currencies (code, name, exponent) VALUES ('usd', 'lower', 2)$$);

SELECT pg_temp.expect_reject(
  'exponent 必須落在 0–4',
  $$INSERT INTO currencies (code, name, exponent) VALUES ('XAU', 'Gold', 9)$$);


-- ===========================================================================
-- 群組三：transactions —— 冪等性與幣別一致性
-- ===========================================================================

SELECT pg_temp.expect_reject(
  '重複的 idempotency_key（防重複扣款的核心保證）',
  $$INSERT INTO transactions
      (idempotency_key, currency, amount, debit_account_id, credit_account_id)
    VALUES ('req-a3f9-01', 'USD', 100, 1001, 2002)$$);

SELECT pg_temp.expect_reject(
  '自己轉給自己（若不擋，記憶體端會對同一把 mutex 鎖兩次）',
  $$INSERT INTO transactions
      (idempotency_key, currency, amount, debit_account_id, credit_account_id)
    VALUES ('t-self', 'USD', 100, 1001, 1001)$$);

SELECT pg_temp.expect_reject(
  '轉帳金額必須為正（0 元）',
  $$INSERT INTO transactions
      (idempotency_key, currency, amount, debit_account_id, credit_account_id)
    VALUES ('t-zero', 'USD', 0, 1001, 2002)$$);

SELECT pg_temp.expect_reject(
  '轉帳金額必須為正（負數）',
  $$INSERT INTO transactions
      (idempotency_key, currency, amount, debit_account_id, credit_account_id)
    VALUES ('t-neg', 'USD', -100, 1001, 2002)$$);

SELECT pg_temp.expect_reject(
  '空白的 idempotency_key',
  $$INSERT INTO transactions
      (idempotency_key, currency, amount, debit_account_id, credit_account_id)
    VALUES ('   ', 'USD', 100, 1001, 2002)$$);

SELECT pg_temp.expect_reject(
  '跨幣別轉帳：USD 帳戶轉給 JPY 帳戶（複合外鍵擋下）',
  $$INSERT INTO transactions
      (idempotency_key, currency, amount, debit_account_id, credit_account_id)
    VALUES ('t-fx', 'USD', 100, 1001, 3003)$$);

SELECT pg_temp.expect_reject(
  '交易幣別與帳戶幣別不符：兩腳都是 USD 帳戶但標成 JPY',
  $$INSERT INTO transactions
      (idempotency_key, currency, amount, debit_account_id, credit_account_id)
    VALUES ('t-wrong-ccy', 'JPY', 100, 1001, 2002)$$);


-- ===========================================================================
-- 群組四：entries —— 雙式記帳不變式（延後檢查的約束觸發器）
-- ===========================================================================

SELECT pg_temp.expect_reject(
  '不平衡的交易：兩腳相加不為零 (I1)',
  $$WITH t AS (
      INSERT INTO transactions
        (idempotency_key, currency, amount, debit_account_id, credit_account_id)
      VALUES ('t-unbalanced', 'USD', 100, 1001, 2002) RETURNING id)
    INSERT INTO entries (transaction_id, account_id, currency, amount, balance_after)
    SELECT t.id, 1001, 'USD', -100, 0 FROM t
    UNION ALL
    SELECT t.id, 2002, 'USD',  999, 0 FROM t$$);

SELECT pg_temp.expect_reject(
  '只有一腳的交易（單邊記帳）',
  $$WITH t AS (
      INSERT INTO transactions
        (idempotency_key, currency, amount, debit_account_id, credit_account_id)
      VALUES ('t-one-leg', 'USD', 100, 1001, 2002) RETURNING id)
    INSERT INTO entries (transaction_id, account_id, currency, amount, balance_after)
    SELECT t.id, 1001, 'USD', -100, 0 FROM t$$);

SELECT pg_temp.expect_reject(
  '同一交易裡同一帳戶出現兩次',
  $$WITH t AS (
      INSERT INTO transactions
        (idempotency_key, currency, amount, debit_account_id, credit_account_id)
      VALUES ('t-dup-leg', 'USD', 100, 1001, 2002) RETURNING id)
    INSERT INTO entries (transaction_id, account_id, currency, amount, balance_after)
    SELECT t.id, 1001, 'USD', -100, 0 FROM t
    UNION ALL
    SELECT t.id, 1001, 'USD',  100, 0 FROM t$$);

SELECT pg_temp.expect_reject(
  '分錄金額不得為零',
  $$WITH t AS (
      INSERT INTO transactions
        (idempotency_key, currency, amount, debit_account_id, credit_account_id)
      VALUES ('t-zero-leg', 'USD', 100, 1001, 2002) RETURNING id)
    INSERT INTO entries (transaction_id, account_id, currency, amount, balance_after)
    SELECT t.id, 1001, 'USD', 0, 0 FROM t$$);

SELECT pg_temp.expect_reject(
  '分錄幣別與其帳戶幣別不符（複合外鍵擋下）',
  $$WITH t AS (
      INSERT INTO transactions
        (idempotency_key, currency, amount, debit_account_id, credit_account_id)
      VALUES ('t-leg-ccy', 'USD', 100, 1001, 2002) RETURNING id)
    INSERT INTO entries (transaction_id, account_id, currency, amount, balance_after)
    SELECT t.id, 1001, 'JPY', -100, 0 FROM t$$);


-- ===========================================================================
-- 群組五：append-only —— 稽核軌跡不可竄改
-- ===========================================================================

SELECT pg_temp.expect_reject(
  'entries 不得 UPDATE（改歷史）',
  $$UPDATE entries SET amount = amount * 2 WHERE transaction_id = 900005$$);

SELECT pg_temp.expect_reject(
  'entries 不得 DELETE（湮滅證據）',
  $$DELETE FROM entries WHERE transaction_id = 900005$$);

SELECT pg_temp.expect_reject(
  '有分錄的 transaction 不得刪除',
  $$DELETE FROM transactions WHERE id = 900005$$);


-- ===========================================================================
-- 結果
-- ===========================================================================

\echo ''
\echo '════════════════════════════════════════════════════════════════════'
\echo ' schema 約束測試'
\echo '════════════════════════════════════════════════════════════════════'

SELECT CASE WHEN passed THEN '  PASS' ELSE '  FAIL' END AS result,
       name,
       detail
  FROM test_results
 ORDER BY seq;

\echo ''

SELECT count(*) FILTER (WHERE passed)     AS passed,
       count(*) FILTER (WHERE NOT passed) AS failed,
       count(*)                           AS total
  FROM test_results;

-- 有任何一項失敗就讓整個腳本以錯誤結束，CI 才會紅燈。
DO $$
DECLARE n INTEGER;
BEGIN
  SELECT count(*) INTO n FROM test_results WHERE NOT passed;
  IF n > 0 THEN
    RAISE EXCEPTION '% 項 schema 約束測試失敗', n;
  END IF;
  RAISE NOTICE '全部 schema 約束測試通過';
END;
$$;

-- 整份測試都在同一個 transaction 裡，最後全部丟掉，
-- 資料庫回到執行前的狀態（種子資料完好）。
ROLLBACK;
