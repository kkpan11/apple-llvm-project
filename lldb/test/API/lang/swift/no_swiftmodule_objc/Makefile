# Debugging works here without any .swiftmodule: HIDE_SWIFTMODULE keeps one
# from being left next to the binary or embedded in it, so every type below has
# to be recovered from DWARF and the Objective-C runtime.
SWIFT_SOURCES := main.swift
OBJC_SOURCES := ObjCStuff.m
HIDE_SWIFTMODULE := YES
SWIFT_ENABLE_EXPLICIT_MODULES := NO
MAKE_GMODULES := YES
SWIFT_OBJC_INTEROP = 1
# Leave no .swiftinterface next to the binary either, so there is nothing at
# all for the main module to be rebuilt from. Makefile.rules pairs the
# interface with -swift-version 5, which still has to be passed explicitly.
DISABLE_SWIFT_INTERFACE := YES
SWIFTFLAGS_EXTRAS = -I$(SRCDIR) -swift-version 5

include Makefile.rules
