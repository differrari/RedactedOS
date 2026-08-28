include ../common.mk

SUBDIRS := $(wildcard */)
SUBCLEAN := $(addsuffix .clean,$(SUBDIRS))
SUBDUMP := $(addsuffix .dump,$(SUBDIRS))
MAKEFILE := $(CURDIR)/LibMakefile

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -f $(MAKEFILE) -C $@

clean: $(SUBCLEAN)

$(SUBCLEAN): %.clean:
		$(MAKE) -f $(MAKEFILE) -C $* clean

.PHONY: all clean $(SUBDIRS) $(SUBCLEAN)