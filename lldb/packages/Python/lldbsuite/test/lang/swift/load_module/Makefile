SWIFT_SOURCES := main.swift
# In bottom-up order, because we didn't capture the dependencies.
BREADTH := 4
ALL_LIBS := \
  $(patsubst %,d%,$(shell seq 1 $(BREADTH))) \
  $(patsubst %,c%,$(shell seq 1 $(BREADTH))) \
  $(patsubst %,b%,$(shell seq 1 $(BREADTH))) \
  $(patsubst %,a%,$(shell seq 1 $(BREADTH))) 

SWIFTFLAGS_EXTRAS = -I$(BUILDDIR) -autolink-force-load
LD_EXTRAS = -L$(BUILDDIR) $(patsubst %, -l%, $(patsubst %,a%,$(shell seq 1 $(BREADTH))))

all: $(patsubst %, lib%.dylib, $(ALL_LIBS)) $(EXE)

include Makefile.rules

lib%.dylib: %.swift
	$(MAKE) MAKE_DSYM=$(MAKE_DSYM) CC="$(CC)" SWIFTC="$(SWIFTC)" \
		ARCH=$(ARCH) DSYMUTIL="$(DSYMUTIL)" VPATH="$(BUILDDIR)"  \
		SWIFTSDKROOT=$(SWIFTSDKROOT) \
		SWIFT_MODULE_CACHE_FLAGS="$(SWIFT_MODULE_CACHE_FLAGS)" \
		SWIFTFLAGS_EXTRAS="$(SWIFTFLAGS_EXTRAS) -module-link-name $(shell basename $< .swift)" \
		LD_EXTRAS="-L$(BUILDDIR)" \
		BASENAME=$(shell basename $< .swift) \
		-f $(SRCDIR)/dylib.mk

