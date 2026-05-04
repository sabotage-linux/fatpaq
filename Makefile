PROGS := makeapp launcher

PKG_CONFIG ?= pkg-config
FCFLAGS := $(shell $(PKG_CONFIG) --cflags squashfuse_ll fuse)
FLFLAGS := $(shell $(PKG_CONFIG) --libs squashfuse_ll fuse)

# requires squashfuse built with:
# --without-zlib --without-lzo --without-lz4 --without-xz
# to save on binary size and linker flags

CFLAGS ?= -Os -g0 -fdata-sections -ffunction-sections
LDFLAGS ?= -Wl,--gc-sections -Wl,-z,relro,-z,now -Wl,-z,text -s -static

-include config.mak

all: $(PROGS)

makeapp: makeapp.c
	gcc $^ -O0 -o $@

launcher: launcher.c
	gcc $^ -o $@ $(FCFLAGS) $(CFLAGS) $(FLFLAGS) $(LDFLAGS) -lzstd -lm -lpthread

pack: launcher
	upx -9 --ultra-brute launcher

clean:
	rm -f launcher makeapp

.PHONY: clean pack
