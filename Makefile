# ---------------------------------------------------------------------------
# 常用指令的封裝。所有 build 目標都假設你已經在 Linux 環境裡
# （原生 Linux，或 `make shell` 進到容器內）。
# ---------------------------------------------------------------------------

BUILD_DIR ?= build
JOBS      ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: help up down shell configure build test run clean rebuild tsan asan fmt fmt-check

help:
	@echo "Docker 環境"
	@echo "  make up          啟動 postgres + engine 容器"
	@echo "  make down        關閉容器（保留資料）"
	@echo "  make shell       進入 engine 容器的 bash"
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
