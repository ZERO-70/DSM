# Distributed Shared Memory (DSM) System
# Makefile for building the DSM library and test programs

CC = gcc
CFLAGS = -Wall -Wextra -Werror -g -O2 -pthread -I./include
LDFLAGS = -pthread

# Directories
SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj
BIN_DIR = bin

# Library output
LIB_NAME = libdsm
LIB_STATIC = $(BIN_DIR)/$(LIB_NAME).a
LIB_SHARED = $(BIN_DIR)/$(LIB_NAME).so

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

# Test programs
TEST_SRC = tests/test_dsm.c
TEST_BIN = $(BIN_DIR)/test_dsm

# Default target
.PHONY: all
all: dirs $(LIB_STATIC) $(LIB_SHARED) $(TEST_BIN)

# Create directories
.PHONY: dirs
dirs:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR)

# Compile object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# Build static library
$(LIB_STATIC): $(OBJS)
	ar rcs $@ $^
	@echo "Built static library: $@"

# Build shared library
$(LIB_SHARED): $(OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)
	@echo "Built shared library: $@"

# Build test program
$(TEST_BIN): tests/test_dsm.c $(LIB_STATIC)
	@mkdir -p tests
	$(CC) $(CFLAGS) -o $@ $< -L$(BIN_DIR) -ldsm $(LDFLAGS)
	@echo "Built test program: $@"

# Clean
.PHONY: clean
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts"

# Install (to /usr/local by default)
PREFIX ?= /usr/local
.PHONY: install
install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(PREFIX)/lib
	install -d $(PREFIX)/include/dsm
	install -m 644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 755 $(LIB_SHARED) $(PREFIX)/lib/
	install -m 644 $(INC_DIR)/*.h $(PREFIX)/include/dsm/
	ldconfig 2>/dev/null || true
	@echo "Installed to $(PREFIX)"

# Uninstall
.PHONY: uninstall
uninstall:
	rm -f $(PREFIX)/lib/$(LIB_NAME).a
	rm -f $(PREFIX)/lib/$(LIB_NAME).so
	rm -rf $(PREFIX)/include/dsm
	ldconfig 2>/dev/null || true
	@echo "Uninstalled from $(PREFIX)"

# Run test
.PHONY: test
test: $(TEST_BIN)
	@echo "Running DSM test..."
	@echo "Start test as: $(TEST_BIN) <port> <chunks>"
	@echo "Example: $(TEST_BIN) 8080 10"

# Debug build
.PHONY: debug
debug: CFLAGS += -DDEBUG -O0
debug: clean all

# Release build
.PHONY: release
release: CFLAGS += -DNDEBUG -O3
release: clean all

# Show help
.PHONY: help
help:
	@echo "DSM Build System"
	@echo "================"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and test program (default)"
	@echo "  clean    - Remove build artifacts"
	@echo "  install  - Install to PREFIX (default: /usr/local)"
	@echo "  test     - Show test instructions"
	@echo "  debug    - Build with debug flags"
	@echo "  release  - Build with release optimizations"
	@echo "  help     - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX   - Installation prefix (default: /usr/local)"
	@echo ""
	@echo "Usage:"
	@echo "  make                 # Build everything"
	@echo "  make clean           # Clean build"
	@echo "  make install PREFIX=/opt/dsm"
