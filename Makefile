# DSM - Distributed Shared Memory System
# Makefile

CC = gcc
CFLAGS = -Wall -Wextra -Werror -O2 -g
CFLAGS += -D_GNU_SOURCE -D_XOPEN_SOURCE=600

# Source files
SRCS = dsm.c memory.c network.c fault_handler.c coherence.c worker.c
OBJS = $(SRCS:.c=.o)
HDRS = dsm.h memory.h network.h fault_handler.h coherence.h worker.h

# Output binary
TARGET = dsm

# Platform detection
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    # Linux: Full static linking possible
    LDFLAGS = -static -pthread
    CFLAGS += -fno-pie
else ifeq ($(UNAME_S),Darwin)
    # macOS: Static linking not fully supported
    # Use -fno-pie and disable ASLR at runtime instead
    LDFLAGS = -pthread
    # Note: On macOS, use 'arch -arch x86_64' or set DYLD_NO_PIE=1
    # Or run with: arch -x86_64 ./dsm (on Apple Silicon)
    CFLAGS += -fno-pie
    # macOS specific: disable position independent executable
    LDFLAGS += -Wl,-no_pie
endif

.PHONY: all clean run-server run-client test help

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)
	@echo "Built $(TARGET) successfully"
ifeq ($(UNAME_S),Darwin)
	@echo "Note: On macOS, ASLR may affect memory mapping."
	@echo "Run with: DYLD_NO_PIE=1 ./dsm ..."
endif

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	rm -f /tmp/dsm_region_*.dat

# Example run commands
run-server:
	./$(TARGET) --server --port 9000 --clients 1 -v

run-client:
	./$(TARGET) --client --host 127.0.0.1 --port 9000 -v

# Run a quick local test with 2 clients
test:
	@echo "Starting test with 1 server and 2 clients..."
	@echo "Terminal 1: make run-server2"
	@echo "Terminal 2: make run-client"  
	@echo "Terminal 3: make run-client"

run-server2:
	./$(TARGET) --server --port 9000 --clients 2 -v

help:
	@echo "DSM - Distributed Shared Memory Demo"
	@echo ""
	@echo "Targets:"
	@echo "  all          Build the dsm binary"
	@echo "  clean        Remove build artifacts"
	@echo "  run-server   Run as server (1 client)"
	@echo "  run-server2  Run as server (2 clients)"
	@echo "  run-client   Run as client"
	@echo "  help         Show this help"
	@echo ""
	@echo "Quick test:"
	@echo "  Terminal 1: make run-server"
	@echo "  Terminal 2: make run-client"
