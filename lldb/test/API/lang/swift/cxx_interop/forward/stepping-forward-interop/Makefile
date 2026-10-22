# Explicit module builds fail to find the libstdc++ module map.
ifeq "$(OS)" "Linux"
SWIFT_ENABLE_EXPLICIT_MODULES := NO
endif
SWIFT_SOURCES := main.swift
CXX_SOURCES := impl.cpp
SWIFT_CXX_INTEROP := 1
SWIFTFLAGS_EXTRAS = -Xcc -I$(SRCDIR)
include Makefile.rules 
