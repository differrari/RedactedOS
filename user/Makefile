include ../common.mk

USER_DIRS := $(sort $(dir $(wildcard */*.S */*.c */*.cpp)))
SUBDIRS := $(filter-out build/ resources/ %.red/,$(USER_DIRS))
SUBCLEAN := $(addsuffix .clean,$(SUBDIRS))
SUBDUMP := $(addsuffix .dump,$(SUBDIRS))
MAKEFILE := $(shell pwd)/UserMakefile

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -f $(MAKEFILE) -C $@

clean: $(SUBCLEAN)
	$(RM) -r build *.red dump

dump: $(SUBDUMP)

$(SUBCLEAN): %.clean:
		$(MAKE) -f $(MAKEFILE) -C $* clean
		
$(SUBDUMP): %.dump:
	$(MAKE) -f $(MAKEFILE) -C $* dump

.PHONY: all clean dump $(SUBDIRS) $(SUBCLEAN) $(SUBDUMP)
