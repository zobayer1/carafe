# Thin wrapper around CMake presets: every target shells out to `cmake --preset`
# or `ctest --preset`, so CMakePresets.json stays the single source of truth and
# each preset builds into its own build/<preset>/.
#
#   cmake --preset debug && cmake --build --preset debug && ctest --preset debug

PRESET ?= debug
JOBS   ?= $(shell nproc 2>/dev/null || echo 4)

BUILD_DIR = build/$(PRESET)

SOURCE_DIRS := src include tests examples
CXX_FILES    = $(shell find $(SOURCE_DIRS) -type f \( -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null)

.DEFAULT_GOAL := help
.PHONY: help configure build rebuild test run clean distclean format format-check \
        tidy debug release asan coverage compdb

help: ## Show this help
	@echo "carafe -- available targets:"
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'
	@echo ""
	@echo "Presets: debug release asan coverage"
	@echo "Current: PRESET=$(PRESET) -> $(BUILD_DIR)   JOBS=$(JOBS)"
	@echo "Override per invocation, e.g. make PRESET=release test"

$(BUILD_DIR)/CMakeCache.txt:
	@$(MAKE) --no-print-directory configure

configure: ## Configure the current preset
	cmake --preset $(PRESET)

build: $(BUILD_DIR)/CMakeCache.txt ## Build the current preset, configuring first if needed
	cmake --build --preset $(PRESET) -j $(JOBS)

rebuild: clean build ## Clean and build again

test: build ## Build, then run the suite through CTest
	ctest --preset $(PRESET)

run: build ## Build and run the hello example
	@$(BUILD_DIR)/bin/hello

debug: ## Build + test the debug preset
	@$(MAKE) --no-print-directory PRESET=debug test

release: ## Build + test the release preset
	@$(MAKE) --no-print-directory PRESET=release test

asan: ## Build + test with AddressSanitizer + UBSan
	@$(MAKE) --no-print-directory PRESET=asan test

coverage: ## Build + test with gcov, then print a coverage summary
	@$(MAKE) --no-print-directory PRESET=coverage test
	@command -v gcovr >/dev/null 2>&1 \
		&& gcovr --root . --filter 'src/|include/' --exclude-unreachable-branches \
			--print-summary build/coverage \
		|| echo "gcovr not installed -- try: pipx install gcovr"

format: ## Reformat all sources in place with clang-format
	@echo "$(CXX_FILES)" | tr ' ' '\n' | grep . | xargs -r clang-format -i
	@echo "formatted."

format-check: ## Fail if any source is not clang-format clean
	@echo "$(CXX_FILES)" | tr ' ' '\n' | grep . | xargs -r clang-format --dry-run --Werror

# The regex matches paths in the compilation database; anchoring it to $(CURDIR)
# keeps clang-tidy off _deps/ sources.
tidy: build ## Run clang-tidy over the project sources
	@command -v run-clang-tidy >/dev/null 2>&1 \
		&& run-clang-tidy -p $(BUILD_DIR) -quiet '^$(CURDIR)/(src|tests|examples)/' \
		|| clang-tidy -p $(BUILD_DIR) $(shell find src -name '*.cpp')

compdb: $(BUILD_DIR)/CMakeCache.txt ## Symlink compile_commands.json to the project root
	@ln -sf $(BUILD_DIR)/compile_commands.json compile_commands.json
	@echo "compile_commands.json -> $(BUILD_DIR)/compile_commands.json"

clean: ## Delete build artifacts for the current preset, keep its CMake cache
	@cmake --build --preset $(PRESET) --target clean 2>/dev/null || true

distclean: ## Delete every build directory
	rm -rf build compile_commands.json
