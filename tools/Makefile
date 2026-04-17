include ../common.mk

SUBDIRS := $(wildcard */.)
SUBCLEAN = $(addsuffix .clean,$(SUBDIRS))
SUBDUMP = $(addsuffix .dump,$(SUBDIRS))
MAKEFILE := $(shell pwd)/ToolMakefile

all: $(SUBDIRS)

dump: $(SUBDUMP)
	
$(SUBDIRS):
	$(MAKE) -f $(MAKEFILE) -C $@

clean: $(SUBCLEAN)

$(SUBCLEAN): %.clean:
	$(MAKE) -f $(MAKEFILE) -C $* clean

$(SUBDUMP): %.dump:
	$(MAKE) -f $(MAKEFILE) -C $* dump

clean: $(SUBCLEAN)

.PHONY: all $(SUBDIRS)
