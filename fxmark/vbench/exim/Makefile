-include ../config.mk
SUPPORT = ../support

# Quiet compile, unset for verbose output
Q	= @
OS 	= $(shell uname)

CC	= gcc

CWARNS	= -Wformat=2 -Wextra -Wmissing-noreturn -Wwrite-strings -Wshadow \
	  -Wno-unused-parameter -Wmissing-format-attribute -fno-builtin \
	  -Wswitch-default -Wmissing-prototypes \
	  -Wmissing-declarations
CFLAGS  = -D_GNU_SOURCE -Wall -Werror -fno-strict-aliasing $(CWARNS) \
	  -g -O3 -I..

# Build Exim with our tweaked libdb
DBMDIR  = $(abspath ../libdb/db4.7-mod)
#DBMDIR  = $(abspath ../libdb/db4.6-mod)
#DBMDIR = /home/rtm/bdb

# Build Exim with MOSBENCH modifications
# Unused options: -DMOSBENCH_DELIVER_SAME_CORE -DMOSBENCH_NO_J_FILE
# -DMOSBENCH_CPU_BIND
EXIM_DEFINES = \
	-DMOSBENCH_FAKE_LOADAVG -DMOSBENCH_WAIT_DELIVERY \
	-DMOSBENCH_NO_TZ_LOCK \
	-DMOSBENCH_SPLIT_BY_PID -DMOSBENCH_CPU_BIND \
	-DMOSBENCH_DELIVER_INLINE

ifeq ($(OS), Linux)
LIBS = 
endif
ifeq ($(OS), SunOS)
LIBS = -lsocket -lnsl
endif

all: smtpbm exim-mod

smtpbm: smtpbm.c $(SUPPORT)/mtrace.h
	@echo "CC	$@"
	$(Q)$(CC) $(CFLAGS) -o $@ $< $(LIBS)
clean:
	@echo "CLEAN"
	$(Q)rm -rf smtpbm


.PHONY: exim-mod
exim-mod:
	mkdir -p exim-4.71/Local
	sed -e "s,@INSTALL_DIR@,`pwd`/$@,;s,@USER@,$$USER,;s,@DBMDIR@,$(DBMDIR),;s,@DEFINES@,$(EXIM_DEFINES)," \
	  < Local-Makefile.tmpl > exim-4.71/Local/Makefile.tmp
	if ! cmp exim-4.71/Local/{Makefile,Makefile.tmp}; then \
	  $(MAKE) -C exim-4.71 distclean; \
	  mv exim-4.71/Local/Makefile.tmp exim-4.71/Local/Makefile; \
	fi
	$(MAKE) -C exim-4.71
	$(MAKE) -C exim-4.71 install INSTALL_ARG=-no_chown

.PHONY: exim-clean
exim-clean:
	$(MAKE) -C exim-4.71 distclean
	rm -rf exim-mod exim-4.71/Local
