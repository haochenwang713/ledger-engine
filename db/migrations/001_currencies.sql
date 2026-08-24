-- ===========================================================================
-- 001_currencies.sql
--
-- 幣別參照表。這張表是整個系統「一塊錢是多少」的唯一定義來源。
--
-- 核心概念 —— exponent（小數位數）：
--   所有金額在資料庫裡都是 BIGINT，存的是該幣別的「最小單位」。
--   exponent 告訴你要把那個整數往左移幾位才是人看的金額。
--
--     USD, exponent = 2 → 5000 代表 $50.00      （5000 / 10^2）
--     JPY, exponent = 0 → 5000 代表 ¥5,000      （5000 / 10^0）
--
--   同一個 5000，差一百倍。任何顯示、解析、日誌都必須查這張表，
--   絕對不能在程式裡寫死 /100。這是多幣別系統最常見的線上事故來源。
--
-- 為什麼不用 NUMERIC/DECIMAL 存金額：
--   BIGINT 的加減是精確且原子的，比較也不會有意外。NUMERIC 在 Postgres 裡
--   是變長型別，運算較慢，而且它容許小數位數超出幣別定義（例如在 JPY 欄位
--   存 0.5 圓），等於把「不可能的值」變成「可以存進去的值」。
--   整數 + exponent 讓不合法的狀態根本無法表示。
-- ===========================================================================

BEGIN;

CREATE TABLE currencies (
  code     CHAR(3)  PRIMARY KEY,
  name     TEXT     NOT NULL,
  exponent SMALLINT NOT NULL,

  -- ISO 4217 的 exponent 實際範圍是 0–4（例如 BHD 是 3）。
  -- 這裡限制在 0–4，超出範圍的資料進不來。
  CONSTRAINT currencies_exponent_range CHECK (exponent BETWEEN 0 AND 4),

  -- 幣別代碼一律大寫三個英文字母。'usd' 或 'US' 會被擋掉。
  CONSTRAINT currencies_code_format CHECK (code ~ '^[A-Z]{3}$')
);

COMMENT ON TABLE  currencies IS
  'ISO 4217 幣別。exponent = 最小單位的小數位數，決定 BIGINT 金額怎麼解讀。';
COMMENT ON COLUMN currencies.exponent IS
  '小數位數。USD/EUR/GBP/CNY/TWD = 2（分），JPY = 0（圓，無小數）。';

-- ---------------------------------------------------------------------------
-- 支援的六種幣別（依經濟體規模挑選）。
-- 這是參照資料（reference data），不是使用者資料，所以直接寫在 migration 裡。
-- ---------------------------------------------------------------------------
INSERT INTO currencies (code, name, exponent) VALUES
  ('USD', 'US Dollar',          2),
  ('EUR', 'Euro',               2),
  ('JPY', 'Japanese Yen',       0),   -- ← 唯一 exponent = 0 的，刻意留的測試案例
  ('GBP', 'Pound Sterling',     2),
  ('CNY', 'Chinese Yuan',       2),
  ('TWD', 'New Taiwan Dollar',  2);

INSERT INTO schema_migrations (version, description)
VALUES ('001', 'currencies 幣別參照表 + 六種幣別種子資料');

COMMIT;
