KBUILD_CFLAGS   += -w
MODULE_NAME = max
obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-y		:= dir.o file.o inode.o namei.o hash.o super.o inline.o
$(MODULE_NAME)-y		+= checkpoint.o gc.o data.o node.o segment.o recovery.o max_init.o rps.o
$(MODULE_NAME)-$(CONFIG_F2FS_STAT_FS) += debug.o
$(MODULE_NAME)-$(CONFIG_F2FS_FS_XATTR) += xattr.o
$(MODULE_NAME)-$(CONFIG_F2FS_FS_POSIX_ACL) += acl.o
$(MODULE_NAME)-$(CONFIG_F2FS_IO_TRACE) += trace.o
$(MODULE_NAME)-$(CONFIG_F2FS_FS_ENCRYPTION) += crypto_policy.o crypto.o \
		crypto_key.o crypto_fname.o
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
