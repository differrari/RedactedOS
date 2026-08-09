include ../common.mk

SUBDIRS := $(wildcard */.)
SUBCLEAN = $(addsuffix .clean,$(SUBDIRS))
SUBDUMP = $(addsuffix .dump,$(SUBDIRS))
MAKEFILE := $(shell pwd)/LibMakefile

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -f $(MAKEFILE) -C $@

clean: $(SUBCLEAN)

$(SUBCLEAN): %.clean:
		$(MAKE) -f $(MAKEFILE) -C $* clean

clean: $(SUBCLEAN)

.PHONY: all $(SUBDIRS)