include ../common.mk

SUBDIRS := $(wildcard */.)
SUBCLEAN = $(addsuffix .clean,$(SUBDIRS))

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean: $(SUBCLEAN)

$(SUBCLEAN): %.clean:
		$(MAKE) -C $* clean

clean: $(SUBCLEAN)

.PHONY: all $(SUBDIRS)
