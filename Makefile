FINAL_LIBS=-lpthread -lcurl -lsqlite3

# Linux ARM needs -latomic at linking time
ifneq (,$(filter aarch64 armv,$(uname_M)))
        FINAL_LIBS+=-latomic
else
ifneq (,$(findstring armv,$(uname_M)))
        FINAL_LIBS+=-latomic
endif
endif

all: stonky

stonky: stonky.c canvas.c cJSON.c sds.c sds.h canvas.h
	$(CC) -g -ggdb -O2 -Wall -W -std=c11 \
		canvas.c cJSON.c sds.c stonky.c \
		-o stonky $(FINAL_LIBS)

clean:
	rm -f stonky
