include ../common.mk

SUBDIRS := $(wildcard */.)
SUBCLEAN = $(addsuffix .clean, $(SUBDIRS))
MAKEFILE := $(shell pwd)/MakefileModule

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -f $(MAKEFILE) -C $@ DRIVER_TARGET=$(DRIVER_TARGET)

clean: $(SUBCLEAN)

$(SUBCLEAN): %.clean:
	$(MAKE) -f $(MAKEFILE) -C $* clean DRIVER_TARGET=$(DRIVER_TARGET)

clean: $(SUBCLEAN)

.PHONY: all $(SUBDIRS)
