CC      ?= cc
CFLAGS  ?= -O2
LDFLAGS ?=
PREFIX  ?= $(HOME)/.local

# Kept out of CFLAGS so that `make CFLAGS=-g` cannot drop the warning set.
# -Wcast-qual and -Wshadow are worth an occasional manual run but stay out of
# the default set: they only fire on the vendored agent headers and on the
# const cast that execvp's argv signature forces.
WARNINGS := -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes \
            -Wpointer-arith -Wundef -Wformat=2 -Wno-format-nonliteral
ALL_CFLAGS := -std=gnu11 $(WARNINGS) $(CFLAGS) -Isrc -Isrc/vendor

BIN     := mux
BUILD   := build

TOOL_NAMES := $(patsubst tools/%.c,%,$(wildcard tools/*.c))
CHECKS := overlaytest viewporttest imagerowtest chrometest imagefittest mdtest \
          reflowtest toolstyletest sessionlisttest claudetest codextest \
          groktest filedifftest pitest agenttabstest statustest transcripttest \
          sessionviewtest sessionloadtest highlighttest muxcfgtest telegramtest \
          boardtest modelstest boardgridtest boardtiletest viewstest sessionpresenttest \
          workspacetest replboxtest ttytest gitinfotest sidechannelviewtest \
          sidechannelcmdtest taskstest
MANUAL_TOOLS := imagetest keydump palette pastetest spintest

# Make every harness declare whether it is safe for an unattended check. This
# deliberately fails at parse time when a new tools/*.c has not been classified.
ifneq ($(strip $(filter $(CHECKS),$(MANUAL_TOOLS))),)
$(error a tool cannot appear in both CHECKS and MANUAL_TOOLS)
endif
ifneq ($(sort $(TOOL_NAMES)),$(sort $(CHECKS) $(MANUAL_TOOLS)))
$(error tools/*.c must appear in exactly one of CHECKS or MANUAL_TOOLS)
endif

CHECK_BINS  := $(addprefix $(BUILD)/,$(CHECKS))
MANUAL_BINS := $(addprefix $(BUILD)/,$(MANUAL_TOOLS))
TOOLS       := $(CHECK_BINS) $(MANUAL_BINS)
SRC     := $(wildcard src/*.c) $(wildcard src/vendor/*.c)
OBJ     := $(SRC:.c=.o)
DEP     := $(OBJ:.o=.d)

all: $(BIN)

# forkpty lives in libutil outside the BSDs.
LIBS += -pthread -lcurl
ifneq ($(shell uname -s),Darwin)
LIBS += -lutil
endif

# libjpeg-turbo decodes a jpeg straight out of the DCT at 1/2, 1/4 or 1/8
# scale, so drawing a camera photo costs a fraction of its pixels. Optional:
# without it stb_image decodes every jpeg at full resolution. The static
# archive is preferred so the binary carries no brew dylib with it.
ifndef JPEG_PREFIX
JPEG_PREFIX := $(shell pkg-config --variable=prefix libjpeg 2>/dev/null || \
                       brew --prefix jpeg-turbo 2>/dev/null)
endif
ifneq ($(wildcard $(JPEG_PREFIX)/include/jpeglib.h),)
  ALL_CFLAGS += -DPIX_HAVE_JPEG -I$(JPEG_PREFIX)/include
  ifneq ($(wildcard $(JPEG_PREFIX)/lib/libjpeg.a),)
    JPEG_LIBS := $(JPEG_PREFIX)/lib/libjpeg.a
  else
    JPEG_LIBS := -L$(JPEG_PREFIX)/lib -ljpeg
  endif
else ifneq ($(wildcard /usr/include/jpeglib.h),)
  ALL_CFLAGS += -DPIX_HAVE_JPEG
  JPEG_LIBS := -ljpeg
endif
LIBS += $(JPEG_LIBS)

$(BIN): $(OBJ)
	$(CC) $(ALL_CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)

# -MMD -MP emits the .d files that keep object files in step with header edits.
# -Isrc/vendor lets the agent drivers find the single shared cJSON.h.
%.o: %.c
	$(CC) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEP)
-include $(wildcard $(BUILD)/*.d)

# The role and kind files under board/ are the only place they are defined:
# this bakes them into the binary, and nothing is read from the config. Generate
# to a temporary file every time so deleting an input cannot disappear from the
# prerequisite list; preserve the target timestamp when its contents are equal.
src/boarddefaults.c: FORCE tools/gen-defaults.sh
	@tmp=$@.tmp; trap 'rm -f $$tmp' EXIT HUP INT TERM; \
	 tools/gen-defaults.sh board > $$tmp; \
	 if ! cmp -s $$tmp $@; then mv $$tmp $@; fi

FORCE:

# Harnesses are built only when asked for: the default target is the app alone.
# `check` runs unattended checks, `manual` builds interactive/platform-dependent
# diagnostics, and `tests` compiles both groups without running the manual group.
tests: $(TOOLS)
manual: $(MANUAL_BINS)

FULL_LIB_TOOLS := spintest chrometest ttytest keydump gitinfotest muxcfgtest \
                  telegramtest boardgridtest workspacetest replboxtest
JPEG_TOOLS := imagerowtest imagefittest imagetest mdtest sessionpresenttest

$(addprefix $(BUILD)/,$(FULL_LIB_TOOLS)): TOOL_LIBS = $(LIBS)
$(addprefix $(BUILD)/,$(JPEG_TOOLS)): TOOL_LIBS = $(JPEG_LIBS)

# Dependencies stay explicit below so each harness remains an isolated module
# link. The compile/link mechanics and flag handling live in one place.
$(TOOLS): $(BUILD)/%: tools/%.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^) $(LDFLAGS) $(TOOL_LIBS)

$(BUILD)/palette: tools/palette.c src/vendor/colors.h | $(BUILD)

$(BUILD)/spintest: tools/spintest.c tools/stubs/tabbar.c src/status.o src/chrome.o src/block.o src/prompt.o src/replframe.o src/replkeys.o src/files.o src/paste.o src/settings.o src/tty.o src/ui.o src/viewport.o src/bash.o src/vendor/impl.o src/vendor/cJSON.o src/text.o | $(BUILD)

$(BUILD)/statustest: tools/statustest.c tools/stubs/tabbar.c src/status.o src/chrome.o src/prompt.o src/replframe.o src/replkeys.o src/files.o src/paste.o src/bash.o src/block.o src/tty.o src/ui.o src/viewport.o src/settings.o src/vendor/impl.o src/vendor/cJSON.o src/text.o | $(BUILD)

$(BUILD)/chrometest: tools/chrometest.c tools/stubs/tabbar.c src/status.o src/chrome.o src/block.o src/prompt.o src/replframe.o src/replkeys.o src/files.o src/paste.o src/settings.o src/tty.o src/ui.o src/viewport.o src/bash.o src/vendor/impl.o src/vendor/cJSON.o src/text.o | $(BUILD)

$(BUILD)/imagerowtest: tools/imagerowtest.c src/image.o src/viewport.o src/ui.o src/tty.o src/settings.o src/scrollback.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/overlaytest: tools/overlaytest.c src/overlay.o src/menu.o src/ui.o src/viewport.o src/tty.o src/settings.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/viewporttest: tools/viewporttest.c src/viewport.o src/ui.o src/tty.o src/settings.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/ttytest: tools/ttytest.c src/tty.o src/viewport.o src/ui.o src/settings.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/keydump: tools/keydump.c src/tty.o src/viewport.o src/ui.o src/settings.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/gitinfotest: tools/gitinfotest.c src/gitinfo.o src/text.o | $(BUILD)

$(BUILD)/imagefittest: tools/imagefittest.c src/image.o src/ui.o src/viewport.o src/block.o src/settings.o src/tty.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/reflowtest: tools/reflowtest.c src/ui.o src/viewport.o src/block.o src/settings.o src/tty.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/sidechannelviewtest: tools/sidechannelviewtest.c src/sidechannelview.o src/ui.o src/viewport.o src/settings.o src/tty.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/sidechannelcmdtest: tools/sidechannelcmdtest.c src/sidechannelcmd.o | $(BUILD)

$(BUILD)/toolstyletest: tools/toolstyletest.c src/toolstyle.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/sessionlisttest: tools/sessionlisttest.c src/sessionlist.o src/vendor/cJSON.o src/text.o | $(BUILD)

$(BUILD)/codextest: tools/codextest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/modelstest: tools/modelstest.c src/models.o src/text.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/groktest: tools/groktest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/filedifftest: tools/filedifftest.c src/filediff.o src/ui.o src/viewport.o src/block.o src/settings.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o src/text.o | $(BUILD)

$(BUILD)/taskstest: tools/taskstest.c src/tasks.o src/text.o src/toolstyle.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/claudetest: tools/claudetest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/pitest: tools/pitest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/agenttabstest: tools/agenttabstest.c src/agenttabs.o src/text.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/imagetest: tools/imagetest.c src/image.o src/md.o src/ui.o src/viewport.o src/block.o src/settings.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o src/text.o | $(BUILD)

$(BUILD)/mdtest: tools/mdtest.c src/md.o src/ui.o src/viewport.o src/block.o src/settings.o src/tty.o src/text.o src/image.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/pastetest: tools/pastetest.c src/paste.o src/text.o | $(BUILD)

$(BUILD)/transcripttest: tools/transcripttest.c src/transcript.o | $(BUILD)

$(BUILD)/sessionviewtest: tools/sessionviewtest.c src/sessionview.o src/filediff.o src/highlight.o src/toolstyle.o src/ui.o src/viewport.o src/block.o src/settings.o src/tty.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/sessionpresenttest: tools/sessionpresenttest.c tools/stubs/tabbar.c src/sessionpresent.o src/sessionview.o src/filediff.o src/highlight.o src/md.o src/prompt.o src/status.o src/tasks.o src/transcript.o src/toolstyle.o src/replframe.o src/replkeys.o src/files.o src/paste.o src/bash.o src/chrome.o src/block.o src/tty.o src/ui.o src/viewport.o src/settings.o src/image.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/sessionloadtest: tools/sessionloadtest.c src/sessionload.o src/transcript.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/muxcfgtest: tools/muxcfgtest.c src/muxcfg.o src/models.o src/settings.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/telegramtest: tools/telegramtest.c src/vendor/cJSON.o | $(BUILD)

$(BUILD)/boardtest: tools/boardtest.c src/board.o src/boardname.o src/boardstep.o src/boardcfg.o src/boarddefaults.o src/boardflow.o src/boardlog.o src/mdcfg.o src/replyjson.o src/child.o src/gitcmd.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/boardgridtest: tools/boardgridtest.c tools/stubs/tabbar.c src/boardgrid.o src/menu.o src/overlay.o src/chrome.o src/block.o src/prompt.o src/replframe.o src/replkeys.o src/files.o src/paste.o src/settings.o src/status.o src/tty.o src/ui.o src/viewport.o src/bash.o src/frontend.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

$(BUILD)/boardtiletest: tools/boardtiletest.c src/boardtile.o src/text.o | $(BUILD)

$(BUILD)/viewstest: tools/viewstest.c src/views.o | $(BUILD)

$(BUILD)/workspacetest: tools/workspacetest.c tools/stubs/tabbar.c src/workspace.o src/status.o src/chrome.o src/block.o src/prompt.o src/replframe.o src/replkeys.o src/files.o src/paste.o src/settings.o src/tty.o src/ui.o src/viewport.o src/bash.o src/vendor/impl.o src/vendor/cJSON.o src/text.o | $(BUILD)

$(BUILD)/highlighttest: tools/highlighttest.c src/highlight.o | $(BUILD)

$(BUILD)/replboxtest: tools/replboxtest.c src/replbox.o src/replframe.o src/replkeys.o src/paste.o src/ui.o src/viewport.o src/tty.o src/settings.o src/text.o src/files.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)

check: $(CHECK_BINS)
	@for t in $^; do echo "$$t"; ./$$t || exit 1; done

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)
	@# -URG would parse as -U RG, a user. -a because the mux running this is an
	@# ancestor of pkill, and ancestors are excluded by default.
	@pkill -SIGURG -a -x $(BIN) || true

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
	rm -f src/*.o src/*.d src/vendor/*.o src/vendor/*.d $(BIN) \
	      src/*.o.tmp src/vendor/*.o.tmp

.PHONY: all install clean check FORCE manual tests
