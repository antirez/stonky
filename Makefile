all: stonky

stonky: stonky.c canvas.c cJSON.c sds.c sds.h canvas.h
	$(CC) -g -ggdb -O2 -Wall -W -std=c11 \
		canvas.c cJSON.c sds.c stonky.c \
		-o stonky -lpthread -lcurl -lsqlite3

clean:
	rm -f stonky
