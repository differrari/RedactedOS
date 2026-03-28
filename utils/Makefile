include ../common.mk

SUBDIRS := $(wildcard */.)
SUBCLEAN = $(addsuffix .clean,$(SUBDIRS))
SUBDUMP = $(addsuffix .dump,$(SUBDIRS))

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean: $(SUBCLEAN)
	
dump: $(SUBDUMP)

$(SUBCLEAN): %.clean:
		$(MAKE) -C $* clean
		
$(SUBDUMP): %.dump:
	$(MAKE) -C $* dump

clean: $(SUBCLEAN)

.PHONY: all $(SUBDIRS)
