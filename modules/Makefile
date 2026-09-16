include ../common.mk

SUBDIRS := $(wildcard */)
SUBCLEAN := $(addsuffix .clean, $(SUBDIRS))
MAKEFILE := $(CURDIR)/MakefileModule

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -f $(MAKEFILE) -C $@ DRIVER_TARGET=$(DRIVER_TARGET)

clean: $(SUBCLEAN)

$(SUBCLEAN): %.clean:
	$(MAKE) -f $(MAKEFILE) -C $* clean DRIVER_TARGET=$(DRIVER_TARGET)

.PHONY: all clean $(SUBDIRS) $(SUBCLEAN)
