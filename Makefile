CC = gcc
CFLAGS = -Wall -O2 -std=c11
LIBS = -lncurses -lpthread

TARGET = coshell
SRC = coshell.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

run: $(TARGET)
	./$(TARGET)

setup:
	sudo apt update && sudo apt install -y build-essential libncurses5-dev libncursesw5-dev qrencode

clean:
	rm -f $(TARGET)
