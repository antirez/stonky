all: stonky

stonky: stonky.c canvas.c cJSON.c sds.c sds.h canvas.h \
	kann.c kann.h kautodiff.c kautodiff.h
	$(CC) -g -ggdb -O2 -Wall -W -std=c99 \
		canvas.c cJSON.c sds.c stonky.c \
		kann.c kautodiff.c \
		-o stonky -lpthread -lcurl -lsqlite3

clean:
	rm -f stonky
