include ../common.mk

SUBDIRS := $(sort $(dir $(wildcard */*.S */*.c */*.cpp)))
SUBCLEAN := $(addsuffix .clean,$(wildcard */))
SUBDUMP := $(addsuffix .dump,$(SUBDIRS))
MAKEFILE := $(CURDIR)/ToolMakefile

all: $(SUBDIRS)

dump: $(SUBDUMP)
	
$(SUBDIRS):
	$(MAKE) -f $(MAKEFILE) -C $@

clean: $(SUBCLEAN)

$(SUBCLEAN): %.clean:
	$(MAKE) -f $(MAKEFILE) -C $* clean

$(SUBDUMP): %.dump:
	$(MAKE) -f $(MAKEFILE) -C $* dump

.PHONY: all clean dump $(SUBDIRS) $(SUBCLEAN) $(SUBDUMP)
