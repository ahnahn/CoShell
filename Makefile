# Makefile

CC      = gcc
CFLAGS  = -Wall -O2 -std=c11
LIBS    = -lncurses -lpthread

TARGET  = coshell
SRC     = coshell.c

.PHONY: all setup install clean

all: setup $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin

setup:
	@echo "Installing dependencies..."
	@sudo apt install -y libncurses-dev qrencode

clean:
	rm -f $(TARGET)
