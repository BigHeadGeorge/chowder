CC=cc
CFLAGS=-g -Wall -Wextra -Werror -pedantic
LIBS=-lz -lm
TARGET=tests
SOURCES=*.c ../region.c ../nbt.c ../blocks.c ../section.c

$(TARGET):
	$(CC) $(CFLAGS) $(SOURCES) $(LIBS) -o $@
