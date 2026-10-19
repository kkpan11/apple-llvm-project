# Explicit module builds fail to find the libstdc++ module map.
ifeq "$(OS)" "Linux"
SWIFT_ENABLE_EXPLICIT_MODULES := NO
endif
SWIFT_CXX_HEADER := swift-types.h
SWIFT_SOURCES := swift-types.swift
CXX_SOURCES := main.cpp
SWIFT_CXX_INTEROP := 1
SWIFTFLAGS_EXTRAS = -Xcc -I$(SRCDIR) -parse-as-library 
SWIFT_CXX_HEADER_FLAGS = -clang-header-expose-decls=has-expose-attr
CFLAGS_EXTRAS := -I.
include Makefile.rules
