XEN_ROOT=$(CURDIR)/../../../..
include $(XEN_ROOT)/tools/Rules.mk

CFLAGS += -Werror
CFLAGS += $(CFLAGS_libxenctrl)
CFLAGS += $(CFLAGS_libxenguest)
CFLAGS += $(CFLAGS_libxenstore) 
CFLAGS += $(CFLAGS_xeninclude) 

.PHONY: all
all: xen-mceinj

install: 
	$(INSTALL_PROG) xen-mceinj $(DESTDIR)$(SBINDIR)

.PHONY: clean
clean:
	$(RM) *.o xen-mceinj

xen-mceinj: xen-mceinj.o Makefile
	$(CC) -o $@ $< $(LDFLAGS) $(LDLIBS_libxenctrl) $(LDLIBS_libxenguest) $(LDLIBS_libxenstore)
