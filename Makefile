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

# The signed Swift helper that /voice talks to. Built from the vendored source
# in src/vendor/macos-voice and installed beside the binary; voice_helper in
# settings overrides.
VOICE_HELPER     := $(BUILD)/VoiceHelper.app
VOICE_HELPER_DIR := $(PREFIX)/libexec/mux
VOICE_SRC        := src/vendor/macos-voice
ALL_CFLAGS += -DVOICE_HELPER_PATH='"$(VOICE_HELPER_DIR)/VoiceHelper.app"'

CHECK_NAMES  := $(patsubst tests/%.c,%,$(wildcard tests/*.c))
MANUAL_NAMES := $(patsubst tools/%.c,%,$(wildcard tools/*.c))
CHECKS := overlaytest viewporttest imagerowtest chrometest imagefittest mdtest \
          reflowtest toolstyletest sessionlisttest claudetest codextest \
          groktest filedifftest pitest agenttabstest statustest transcripttest \
          sessionviewtest sessionloadtest highlighttest muxcfgtest telegramtest \
          boardtest modelstest boardgridtest boardtiletest viewstest sessionpresenttest \
          workspacetest replboxtest ttytest gitinfotest sidechannelviewtest \
          sidechannelcmdtest taskstest voicetest voicehandofftest filelocktest prompttest \
          dispatchtest voicetabtest grokbottailtest vncinsettest grokvnctest \
          settingstest jevtest
CHECKS += orchstatustest orchinstalltest sessionaddrtest orchtasktest orchtargettest orcheventtest orchclitest orchtest
MANUAL_TOOLS := imagetest keydump palette pastetest spintest vncprobe

# A harness is classified by the directory it sits in: tests/ runs unattended,
# tools/ is driven by hand. Both lists stay explicit so that dropping in a new
# file fails at parse time instead of silently joining `make check`.
ifneq ($(sort $(CHECK_NAMES)),$(sort $(CHECKS)))
$(error every tests/*.c must be listed in CHECKS)
endif
ifneq ($(sort $(MANUAL_NAMES)),$(sort $(MANUAL_TOOLS)))
$(error every tools/*.c must be listed in MANUAL_TOOLS)
endif

CHECK_BINS  := $(addprefix $(BUILD)/,$(CHECKS))
MANUAL_BINS := $(addprefix $(BUILD)/,$(MANUAL_TOOLS))
TOOLS       := $(CHECK_BINS) $(MANUAL_BINS)
SRC     := $(wildcard src/*.c) $(wildcard src/vendor/*.c) $(wildcard src/vendor/mermaid/*.c)
MERMAID_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(wildcard src/vendor/mermaid/*.c))
OBJ     := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
DEP     := $(OBJ:.o=.d)

all: $(BIN)

# forkpty lives in libutil outside the BSDs.
LIBS += -pthread -lcurl

# grokvnc.h (instantiated in vendor/impl.o): zlib for ZRLE, Security.framework
# for TLS. image.o also uses zlib for compressed kitty transmits.
VNC_LIBS := -lz
ifeq ($(shell uname -s),Darwin)
VNC_LIBS += -framework Security -framework CoreFoundation
endif
LIBS += $(VNC_LIBS)
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
$(BUILD)/%.o: src/%.c | $(BUILD) $(BUILD)/vendor $(BUILD)/vendor/mermaid
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

# Skill, quota.sh, and routing seed under orchestrate/ are compiled in and
# written out on startup, so a binary-only deploy still has the machinery.
src/orchdata.c: FORCE tools/gen-embed.sh
	@tmp=$@.tmp; trap 'rm -f $$tmp' EXIT HUP INT TERM; \
	 tools/gen-embed.sh orchestrate orchdata.h orch_files orch_files_n > $$tmp; \
	 if ! cmp -s $$tmp $@; then mv $$tmp $@; fi

# Version is the commit count, so it advances with every commit, plus the
# short hash. Only main.o includes it.
$(BUILD)/version.h: FORCE | $(BUILD)
	@tmp=$@.tmp; trap 'rm -f $$tmp' EXIT HUP INT TERM; \
	 n=$$(git rev-list --count HEAD 2>/dev/null || echo 0); \
	 h=$$(git rev-parse --short HEAD 2>/dev/null || echo unknown); \
	 printf '#define MUX_VERSION "0.%s+%s"\n' "$$n" "$$h" > $$tmp; \
	 if ! cmp -s $$tmp $@; then mv $$tmp $@; fi

$(BUILD)/main.o: $(BUILD)/version.h
$(BUILD)/main.o: ALL_CFLAGS += -I$(BUILD)

FORCE:

# Harnesses are built only when asked for: the default target is the app alone.
# `check` runs unattended checks, `manual` builds interactive/platform-dependent
# diagnostics, and `tests` compiles both groups without running the manual group.
tests: $(TOOLS)
manual: $(MANUAL_BINS)

FULL_LIB_TOOLS := spintest chrometest ttytest keydump gitinfotest muxcfgtest \
                  telegramtest boardgridtest workspacetest replboxtest voicetabtest
JPEG_TOOLS := imagerowtest vncinsettest imagefittest imagetest mdtest sessionpresenttest

$(addprefix $(BUILD)/,$(FULL_LIB_TOOLS)): TOOL_LIBS = $(LIBS)
$(addprefix $(BUILD)/,$(JPEG_TOOLS)): TOOL_LIBS = $(JPEG_LIBS) -pthread -lcurl $(VNC_LIBS)

# vendor/impl.o carries the grokbot backend, which links libcurl.
TOOL_LIBS ?= -pthread -lcurl $(VNC_LIBS)

# Dependencies stay explicit below so each harness remains an isolated module
# link. The compile/link mechanics and flag handling live in one place.
$(CHECK_BINS): $(BUILD)/%: tests/%.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^) $(LDFLAGS) $(TOOL_LIBS)

$(MANUAL_BINS): $(BUILD)/%: tools/%.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^) $(LDFLAGS) $(TOOL_LIBS)

$(BUILD)/palette: tools/palette.c src/vendor/colors.h | $(BUILD)

$(BUILD)/spintest: tools/spintest.c tests/stubs/tabbar.c $(BUILD)/status.o $(BUILD)/chrome.o $(BUILD)/block.o $(BUILD)/prompt.o $(BUILD)/replframe.o $(BUILD)/replkeys.o $(BUILD)/files.o $(BUILD)/paste.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/bash.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/statustest: tests/statustest.c tests/stubs/tabbar.c $(BUILD)/status.o $(BUILD)/chrome.o $(BUILD)/prompt.o $(BUILD)/replframe.o $(BUILD)/replkeys.o $(BUILD)/files.o $(BUILD)/paste.o $(BUILD)/bash.o $(BUILD)/block.o $(BUILD)/tty.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/settings.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/prompttest: tests/prompttest.c tests/stubs/tabbar.c $(BUILD)/status.o $(BUILD)/chrome.o $(BUILD)/prompt.o $(BUILD)/replframe.o $(BUILD)/replkeys.o $(BUILD)/files.o $(BUILD)/paste.o $(BUILD)/bash.o $(BUILD)/block.o $(BUILD)/tty.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/settings.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/chrometest: tests/chrometest.c $(BUILD)/confirm.o $(BUILD)/frontend.o tests/stubs/tabbar.c $(BUILD)/status.o $(BUILD)/chrome.o $(BUILD)/block.o $(BUILD)/prompt.o $(BUILD)/replframe.o $(BUILD)/replkeys.o $(BUILD)/files.o $(BUILD)/paste.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/bash.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/imagerowtest: tests/imagerowtest.c $(BUILD)/image.o $(BUILD)/viewport.o $(BUILD)/ui.o $(BUILD)/tty.o $(BUILD)/settings.o $(BUILD)/scrollback.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/vncinsettest: tests/vncinsettest.c $(BUILD)/vncinset.o $(BUILD)/overlay.o $(BUILD)/image.o $(BUILD)/viewport.o $(BUILD)/ui.o $(BUILD)/tty.o $(BUILD)/settings.o $(BUILD)/scrollback.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/overlaytest: tests/overlaytest.c $(BUILD)/overlay.o $(BUILD)/menu.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/tty.o $(BUILD)/settings.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/viewporttest: tests/viewporttest.c $(BUILD)/viewport.o $(BUILD)/ui.o $(BUILD)/tty.o $(BUILD)/settings.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/ttytest: tests/ttytest.c $(BUILD)/tty.o $(BUILD)/viewport.o $(BUILD)/ui.o $(BUILD)/settings.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/keydump: tools/keydump.c $(BUILD)/tty.o $(BUILD)/viewport.o $(BUILD)/ui.o $(BUILD)/settings.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/gitinfotest: tests/gitinfotest.c $(BUILD)/gitinfo.o $(BUILD)/gitcmd.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/imagefittest: tests/imagefittest.c $(BUILD)/image.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/block.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/reflowtest: tests/reflowtest.c $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/block.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/sidechannelviewtest: tests/sidechannelviewtest.c $(BUILD)/sidechannelview.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/sidechannelcmdtest: tests/sidechannelcmdtest.c $(BUILD)/sidechannelcmd.o | $(BUILD)

$(BUILD)/toolstyletest: tests/toolstyletest.c $(BUILD)/toolstyle.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/sessionlisttest: tests/sessionlisttest.c $(BUILD)/sessionlist.o $(BUILD)/vendor/cJSON.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/codextest: tests/codextest.c $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/modelstest: tests/modelstest.c $(BUILD)/models.o $(BUILD)/text.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/groktest: tests/groktest.c $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/filedifftest: tests/filedifftest.c $(BUILD)/filediff.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/block.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/taskstest: tests/taskstest.c $(BUILD)/tasks.o $(BUILD)/text.o $(BUILD)/toolstyle.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/orchstatustest: tests/orchstatustest.c $(BUILD)/orchstatus.o $(BUILD)/orchtask.o $(BUILD)/text.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/orchinstalltest: tests/orchinstalltest.c $(BUILD)/orchinstall.o $(BUILD)/orchdata.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/claudetest: tests/claudetest.c $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/pitest: tests/pitest.c $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/agenttabstest: tests/agenttabstest.c $(BUILD)/agenttabs.o $(BUILD)/text.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/imagetest: tools/imagetest.c $(BUILD)/image.o $(BUILD)/md.o $(MERMAID_OBJ) $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/block.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/mdtest: tests/mdtest.c $(BUILD)/md.o $(MERMAID_OBJ) $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/block.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/text.o $(BUILD)/image.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/pastetest: tools/pastetest.c $(BUILD)/paste.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/transcripttest: tests/transcripttest.c $(BUILD)/transcript.o | $(BUILD)

$(BUILD)/sessionviewtest: tests/sessionviewtest.c $(BUILD)/sessionview.o $(BUILD)/filediff.o $(BUILD)/highlight.o $(BUILD)/toolstyle.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/block.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/sessionpresenttest: tests/sessionpresenttest.c tests/stubs/tabbar.c $(BUILD)/sessionpresent.o $(BUILD)/sessionview.o $(BUILD)/filediff.o $(BUILD)/highlight.o $(BUILD)/md.o $(MERMAID_OBJ) $(BUILD)/prompt.o $(BUILD)/status.o $(BUILD)/tasks.o $(BUILD)/transcript.o $(BUILD)/toolstyle.o $(BUILD)/replframe.o $(BUILD)/replkeys.o $(BUILD)/files.o $(BUILD)/paste.o $(BUILD)/bash.o $(BUILD)/chrome.o $(BUILD)/block.o $(BUILD)/tty.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/settings.o $(BUILD)/image.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)


$(BUILD)/grokvnctest: tests/grokvnctest.c src/vendor/vnc/grokvnc.h | $(BUILD)

$(BUILD)/vncprobe: tools/vncprobe.c src/vendor/vnc/grokvnc.h $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/grokbottailtest: tests/grokbottailtest.c $(BUILD)/grokbottail.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/sessionloadtest: tests/sessionloadtest.c $(BUILD)/sessionload.o $(BUILD)/transcript.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/settingstest: tests/settingstest.c $(BUILD)/settings.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/muxcfgtest: tests/muxcfgtest.c $(BUILD)/muxcfg.o $(BUILD)/models.o $(BUILD)/settings.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/telegramtest: tests/telegramtest.c $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/boardtest: tests/boardtest.c $(BUILD)/board.o $(BUILD)/boardname.o $(BUILD)/boardstep.o $(BUILD)/boardcfg.o $(BUILD)/boarddefaults.o $(BUILD)/boardflow.o $(BUILD)/boardlog.o $(BUILD)/mdcfg.o $(BUILD)/replyjson.o $(BUILD)/child.o $(BUILD)/gitcmd.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/boardgridtest: tests/boardgridtest.c tests/stubs/tabbar.c $(BUILD)/boardgrid.o $(BUILD)/menu.o $(BUILD)/overlay.o $(BUILD)/chrome.o $(BUILD)/block.o $(BUILD)/prompt.o $(BUILD)/replframe.o $(BUILD)/replkeys.o $(BUILD)/files.o $(BUILD)/paste.o $(BUILD)/settings.o $(BUILD)/status.o $(BUILD)/tty.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/bash.o $(BUILD)/frontend.o $(BUILD)/text.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/boardtiletest: tests/boardtiletest.c $(BUILD)/boardtile.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/viewstest: tests/viewstest.c $(BUILD)/views.o | $(BUILD)

$(BUILD)/workspacetest: tests/workspacetest.c tests/stubs/tabbar.c $(BUILD)/workspace.o $(BUILD)/status.o $(BUILD)/chrome.o $(BUILD)/block.o $(BUILD)/prompt.o $(BUILD)/replframe.o $(BUILD)/replkeys.o $(BUILD)/files.o $(BUILD)/paste.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/bash.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/orchtest: tests/orchtest.c $(BUILD)/orch.o $(BUILD)/orchevent.o $(BUILD)/orchtask.o $(BUILD)/orchtarget.o $(BUILD)/text.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/orchclitest: tests/orchclitest.c $(BUILD)/orchcli.o $(BUILD)/orchtask.o $(BUILD)/orchtarget.o $(BUILD)/text.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/orcheventtest: tests/orcheventtest.c $(BUILD)/orchevent.o $(BUILD)/orchtask.o $(BUILD)/text.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/orchtargettest: tests/orchtargettest.c $(BUILD)/orchtarget.o $(BUILD)/orchtask.o $(BUILD)/text.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/orchtasktest: tests/orchtasktest.c $(BUILD)/orchtask.o $(BUILD)/text.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/sessionaddrtest: tests/sessionaddrtest.c $(BUILD)/sessionaddr.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/dispatchtest: tests/dispatchtest.c $(BUILD)/dispatch.o $(BUILD)/orchevent.o $(BUILD)/orchtask.o $(BUILD)/text.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/highlighttest: tests/highlighttest.c $(BUILD)/highlight.o | $(BUILD)

$(BUILD)/replboxtest: tests/replboxtest.c $(BUILD)/replbox.o $(BUILD)/replframe.o $(BUILD)/replkeys.o $(BUILD)/paste.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/tty.o $(BUILD)/settings.o $(BUILD)/text.o $(BUILD)/files.o $(BUILD)/vendor/impl.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/jevtest: tests/jevtest.c $(BUILD)/jev.o $(BUILD)/vendor/cJSON.o | $(BUILD)

$(BUILD)/voicetest: tests/voicetest.c src/voice.c $(BUILD)/jev.o $(BUILD)/vendor/cJSON.o $(BUILD)/text.o | $(BUILD)

$(BUILD)/voicetabtest: tests/voicetabtest.c tests/stubs/tabbar.c tests/stubs/vendorimpl.c src/voice.c $(BUILD)/jev.o $(BUILD)/workspace.o $(BUILD)/status.o $(BUILD)/chrome.o $(BUILD)/block.o $(BUILD)/prompt.o $(BUILD)/replframe.o $(BUILD)/replkeys.o $(BUILD)/files.o $(BUILD)/paste.o $(BUILD)/settings.o $(BUILD)/tty.o $(BUILD)/ui.o $(BUILD)/viewport.o $(BUILD)/bash.o $(BUILD)/vendor/cJSON.o $(BUILD)/text.o | $(BUILD)

check: $(CHECK_BINS)
	@for t in $^; do echo "$$t"; ./$$t || exit 1; done

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)
	@if [ -d $(VOICE_HELPER) ]; then \
	  install -d $(VOICE_HELPER_DIR); \
	  rm -rf $(VOICE_HELPER_DIR)/VoiceHelper.app; \
	  cp -R $(VOICE_HELPER) $(VOICE_HELPER_DIR)/; \
	fi
	@# -URG would parse as -U RG, a user. -a because the mux running this is an
	@# ancestor of pkill, and ancestors are excluded by default.
	@pkill -SIGURG -a -x $(BIN) || true

# The helper is opt-in: it needs swift and macOS 26.
voice-helper: | $(BUILD)
	$(VOICE_SRC)/build.sh $(abspath $(VOICE_HELPER))

.PHONY: voice-helper

$(BUILD) $(BUILD)/vendor $(BUILD)/vendor/mermaid:
	@mkdir -p $(BUILD)/vendor/mermaid

clean:
	rm -rf $(BUILD)
	rm -f src/*.o src/*.d src/vendor/*.o src/vendor/*.d $(BIN) \
	      src/*.o.tmp src/vendor/*.o.tmp

.PHONY: all install clean check FORCE manual tests
