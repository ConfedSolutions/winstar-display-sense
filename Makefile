DESTDIR=
BINDIR=/usr/bin

all: draw-image

draw-image: draw-image.c
	$(CC) -O3 -o draw-image draw-image.c -lgpiod -lm

clean:
	-rm draw-image

install: draw-image:
	install -m 755 $< $(DESTDIR)/$(BINDIR)

.PHONY: all clean install
