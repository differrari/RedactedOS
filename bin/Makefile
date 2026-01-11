include ../common.mk

SUBDIRS := $(wildcard */.)
SUBCLEAN = $(addsuffix .clean,$(SUBDIRS))
MAKEFILE := $(shell pwd)/BinMakefile

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -f $(MAKEFILE) -C $@

clean: $(SUBCLEAN)

$(SUBCLEAN): %.clean:
	$(MAKE) -f $(MAKEFILE) -C $* clean

clean: $(SUBCLEAN)

.PHONY: all $(SUBDIRS)
