BUILD_DIR ?= build
CXX ?= g++
CC ?= gcc
CPPFLAGS += -Isrc -Ivendor -Iassets -D_GNU_SOURCE
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wpedantic
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra
LDLIBS += -l:libwayland-client.so.0
GTK_CPPFLAGS ?= $(shell pkg-config --cflags gobject-2.0)
WITH_QTWEBKIT ?= 1
export WITH_QTWEBKIT
QT_CPPFLAGS ?= $(shell pkg-config --cflags Qt5WebKitWidgets)
QT_LDLIBS ?= $(shell pkg-config --libs Qt5WebKitWidgets)
SOURCES := $(wildcard src/*/*.cpp) src/main.cpp
C_SOURCES := $(wildcard vendor/*.c)
TEST_SOURCES := $(wildcard tests/*.cpp)
OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES)) $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
TEST_OBJECTS := $(patsubst tests/%.cpp,$(BUILD_DIR)/test/%.o,$(TEST_SOURCES))
QT_OBJECT := $(BUILD_DIR)/engines/qtwebkit/main.o
.PHONY: all test test-control test-guards test-screen test-wayland test-display-startup cross deb check check-package
all: $(BUILD_DIR)/zero-browser $(BUILD_DIR)/libzero-netsurf-gtk.so
ifeq ($(WITH_QTWEBKIT),1)
all: $(BUILD_DIR)/zero-qtwebkit
endif
$(BUILD_DIR)/zero-qtwebkit: $(QT_OBJECT)
	$(CXX) $(QT_LDFLAGS) $(LDFLAGS) $^ $(QT_LDLIBS) -o "$@"
$(QT_OBJECT): engines/qtwebkit/main.cpp Makefile
	@mkdir -p "$(@D)"
	@test -n "$(QT_LDLIBS)" || { echo "Install qtbase5-dev and libqt5webkit5-dev (QtWebKit, not QtWebEngine)"; exit 1; }
	$(CXX) $(CPPFLAGS) $(QT_CPPFLAGS) $(CXXFLAGS) -fPIC -MMD -MP -c "$<" -o "$@"
$(BUILD_DIR)/libzero-netsurf-gtk.so: modules/netsurf-gtk.c modules/gtk-api.h src/Browser/GtkEscapeProtocol.h Makefile
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(GTK_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -fPIC -shared -MMD -MP -MF "$@.d" -MT "$@" modules/netsurf-gtk.c -o "$@"
$(BUILD_DIR)/zero-browser: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o "$@"
$(BUILD_DIR)/src/main.o: src/main.cpp VERSION Makefile
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DZERO_BROWSER_VERSION='"$(shell cat VERSION)"' -MMD -MP -c "$<" -o "$@"
$(BUILD_DIR)/%.o: %.cpp Makefile
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c "$<" -o "$@"
$(BUILD_DIR)/%.o: %.c Makefile
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"
$(BUILD_DIR)/test/%.o: tests/%.cpp Makefile
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c "$<" -o "$@"
$(BUILD_DIR)/tests: $(TEST_OBJECTS) $(filter-out $(BUILD_DIR)/src/main.o,$(OBJECTS))
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o "$@"
test: $(BUILD_DIR)/tests
	"$(BUILD_DIR)/tests"
$(BUILD_DIR)/test/control-io.so: tests/helpers/control-io.c Makefile
	@mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -fPIC -shared "$<" -o "$@"
test-control: $(BUILD_DIR)/zero-browser $(BUILD_DIR)/test/control-io.so
	python3 tests/check-control.py "$(BUILD_DIR)/zero-browser" --shim "$(BUILD_DIR)/test/control-io.so"
$(BUILD_DIR)/test/guard-proc.so: tests/helpers/guard-proc.c Makefile
	@mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -fPIC -shared "$<" -ldl -o "$@"
$(BUILD_DIR)/test/qt-guard-driver.o: tests/helpers/qt-guard-driver.cpp Makefile
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c "$<" -o "$@"
$(BUILD_DIR)/test/qt-guard-driver: $(BUILD_DIR)/test/qt-guard-driver.o $(filter-out $(BUILD_DIR)/src/main.o,$(OBJECTS))
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o "$@"
test-guards: $(BUILD_DIR)/test/qt-guard-driver $(BUILD_DIR)/test/guard-proc.so
	python3 tests/check-qt-guards.py "$(BUILD_DIR)/test/qt-guard-driver" --shim "$(BUILD_DIR)/test/guard-proc.so"
test-screen: $(BUILD_DIR)/zero-browser
	python3 tests/check-screen-idle.py "$(BUILD_DIR)/zero-browser"
test-wayland: $(BUILD_DIR)/zero-browser
	python3 tests/check-recovery.py "$(BUILD_DIR)/zero-browser"
test-display-startup: test-wayland

cross:
	python3 scripts/build-arm64.py
deb: cross
	python3 packaging/build-deb.py
check: test check-package
check-package:
	python3 tests/check-package.py
-include $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(QT_OBJECT:.o=.d) $(BUILD_DIR)/libzero-netsurf-gtk.so.d
-include $(BUILD_DIR)/test/qt-guard-driver.d
