CXX ?= c++
CXXFLAGS ?= -std=c++20 -O3 -DNDEBUG -pthread -march=native
LDFLAGS ?=

SRC := $(wildcard *.cpp)
BIN_DIR := bin
BENCH_DIR := benchmarks
TEST_DIR := tests
BINS := $(patsubst %.cpp,$(BIN_DIR)/%,$(SRC))

all: $(BINS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BENCH_DIR):
	mkdir -p $(BENCH_DIR)

$(BIN_DIR)/%: %.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

bench: $(BINS) | $(BENCH_DIR)
	@for b in $(BINS); do \
		name=$$(basename $$b); \
		out="$(BENCH_DIR)/$$name.txt"; \
		{ \
			echo "Benchmark: $$name"; \
			echo "Date: $$(date -u '+%Y-%m-%d %H:%M:%SZ')"; \
			echo "Host: $$(uname -s) $$(uname -m)"; \
			echo "Compiler: $(CXX)"; \
			echo "CXXFLAGS: $(CXXFLAGS)"; \
			echo "----------------------------------------"; \
		} > $$out; \
		$$b >> $$out; \
		echo >> $$out; \
	done

bench-smoke: $(BINS)
	@./scripts/bench_smoke.sh

test: $(BINS)
	@./tests/smoke.sh

clean:
	rm -rf $(BIN_DIR) $(BENCH_DIR)

.PHONY: all bench bench-smoke test clean
