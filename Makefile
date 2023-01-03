.POSIX:

TARGETS = fmt2023

CFLAGS = -D NDEBUG -O
LDFLAGS = -s

.PHONY: all clean

all: $(TARGETS)

clean:
	-rm $(TARGETS)
