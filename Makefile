all: draw-image

draw-image: draw-image.c
	$(CC) -O3 -o draw-image draw-image.c -lgpiod -lm

clean:
	-rm draw-image

install:
	install -m 755 draw-image /usr/bin/draw-image

.PHONY: all clean install
