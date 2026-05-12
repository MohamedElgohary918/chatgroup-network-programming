# ─────────────────────────────────────────────────────────────────────────────
# Makefile — Assiut Messenger Client
#
# Usage:
#   make          → compile everything into ./client
#   make clean    → delete compiled files
# ─────────────────────────────────────────────────────────────────────────────

# Compiler and flags
CC     = gcc
CFLAGS = -Wall -Wextra -g $(shell pkg-config --cflags gtk+-3.0)
LIBS   = $(shell pkg-config --libs gtk+-3.0)

# All .c files in the project
SRCS = main.c \
       theme.c \
       ui_chat.c \
       ui_sidebar.c \
       ui_input.c \
       ui_login.c \
       network.c

# Object files (compiled from each .c)
OBJS = $(SRCS:.c=.o)

# Final binary name
TARGET = client

# Default target: build the client
all: $(TARGET)

# Link all object files into the final binary
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LIBS)
	@echo "✅ Built: ./$(TARGET)"

# Compile each .c file into a .o file
# $< = the .c file, $@ = the .o file
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Remove compiled files
clean:
	rm -f $(OBJS) $(TARGET)
	@echo "🗑  Cleaned"

.PHONY: all clean
