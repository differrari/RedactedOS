include ../common.mk

SUBDIRS := $(wildcard */.)
SUBCLEAN = $(addsuffix .clean,$(SUBDIRS))
SUBDUMP = $(addsuffix .dump,$(SUBDIRS))
MAKEFILE := $(shell pwd)/UserMakefile

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -f $(MAKEFILE) -C $@

clean: $(SUBCLEAN)
	
dump: $(SUBDUMP)

$(SUBCLEAN): %.clean:
		$(MAKE) -f $(MAKEFILE) -C $* clean
		
$(SUBDUMP): %.dump:
	$(MAKE) -f $(MAKEFILE) -C $* dump

clean: $(SUBCLEAN)

.PHONY: all $(SUBDIRS)