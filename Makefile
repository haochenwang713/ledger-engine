# ---------------------------------------------------------------------------
# 常用指令的封裝。所有 build 目標都假設你已經在 Linux 環境裡
# （原生 Linux，或 `make shell` 進到容器內）。
# ---------------------------------------------------------------------------

BUILD_DIR ?= build
JOBS      ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# ---------------------------------------------------------------------------
# 資料庫連線方式：自動偵測。
#
#   有 docker compose  → 用容器裡的 postgres（對外 5433）
#   沒有               → 用本機安裝的 postgres（預設 5432）
#
# 為什麼要有 fallback：Docker Desktop 在 Mac 上是個不小的安裝，而 Stage 2/3
# 其實只需要一個 Postgres。用 brew 裝的原生 Postgres 一樣能跑完所有檢查，
# 等 Stage 4 真的要用 epoll 時再處理容器就好。
#
# 要強制指定的話：
#   make db-all LEDGER_DB=docker
#   make db-all LEDGER_DB=native
# ---------------------------------------------------------------------------
LEDGER_DB ?= auto
PGPORT    ?= 5432

ifeq ($(LEDGER_DB),auto)
  LEDGER_DB := $(shell docker compose version >/dev/null 2>&1 && echo docker || echo native)
endif

ifeq ($(LEDGER_DB),docker)
  PSQL ?= docker compose exec -T postgres psql -U ledger -d ledger
  PSQL_INTERACTIVE ?= docker compose exec postgres psql -U ledger -d ledger
else
  PSQL ?= psql -h localhost -p $(PGPORT) -U ledger -d ledger
  PSQL_INTERACTIVE ?= $(PSQL)
  # 若 pg_hba 設成要密碼就會用到；Homebrew 預設是 trust，這行不會有影響。
  export PGPASSWORD ?= ledger_dev_password
endif

.PHONY: help up down shell configure build test e2e run clean rebuild tsan asan \
        fmt fmt-check psql db-reset db-seed db-check db-test db-status db-all \
        db-native-setup doctor

help:
	@echo "先跑這個"
	@echo "  make doctor      檢查環境，告訴你缺什麼、下一步該做什麼"
	@echo ""
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
	@echo "  make test        編譯並跑 ctest（C++ 單元與整合測試）"
	@echo "  make e2e         用 pytest 從外部驅動真的伺服器跑端對端測試"
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

# --- Doctor ----------------------------------------------------------------
# 環境健檢。卡住的時候先跑這個，它會告訴你缺什麼。

doctor:
	@echo "═══ 環境檢查 ═══"
	@printf '%-22s' "作業系統"; uname -s
	@printf '%-22s' "C++ 編譯器"; (c++ --version 2>/dev/null | head -1) || echo "✗ 找不到"
	@printf '%-22s' "CMake"; (cmake --version 2>/dev/null | head -1) || echo "✗ 找不到"
	@printf '%-22s' "git"; (git --version 2>/dev/null) || echo "✗ 找不到"
	@printf '%-22s' "docker"; (docker --version 2>/dev/null) || echo "— 未安裝（Stage 4 之前不需要）"
	@printf '%-22s' "docker compose"; (docker compose version 2>/dev/null | head -1) \
	   || echo "— 不可用（要先開 Docker Desktop）"
	@printf '%-22s' "psql"; (psql --version 2>/dev/null) || echo "— 未安裝"
	@echo ""
	@echo "資料庫模式：$(LEDGER_DB)"
	@if [ "$(LEDGER_DB)" = "native" ]; then \
	  printf '%-22s' "本機 Postgres"; \
	  (pg_isready -h localhost -p $(PGPORT) 2>/dev/null) || echo "✗ 沒有回應（port $(PGPORT)）"; \
	else \
	  printf '%-22s' "容器 Postgres"; \
	  (docker compose ps postgres 2>/dev/null | tail -1) || echo "✗ 未啟動"; \
	fi
	@echo ""
	@echo "═══ 各 Stage 需要什麼 ═══"
	@echo "  Stage 1-3   只需要 C++ 編譯器 + CMake        →  make test"
	@echo "  Stage 2     再加一個 Postgres（原生或容器）  →  make db-all"
	@echo "  Stage 4+    需要 Linux（epoll）              →  make up && make shell"

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
	$(PSQL_INTERACTIVE)

# 在本機（非容器）的 Postgres 上建立 ledger 角色與資料庫。只需要跑一次。
#
# 前提：brew install postgresql@16 && brew services start postgresql@16
#
# PGSUPERUSER 預設是目前登入的使用者 —— Homebrew 安裝的 Postgres 會把
# 安裝者設成 superuser，所以在 Mac 上通常不用改。
# 若你的環境不是這樣：make db-native-setup PGSUPERUSER=postgres
PGSUPERUSER ?= $(shell whoami)

db-native-setup:
	@echo "在本機 Postgres（port $(PGPORT)，以 $(PGSUPERUSER) 連線）建立 ledger 角色與資料庫"
	@psql -h localhost -p $(PGPORT) -U $(PGSUPERUSER) -d postgres -tAc \
	   "SELECT 1 FROM pg_roles WHERE rolname='ledger'" | grep -q 1 \
	   || psql -h localhost -p $(PGPORT) -U $(PGSUPERUSER) -d postgres -q -c \
	      "CREATE ROLE ledger LOGIN PASSWORD 'ledger_dev_password'"
	@psql -h localhost -p $(PGPORT) -U $(PGSUPERUSER) -d postgres -tAc \
	   "SELECT 1 FROM pg_database WHERE datname='ledger'" | grep -q 1 \
	   || psql -h localhost -p $(PGPORT) -U $(PGSUPERUSER) -d postgres -q -c \
	      "CREATE DATABASE ledger OWNER ledger"
	@echo "✓ 完成。接著跑 make db-all"

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

# 端對端測試：用 pytest 從外部驅動真的 ledger_engine。
#
# 它跟 ctest 測的不是同一件事。ctest 從內部呼叫 C++ 函式；這裡只有
# socket 與 JSON，看到的東西跟瀏覽器、壓測腳本、或一個打 nc 的人一模一樣。
# Stage 6 把記憶體換成 PostgreSQL 之後，這一組測試應該一行都不用改。
#
# 需要 Linux（伺服器要 epoll）。在 Mac 上先 make shell 進容器。
e2e: build
	cd tests/e2e && python3 -m pytest

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
