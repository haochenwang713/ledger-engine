-- ===========================================================================
-- dev_seed.sql —— 開發用種子資料
--
-- 這個檔案「不在」db/migrations/ 底下，所以 docker compose 初始化時不會自動跑。
-- 要手動執行：  make db-seed
--
-- 為什麼要有種子資料：Stage 3 之後的手動測試、Stage 8 的壓測，都需要
-- 一組已知餘額的帳戶當起點。而且它示範了「錢怎麼合法進入系統」。
--
-- 重點觀念 —— 錢不能憑空出現：
--   雙式記帳的世界裡沒有「直接把餘額設成 1200」這種操作。
--   要讓 Alice 有 $1,200，必須有另一個帳戶少了 $1,200。
--   那個帳戶就是「系統帳戶」（allow_negative = TRUE），它會累積負餘額，
--   而負餘額的絕對值就等於系統中流通的錢總量。
--
--   所以下面的初始資金是用「從系統帳戶轉帳」達成的，不是 UPDATE balance。
--   這樣不變式 I1（每筆交易相加為零）從第一筆資料就成立。
-- ===========================================================================

BEGIN;

-- ---------------------------------------------------------------------------
-- dev_transfer() —— 開發與測試用的轉帳輔助函式
--
-- ⚠ 這只是給 Stage 2–5 手動驗證用的鷹架。
--   正式的轉帳邏輯在 Stage 6 由 C++ 的 LedgerRepository 實作。
--   兩者發出的 SQL 應該一模一樣 —— 這個函式就是那段 SQL 的可執行規格。
--
-- 注意 ORDER BY id：SELECT ... FOR UPDATE 的取鎖順序必須與 C++ 端
-- 記憶體鎖的順序一致（都是 account_id 遞增），否則會出現
-- 「記憶體不死鎖但 Postgres row lock 死鎖」的情況（SQLSTATE 40P01）。
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION dev_transfer(
  p_idem_key   TEXT,
  p_from       BIGINT,
  p_to         BIGINT,
  p_amount     BIGINT
) RETURNS BIGINT
LANGUAGE plpgsql AS $$
DECLARE
  v_ccy      CHAR(3);
  v_from_bal BIGINT;
  v_to_bal   BIGINT;
  v_tx_id    BIGINT;
BEGIN
  IF p_from = p_to THEN
    RAISE EXCEPTION '不能轉給自己（account %）', p_from;
  END IF;
  IF p_amount <= 0 THEN
    RAISE EXCEPTION '金額必須為正，收到 %', p_amount;
  END IF;

  -- 依 id 遞增順序鎖住兩列 —— 與記憶體鎖同一個順序，這是防死鎖的關鍵。
  PERFORM id FROM accounts
   WHERE id IN (p_from, p_to)
   ORDER BY id
   FOR UPDATE;

  SELECT currency, balance INTO v_ccy, v_from_bal FROM accounts WHERE id = p_from;
  SELECT balance          INTO v_to_bal          FROM accounts WHERE id = p_to;

  -- 標頭。若 idempotency_key 重複，這裡會拋 23505。
  INSERT INTO transactions
    (idempotency_key, status, currency, amount, debit_account_id, credit_account_id)
  VALUES
    (p_idem_key, 'COMMITTED', v_ccy, p_amount, p_from, p_to)
  RETURNING id INTO v_tx_id;

  -- 兩筆分錄：一負一正，相加為零。
  INSERT INTO entries (transaction_id, account_id, currency, amount, balance_after)
  VALUES
    (v_tx_id, p_from, v_ccy, -p_amount, v_from_bal - p_amount),
    (v_tx_id, p_to,   v_ccy,  p_amount, v_to_bal   + p_amount);

  -- 更新餘額快照。
  UPDATE accounts SET balance = balance - p_amount,
                      version = version + 1,
                      updated_at = now()
   WHERE id = p_from;
  UPDATE accounts SET balance = balance + p_amount,
                      version = version + 1,
                      updated_at = now()
   WHERE id = p_to;

  RETURN v_tx_id;
END;
$$;

COMMENT ON FUNCTION dev_transfer(TEXT, BIGINT, BIGINT, BIGINT) IS
  '開發測試用轉帳。Stage 6 的 C++ LedgerRepository 會發出等價的 SQL。';


-- ---------------------------------------------------------------------------
-- 系統帳戶（owner_id = 0）。允許負餘額 —— 錢從這裡流入系統。
-- ---------------------------------------------------------------------------
INSERT INTO accounts (id, owner_id, currency, balance, allow_negative) VALUES
  (   1, 0, 'USD', 0, TRUE),
  (   2, 0, 'JPY', 0, TRUE),
  (   3, 0, 'TWD', 0, TRUE);

-- ---------------------------------------------------------------------------
-- 使用者帳戶，初始餘額 0。資金稍後由系統帳戶轉入。
-- id 刻意用好認的數字，方便對照設計文件裡的例子。
-- ---------------------------------------------------------------------------
INSERT INTO accounts (id, owner_id, currency) VALUES
  (1001, 101, 'USD'),   -- Alice USD
  (2002, 102, 'USD'),   -- Bob   USD
  (3003, 103, 'JPY'),   -- Carol JPY
  (4004, 104, 'JPY'),   -- Dave  JPY
  (5005, 101, 'TWD');   -- Alice TWD（同一個 owner 可以有多幣別帳戶）

-- 明確指定 id 不會推進 IDENTITY 的序列，之後自動產生的 id 會從 1000 開始撞。
-- 把序列推到現有最大值之後。
SELECT setval(pg_get_serial_sequence('accounts', 'id'),
              (SELECT max(id) FROM accounts));

-- ---------------------------------------------------------------------------
-- 初始資金：從系統帳戶轉出。系統帳戶因此變成負餘額，這是正確的。
-- ---------------------------------------------------------------------------
SELECT dev_transfer('seed-fund-alice-usd', 1,    1001,  120000);  -- $1,200.00
SELECT dev_transfer('seed-fund-bob-usd',   1,    2002,   42000);  -- $  420.00
SELECT dev_transfer('seed-fund-carol-jpy', 2,    3003,  500000);  -- ¥500,000（exponent 0）
SELECT dev_transfer('seed-fund-dave-jpy',  2,    4004,   80000);  -- ¥ 80,000
SELECT dev_transfer('seed-fund-alice-twd', 3,    5005, 3500000);  -- NT$35,000.00

-- ---------------------------------------------------------------------------
-- 設計文件裡那筆範例交易：Alice 轉 $50.00 給 Bob。
-- 之後 Alice 應為 115000（$1,150.00），Bob 應為 47000（$470.00）。
-- ---------------------------------------------------------------------------
SELECT dev_transfer('req-a3f9-01', 1001, 2002, 5000);

COMMIT;

\echo ''
\echo '=== 種子資料完成 ==='
SELECT a.id,
       a.owner_id,
       a.currency,
       a.balance,
       -- 用 exponent 把最小單位還原成人看的金額 —— 注意 JPY 不會被除以 100
       to_char(a.balance / power(10, c.exponent)::numeric,
               'FM999,999,999.' || repeat('0', c.exponent)) AS human,
       a.allow_negative AS is_system
  FROM accounts a
  JOIN currencies c ON c.code = a.currency
 ORDER BY a.id;
