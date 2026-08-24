-- ===========================================================================
-- 000_schema_migrations.sql
--
-- 記錄哪些 migration 跑過了。
--
-- 為什麼需要：docker-entrypoint-initdb.d 只在資料庫「第一次」初始化時執行，
-- 之後就不會再跑。這張表讓你隨時能確認資料庫的 schema 停在哪個版本，
-- 而不是靠記憶或猜測。每個 migration 最後一行都會把自己記進來。
--
-- 這不是完整的 migration runner（沒有 rollback、沒有 checksum），
-- 對一個從空白初始化的專案來說夠用。真的上線的系統會用 Flyway / Liquibase /
-- golang-migrate 之類的工具。
-- ===========================================================================

BEGIN;

CREATE TABLE IF NOT EXISTS schema_migrations (
  version     TEXT        PRIMARY KEY,
  description TEXT        NOT NULL,
  applied_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON TABLE schema_migrations IS
  '已套用的 migration 版本紀錄。每個 migration 檔案結尾自行插入一列。';

INSERT INTO schema_migrations (version, description)
VALUES ('000', 'schema_migrations 版本紀錄表')
ON CONFLICT (version) DO NOTHING;

COMMIT;
