.PHONY: all
all: lightplay

lightplay: main.c
	cc -o $@ main.c -lsndio -lm

.PHONY: clean
clean:
	rm -f lightplay *.d *.o
