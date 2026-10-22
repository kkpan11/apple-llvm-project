# Explicit module builds fail to find the libstdc++ module map.
ifeq "$(OS)" "Linux"
SWIFT_ENABLE_EXPLICIT_MODULES := NO
endif
SWIFT_SOURCES := main.swift
SWIFT_CXX_INTEROP := 1
SWIFTFLAGS_EXTRAS = -Xcc -I$(SRCDIR) -Xlinker -lm
include Makefile.rules
