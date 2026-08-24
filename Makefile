# ---------------------------------------------------------------------------
# 常用指令的封裝。所有 build 目標都假設你已經在 Linux 環境裡
# （原生 Linux，或 `make shell` 進到容器內）。
# ---------------------------------------------------------------------------

BUILD_DIR ?= build
JOBS      ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

PSQL ?= docker compose exec -T postgres psql -U ledger -d ledger

.PHONY: help up down shell configure build test run clean rebuild tsan asan \
        fmt fmt-check psql db-reset db-seed db-check db-test db-status db-all

help:
	@echo "Docker 環境"
	@echo "  make up          啟動 postgres + engine 容器"
	@echo "  make down        關閉容器（保留資料）"
	@echo "  make shell       進入 engine 容器的 bash"
	@echo ""
	@echo "資料庫（Stage 2）"
	@echo "  make db-status   顯示已套用的 migration 與各表列數"
	@echo "  make db-reset    砍掉重建 schema（會清空所有資料）"
	@echo "  make db-seed     載入開發用種子資料"
	@echo "  make db-check    對現有資料跑不變式健檢 I1–I5"
	@echo "  make db-test     跑 schema 約束測試（22 項）"
	@echo "  make db-all      reset + seed + test + check，一次跑完"
	@echo "  make psql        開啟互動式 psql"
	@echo ""
	@echo "建置（在 Linux 或容器內執行）"
	@echo "  make build       configure + 編譯"
	@echo "  make test        編譯並跑 ctest"
	@echo "  make run         執行 ledger_engine"
	@echo "  make clean       刪除 build 目錄"
	@echo "  make rebuild     clean + build"
	@echo ""
	@echo "Sanitizer（Stage 7）"
	@echo "  make tsan        用 ThreadSanitizer 建置並跑測試"
	@echo "  make asan        用 AddressSanitizer + UBSan 建置並跑測試"
	@echo ""
	@echo "格式"
	@echo "  make fmt         用 clang-format 就地格式化"
	@echo "  make fmt-check   檢查格式但不修改（CI 用）"

# --- Docker ----------------------------------------------------------------

up:
	docker compose up -d --build

down:
	docker compose down

shell:
	docker compose exec engine bash

# --- Database (Stage 2) ----------------------------------------------------
# 這些指令從「宿主機」執行（不是在 engine 容器裡），因為它們透過
# docker compose exec 進到 postgres 容器。先跑過 make up。

psql:
	docker compose exec postgres psql -U ledger -d ledger

db-status:
	@echo "── 已套用的 migration ──"
	@$(PSQL) -c "SELECT version, description, applied_at FROM schema_migrations ORDER BY version;"
	@echo "── 各表列數 ──"
	@$(PSQL) -c "SELECT 'currencies' t, count(*) FROM currencies \
	   UNION ALL SELECT 'accounts', count(*) FROM accounts \
	   UNION ALL SELECT 'transactions', count(*) FROM transactions \
	   UNION ALL SELECT 'entries', count(*) FROM entries;"

# 砍掉重建。public schema 整個丟掉再建，比逐張 DROP TABLE 可靠
# （不用煩惱外鍵相依順序、殘留的 type 與 function）。
db-reset:
	@echo "⚠ 清空資料庫並重新套用所有 migration"
	@$(PSQL) -v ON_ERROR_STOP=1 -q \
	  -c "DROP SCHEMA public CASCADE; CREATE SCHEMA public;"
	@for f in db/migrations/*.sql; do \
	  echo "  apply $$f"; \
	  $(PSQL) -v ON_ERROR_STOP=1 -q < $$f || exit 1; \
	done
	@echo "✓ schema 重建完成"

db-seed:
	@$(PSQL) -v ON_ERROR_STOP=1 < db/seeds/dev_seed.sql

db-check:
	@$(PSQL) < db/checks/invariants.sql

db-test:
	@$(PSQL) < db/checks/constraint_tests.sql

db-all: db-reset db-seed db-test db-check
	@echo ""
	@echo "✓ Stage 2 全部通過：schema 重建、種子載入、22 項約束測試、5 條不變式"

# --- Build -----------------------------------------------------------------

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=RelWithDebInfo

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure

run: build
	./$(BUILD_DIR)/src/ledger_engine

clean:
	rm -rf $(BUILD_DIR) build-tsan build-asan

rebuild: clean build

# --- Sanitizers ------------------------------------------------------------

tsan:
	cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLEDGER_ENABLE_TSAN=ON
	cmake --build build-tsan -j$(JOBS)
	cd build-tsan && ctest --output-on-failure

asan:
	cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLEDGER_ENABLE_ASAN=ON
	cmake --build build-asan -j$(JOBS)
	cd build-asan && ctest --output-on-failure

# --- Formatting ------------------------------------------------------------

SOURCES := $(shell find src include tests -name '*.cpp' -o -name '*.h' 2>/dev/null)

fmt:
	@clang-format -i $(SOURCES) && echo "已格式化 $(words $(SOURCES)) 個檔案"

fmt-check:
	@clang-format --dry-run --Werror $(SOURCES) && echo "格式檢查通過"
