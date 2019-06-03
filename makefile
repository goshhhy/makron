CFLAGS ?= -Wall -g
# xcb headers
CFLAGS != pkg-config --cflags xcb
CFLAGS += -L../sulfur/ -lsulfur
CFLAGS += -I../sulfur/include/
OUT := makron

LIBS != pkg-config --libs xcb

all: $(OUT)

$(OUT): src/*.c
	$(CC) -o $(OUT) src/*.c $(LIBS) $(CFLAGS)

clean:
	rm $(OUT)
