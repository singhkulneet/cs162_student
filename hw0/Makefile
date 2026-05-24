CC=i386-gcc
CFLAGS=-g
BINARIES=map limits

default:
	$(MAKE) map
	$(MAKE) limits

map: map.c recurse.c
	$(CC) $(CFLAGS) $^ -o $@

limits: limits.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f $(BINARIES)
