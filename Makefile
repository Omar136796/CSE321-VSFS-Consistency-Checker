CC = gcc
CFLAGS = -Wall
TARGET = vsfsck

$(TARGET): vsfsck.c
	$(CC) $(CFLAGS) -o $(TARGET) vsfsck.c

run: $(TARGET)
	./$(TARGET) vsfs.img || true

clean:
	rm -f $(TARGET)

.PHONY: run clean
