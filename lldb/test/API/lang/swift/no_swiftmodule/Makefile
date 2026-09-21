# The main module is built without a .swiftmodule, while the module it imports
# keeps one. Reconstructing the imported module's types from that lopsided
# setup is what used to crash.
SWIFT_SOURCES := main.swift
HIDE_SWIFTMODULE := YES
# Leave no .swiftinterface next to the binary either, so there is nothing at
# all for the main module to be rebuilt from. Makefile.rules pairs the
# interface with -swift-version 5, which still has to be passed explicitly.
DISABLE_SWIFT_INTERFACE := YES
SWIFTFLAGS_EXTRAS = -I$(BUILDDIR) -swift-version 5
LD_EXTRAS = NoSwiftmoduleHelper.o

all: a.out

include Makefile.rules

# main.swift imports the helper, so its module has to exist first, and the
# binary has to be linked against the helper's object file.
$(SWIFT_OBJECTS): NoSwiftmoduleHelper.swiftmodule
$(EXE): NoSwiftmoduleHelper.o

NoSwiftmoduleHelper.o NoSwiftmoduleHelper.swiftmodule: NoSwiftmoduleHelper.swift \
        $(call tool_prereq,$(SWIFTC_PATH)) $(call tool_prereq,$(SWIFT_FRONTEND_PATH))
	$(SWIFTC) $(SWIFTFLAGS) -c -parse-as-library \
	  -module-name NoSwiftmoduleHelper \
	  -emit-module -emit-module-path NoSwiftmoduleHelper.swiftmodule \
	  $< -o NoSwiftmoduleHelper.o
