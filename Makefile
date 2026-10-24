# Home Hider - PS5 payload
#
#   make            build HomeHider.elf with the ps5-payload-sdk
#   make host       build a desktop copy for testing (no PS5 needed)
#   make test       run the desktop test suite against a mock app.db
#   make clean

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST        ?= ps5
PS5_PORT        ?= 9021

ELF     := HomeHider.elf
BUILD   := build

# GitHub repository the updater checks by default (owner/name). It can also be
# set from the menu: Updates > GitHub repository.
HH_GITHUB_REPO ?=

SRCS    := src/main.c src/hider.c src/http.c src/strbuf.c src/updater.c src/json.c src/sha256.c \
           src/folders.c src/icons.c src/log.c src/usbpics.c src/launch.c src/custom.c src/texture.c
HEADERS := src/hider.h src/http.h src/strbuf.h src/platform.h src/updater.h src/json.h \
           src/sha256.h src/folders.h src/icons.h src/log.h src/usbpics.h src/launch.h src/custom.h src/texture.h third_party/stb/stb_image.h third_party/stb/stb_image_write.h src/webui_html.h src/folder_html.h
DEFS    := -DHH_DEFAULT_REPO='"$(HH_GITHUB_REPO)"'

WARN    := -Wall -Wextra -Wno-unused-parameter
SQLITE_DEFS := -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
               -DSQLITE_TEMP_STORE=3 -DSQLITE_DEFAULT_MEMSTATUS=0 \
               -DSQLITE_OMIT_DEPRECATED

all: $(ELF)

src/webui_html.h: web/index.html tools/embed.py
	python3 tools/embed.py $< $@ WEBUI_HTML

src/folder_html.h: web/folder.html tools/embed.py
	python3 tools/embed.py $< $@ FOLDER_HTML

# ---- PS5 build --------------------------------------------------------------
PS5_CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_STRIP := $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PS5_LIBS := -lSceSystemService -lSceUserService -lSceHttp2 -lSceSsl -lSceNet \
            -lSceAppInstUtil -lpthread

$(BUILD)/ps5/sqlite3.o: third_party/sqlite/sqlite3.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -Os $(SQLITE_DEFS) -c -o $@ $<

$(ELF): $(SRCS) src/platform_ps5.c $(HEADERS) $(BUILD)/ps5/sqlite3.o
	$(PS5_CC) -Os $(WARN) $(DEFS) -I. -o $@ $(SRCS) src/platform_ps5.c \
	    $(BUILD)/ps5/sqlite3.o $(PS5_LIBS)
	$(PS5_STRIP) $@

deploy: $(ELF)
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

# ---- desktop build (tests) --------------------------------------------------
HOST_CC ?= cc

$(BUILD)/host/sqlite3.o: third_party/sqlite/sqlite3.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O1 $(SQLITE_DEFS) -c -o $@ $<

host: $(BUILD)/host/homehider

$(BUILD)/host/homehider: $(SRCS) src/platform_host.c $(HEADERS) $(BUILD)/host/sqlite3.o
	$(HOST_CC) -g -O2 -DHH_HOST $(WARN) $(DEFS) -fsanitize=address,undefined -I. -o $@ \
	    $(SRCS) src/platform_host.c $(BUILD)/host/sqlite3.o -lm -lpthread

test: host
	python3 tests/test_host.py $(BUILD)/host/homehider

clean:
	rm -rf $(BUILD) $(ELF) src/webui_html.h src/folder_html.h

.PHONY: all host test deploy clean
