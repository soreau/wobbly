CC=gcc
CFLAGS=-c -Wall
LIBS=-lm -lGLESv2 -lpng12 -lpthread -lX11 -lEGL
EXE=wobbly

all: wobbly

wobbly: main.o wobbly.o image-loader.o
	$(CC) main.o wobbly.o image-loader.o -o $(EXE) $(LIBS)

main.o: main.c
	$(CC) $(CFLAGS) main.c

wobbly.o: wobbly.c
	$(CC) $(CFLAGS) wobbly.c

image-loader.o: image-loader.c
	$(CC) $(CFLAGS) image-loader.c

clean:
	rm *o wobbly
