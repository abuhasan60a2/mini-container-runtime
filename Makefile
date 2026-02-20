# Minibox - Minimal Container Runtime
# Build system

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -g
LDFLAGS = -ljansson

# Source files
SRCDIR = src
SRCS = $(SRCDIR)/main.c \
       $(SRCDIR)/container.c \
       $(SRCDIR)/namespace.c \
       $(SRCDIR)/cgroup.c \
       $(SRCDIR)/network.c \
       $(SRCDIR)/rootfs.c \
       $(SRCDIR)/utils.c

OBJS = $(SRCS:.c=.o)
TARGET = minibox

# Default target
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Install to /usr/local/bin
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "Installed $(TARGET) to /usr/local/bin/"

# Uninstall
uninstall:
	rm -f /usr/local/bin/$(TARGET)
	@echo "Uninstalled $(TARGET)"

# Run tests
test: $(TARGET)
	@echo "Running tests..."
	./examples/test.sh

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Cleaned build artifacts"

# Debug build
debug: CFLAGS += -O0 -DDEBUG
debug: all

# Release build
release: CFLAGS = -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -O2
release: all

.PHONY: all clean install uninstall test debug release
