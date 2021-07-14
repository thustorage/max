BASE47 := db-4.7.25.NC
DB47_DIR := $(shell pwd)/$(BASE47)
MY_DIR := $(shell pwd)

all: db4.7-mod

clean:
	rm -rf build db4.7-mod


.PHONY: unpack4.7
unpack4.7: $(BASE47)

$(BASE47):
	tar xzf $(BASE47).tar.gz

patch4.7: $(BASE47)/.stamp-patch

$(BASE47)/.stamp-patch: | $(BASE47)
	patch -p0 -d$(BASE47) < patch.4.7.25.1
	patch -p0 -d$(BASE47) < patch.4.7.25.2
	patch -p0 -d$(BASE47) < patch.4.7.25.3
	patch -p0 -d$(BASE47) < patch.4.7.25.4
	patch -p1 -d$(BASE47) < patch.4.7.25.mosbench
	touch $@

.PHONY: setup4.7
setup4.7: $(BASE47)/.stamp-patch
	if [ ! -e build/db4.7-mod/Makefile ]; then \
		mkdir -p build/db4.7-mod && \
		cd build/db4.7-mod && \
		$(DB47_DIR)/dist/configure --prefix=$(MY_DIR)/db4.7-mod CFLAGS='-DMOSBENCH_CACHE_NPROC -DMOSBENCH_FORCE_MULTICORE'; \
	fi

.PHONY: db4.7-mod
db4.7-mod: setup4.7
	$(MAKE) -C build/db4.7-mod
	$(MAKE) -C build/db4.7-mod install
