# Linux build. Needs only gcc, make, pkg-config and GTK3, which Omarchy ships.
#   make                       build
#   make test                  run the tests (needs a running desktop or Xvfb)
#   make install               install to ~/.local
#   make install PREFIX=/usr DESTDIR=...    for packaging
PREFIX ?= $(HOME)/.local
BUILD ?= build-linux
CXX ?= g++
CC ?= gcc
PKGS := gtk+-3.0 gmodule-2.0 gio-2.0
SCI := third_party/scintilla
LEX := third_party/lexilla

CPPFLAGS += -DGTK -DNDEBUG -I$(SCI)/include -I$(SCI)/src -I$(SCI)/gtk -I$(LEX)/include -I$(LEX)/lexlib \
            -Icore -Ilinux -Ithird_party/json -Ithird_party/pugixml -Ithird_party/pcre2 $(shell pkg-config --cflags $(PKGS))
OPT ?= -O2
CXXFLAGS += -std=c++17 $(OPT) -pthread -MMD -MP
CFLAGS += $(OPT) -MMD -MP
LDLIBS += $(shell pkg-config --libs $(PKGS)) -pthread

ENGINE_SRC := $(wildcard $(SCI)/src/*.cxx) $(wildcard $(SCI)/gtk/*.cxx) \
              $(filter-out %/LexUser.cxx,$(wildcard $(LEX)/lexers/*.cxx)) $(wildcard $(LEX)/lexlib/*.cxx) $(wildcard $(LEX)/src/*.cxx)
ENGINE_OBJ := $(ENGINE_SRC:%=$(BUILD)/%.o) $(BUILD)/$(SCI)/gtk/scintilla-marshal.c.o
CORE_SRC := core/TextCore.cpp core/Languages.cpp core/Regex.cpp core/Completion.cpp core/FunctionList.cpp core/Workspace.cpp third_party/pugixml/pugixml.cpp
PCRE2_SRC := $(wildcard third_party/pcre2/*.c)
CORE_OBJ := $(CORE_SRC:%=$(BUILD)/%.o) $(PCRE2_SRC:%=$(BUILD)/%.o)
OWN_FLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wno-unknown-pragmas -Wno-missing-field-initializers
LIB_FLAGS := -w

.PHONY: all test install uninstall clean
all: $(BUILD)/omapad

$(BUILD)/omapad: $(BUILD)/linux/main.cpp.o $(CORE_OBJ) $(ENGINE_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/omapad_tests: $(BUILD)/tests/core_tests.cpp.o $(BUILD)/tests/TestViews_linux.cpp.o $(CORE_OBJ) $(ENGINE_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/tests/core_tests.cpp.o: CPPFLAGS += -Itests -DOMAPAD_RESOURCES=\"$(CURDIR)/resources\"
$(BUILD)/tests/TestViews_linux.cpp.o: CPPFLAGS += -Itests
$(BUILD)/third_party/pcre2/%.c.o: CPPFLAGS += -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 -DPCRE2_STATIC -DSUPPORT_UNICODE

$(BUILD)/core/%.cpp.o: core/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(OWN_FLAGS) -c $< -o $@
$(BUILD)/linux/%.cpp.o: linux/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(OWN_FLAGS) -c $< -o $@
$(BUILD)/tests/%.cpp.o: tests/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(OWN_FLAGS) -c $< -o $@
$(BUILD)/third_party/%.cxx.o: third_party/%.cxx
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LIB_FLAGS) -c $< -o $@
$(BUILD)/third_party/%.cpp.o: third_party/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LIB_FLAGS) -c $< -o $@
$(BUILD)/third_party/%.c.o: third_party/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LIB_FLAGS) -c $< -o $@

test: $(BUILD)/omapad_tests
	$(BUILD)/omapad_tests

install: $(BUILD)/omapad
	install -Dm755 $(BUILD)/omapad $(DESTDIR)$(PREFIX)/bin/omapad
	install -d $(DESTDIR)$(PREFIX)/share/omapad/themes $(DESTDIR)$(PREFIX)/share/omapad/licenses \
	           $(DESTDIR)$(PREFIX)/share/omapad/autoCompletion $(DESTDIR)$(PREFIX)/share/omapad/functionList
	install -m644 resources/langs.model.xml resources/stylers.model.xml LICENSE NOTICE $(DESTDIR)$(PREFIX)/share/omapad/
	install -m644 resources/themes/*.xml $(DESTDIR)$(PREFIX)/share/omapad/themes/
	install -m644 licenses/*.txt $(DESTDIR)$(PREFIX)/share/omapad/licenses/
	install -m644 resources/autoCompletion/*.xml $(DESTDIR)$(PREFIX)/share/omapad/autoCompletion/
	install -m644 resources/functionList/*.xml $(DESTDIR)$(PREFIX)/share/omapad/functionList/
	install -Dm644 resources/omapad.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/omapad.svg
	sed 's|@BINDIR@|$(PREFIX)/bin|' linux/omapad.desktop.in > $(BUILD)/omapad.desktop
	install -Dm644 $(BUILD)/omapad.desktop $(DESTDIR)$(PREFIX)/share/applications/omapad.desktop
	-update-desktop-database $(DESTDIR)$(PREFIX)/share/applications 2>/dev/null
	-gtk-update-icon-cache -q -t $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/omapad $(DESTDIR)$(PREFIX)/share/applications/omapad.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/omapad.svg
	rm -rf $(DESTDIR)$(PREFIX)/share/omapad

clean:
	rm -rf $(BUILD)

# Rebuild consumers when their headers change.
-include $(ENGINE_OBJ:.o=.d) $(CORE_OBJ:.o=.d) $(BUILD)/linux/main.cpp.d $(BUILD)/tests/core_tests.cpp.d $(BUILD)/tests/TestViews_linux.cpp.d
