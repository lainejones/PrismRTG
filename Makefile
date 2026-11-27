# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Copyright (C) 2026 Stefan Reinauer
# Build Prism with amiga-gcc (Bebbo), under WSL or Linux:
#
#     make            everything, then the release drawers (make pkg, make pkg-check)
#     make -j8        the same, in parallel
#     make drivers    only the loadable board drivers
#     make floppy     the install floppy, out/PrismRTG-<release>.adf
#     make package    the release archive, out/PrismRTG-<release>.lha
#     make trace      out/PrismD.trace (tools/mktrace.sh)
#     make check      the host tests (tests/architecture.sh, catalog.sh, guide.sh)
#     make clean      remove out/
#     make V=1        show the full compiler commands
#
# ./build.sh runs "make" (in parallel) and is kept for scripts and CI.
#
# Targets 68020+ (A2000 = 68030, A4000 = 68060); -m68020-60 avoids 64-bit
# mul/div instructions the 060 traps on.
#
# Strip at LINK time with -s. NEVER m68k-amigaos-strip: it corrupts the hunk
# reloc table on this toolchain.

export PATH := /opt/amiga/bin:$(PATH)

CC     = m68k-amigaos-gcc
# -fno-tree-loop-distribute-patterns: no loops turned into calls to libnix's slow memset/memcpy
CFLAGS = -DPRISM_DEBUG -O2 -fno-tree-loop-distribute-patterns -m68020-60 -noixemul -Wall \
         -Wno-pointer-sign -fomit-frame-pointer -Isrc -Isrc/p96sdk
# PrismD and the driver modules: leave out functions nothing calls (debug helpers etc.)
GC     = -ffunction-sections -fdata-sections -Wl,--gc-sections
export CC CFLAGS

# PrismUpdate needs the AmiSSL SDK headers (the include/ directory of
# https://github.com/jens-maus/amissl, Apache 2.0): from AMISSL_INC if that
# exists, else the toolchain's own (amiga-gcc installs them). Without them
# the rest still builds.
AMISSL_INC ?= $(HOME)/amissl-sdk/include
ifneq ($(wildcard $(AMISSL_INC)),)
HAVE_AMISSL = y
AMISSL = -I$(AMISSL_INC)
else ifeq ($(shell printf '\043include <amissl/amissl.h>\n' | PATH='$(PATH)' $(CC) -noixemul -E -x c - >/dev/null 2>&1 && echo y),y)
HAVE_AMISSL = y
endif

O   = out
DEP = $(O)/.deps

# Quiet by default ("CC out/PrismD"); make V=1 shows the full commands.
ifeq ($(V),1)
Q =
say = @:
else ifneq ($(findstring s,$(firstword -$(MAKEFLAGS))),)
Q = @
say = @:
else
Q = @
say = @echo "  $(1)"
endif

.PHONY: all programs drivers pkg pkg-check floppy package check test trace clean
.DELETE_ON_ERROR:

all: programs pkg pkg-check
	@:

# ---- programs -------------------------------------------------------------

# $(call prog,NAME,SOURCES[,FLAGS[,LIBS]]) builds out/NAME from SOURCES in one
# compile-and-link, and records the headers it read in out/.deps/NAME.d
PROGRAMS :=
define prog
PROGRAMS += $(O)/$(1)
$(O)/$(1): $(2) Makefile | $(DEP)
	$$(call say,CC      $$@)
	$$(Q)$$(CC) $$(CFLAGS) $(3) -s -o $$@ $(2) $(4)
	@$$(CC) $$(CFLAGS) $(3) -MM -MP -MT $$@ $(2) > $(DEP)/$(1).d
endef

# M0 tools
$(eval $(call prog,PrismProbe,tools/prismprobe.c))
ifneq ($(wildcard tools/prismtest.c),)
$(eval $(call prog,PrismTest,tools/prismtest.c src/boardops.c src/driver_loader.c))
endif

# M1
PRISMD_SRC = src/prismd.c src/prefs.c src/bitmap.c src/present.c src/render.c src/gels.c \
             src/selftest.c src/pointer.c src/cgx.c src/cgxvideo.c src/p96.c src/stubs.S src/p2c.S \
             src/boardops.c src/driver_loader.c src/arexx.c
$(eval $(call prog,PrismD,$(PRISMD_SRC),$(GC),-lm))
$(eval $(call prog,PrismScreen,tools/prismscreen.c tools/m8tests.c))
$(eval $(call prog,PrismMouse,tools/prismmouse.c))
$(eval $(call prog,PrismBench,tools/prismbench.c,,-lm))

# M5: the tools' texts get IDs for the translation catalog (src/catalog_ids.h,
# catalogs/PrismRTG.cd)
# mkcatalog.py rewrites the header only when an ID changed, so the stamp
# records the run and the programs rebuild only for a real change
CATALOG_IDS = python3 tools/mkcatalog.py --ids tools/prismprefs.c tools/prismsetup.c
$(DEP)/catalog.stamp: tools/prismprefs.c tools/prismsetup.c tools/mkcatalog.py | $(DEP)
	$(call say,CATALOG src/catalog_ids.h)
	$(Q)$(CATALOG_IDS) >/dev/null
	@touch $@
src/catalog_ids.h: $(DEP)/catalog.stamp
	@test -f $@ || $(CATALOG_IDS) >/dev/null
$(O)/PrismPrefs $(O)/PrismSetup: src/catalog_ids.h
$(eval $(call prog,PrismPrefs,tools/prismprefs.c tools/tabs.c src/prefs.c src/catalog.c))

ifeq ($(HAVE_AMISSL),y)
UPDATE_SRC = tools/update/update.c tools/update/upd_prismrtg.c src/prefs.c
$(eval $(call prog,PrismUpdate,$(UPDATE_SRC),$(AMISSL)))
# the same, calling itself 1.0, for the gate's updatecheck: it always finds a
# newer release, so GET goes on through the fetch to the Installer (not packaged)
$(eval $(call prog,PrismUpdate.old,$(UPDATE_SRC),-DUPD_VERSION='"1.0"' $(AMISSL)))
else
.PHONY: update-skipped
programs: update-skipped
update-skipped:
	@echo "PrismUpdate skipped: no AmiSSL SDK headers (AMISSL_INC=$(AMISSL_INC), nor in the toolchain)"
endif

# M6
$(eval $(call prog,PrismMon,tools/prismmon.c))
$(eval $(call prog,PrismSetup,tools/prismsetup.c src/prefs.c src/catalog.c))
$(eval $(call prog,PrismImport,tools/prismimport.c src/prefs.c))
$(eval $(call prog,PrismShow,tools/prismshow.c))
# single-source tools named after their source: out/NAME from tools/NAME.c.
# Hardware probes (real boards only) are among them, built so they cannot
# rot unnoticed.
TOOLS = prismgrab prismdiag iotime p2peek ddcprobe memwhere zztime lockwatch cmreq lastalert \
        wintest prismlog copperlists p96test cgxtest mirrortest trapwatch tmpltest ropcheck \
        vrampeek bigpointer movebench dragcheck dopusscreen busydraw stftest vramcheck \
        prismstate pencheck icondraw
$(foreach t,$(TOOLS),$(eval $(call prog,$(t),tools/$(t).c)))
$(eval $(call prog,cgxbench,tools/cgxbench.c,,-lm))
$(eval $(call prog,muitime,tools/muitime.c,,-lm))
$(eval $(call prog,vlayertest,tools/vlayertest.c tools/cgxvideo_stubs.S))
$(eval $(call prog,scrmove,tools/dragcheck.c,-DSCRMOVE))

# Prism's own Picasso96API.library
PROGRAMS += $(O)/Picasso96API.library $(O)/arexxtest.rexx
$(O)/Picasso96API.library: tools/p96stub.S Makefile | $(DEP)
	$(call say,AS      $@)
	$(Q)$(CC) -nostartfiles -nostdlib -s -o $@ tools/p96stub.S
$(O)/arexxtest.rexx: tools/arexxtest.rexx | $(DEP)
	$(call say,CP      $@)
	$(Q)cp $< $@

# ---- loadable board drivers (tools/mkdriver.sh NAME sources...) -----------

# $(call driver,NAME,SOURCES[,DIR]) builds DIR/NAME.driver (out/Drivers)
DRIVERS :=
define driver
DRIVERS += $(or $(3),$(O)/Drivers)/$(1).driver
$(or $(3),$(O)/Drivers)/$(1).driver: src/driver_module.c $(2) src/boardops.c tools/mkdriver.sh Makefile | $(DEP)
	$$(call say,DRIVER  $$@)
	$$(Q)OUT=$$(@D) sh tools/mkdriver.sh $(1) $(2)
	@$$(CC) $$(CFLAGS) -DPRISM_DRIVER_MODULE -DDRIVER_NAME='"$(1)"' -MM -MP -MT $$@ src/driver_module.c $(2) src/boardops.c > $(DEP)/$(1).driver.d
endef

$(eval $(call driver,PICASSO2,src/drv_picasso2.c))
$(eval $(call driver,ZZ9000,src/drv_zz9000.c))
$(eval $(call driver,Z3660,src/drv_z3660.c))
$(eval $(call driver,P96,src/drv_p96.c src/rtg_ops.c))
$(eval $(call driver,UAEGFX,src/drv_uaegfx.c src/rtg_ops.c))
# the SAGA driver (Vampire V4): packaged from 1.4b1 (ran on HANSolo-0007's A6000, 2026-11-01); BOARD=SAGA
$(eval $(call driver,SAGA,src/drv_saga.c))
# the MiSTer Minimig-AGA core's RTG frame buffer: BOARD=MINIMIG
$(eval $(call driver,MINIMIG,src/drv_minimig.c))
# the template for new drivers must keep building (it is not packaged)
$(eval $(call driver,TEMPLATE,src/drv_template.c,$(O)/template))

drivers: $(DRIVERS)
programs: $(PROGRAMS) $(DRIVERS)

# ---- release drawers and what a release ships ------------------------------

# each drawer is restaged when anything in it changed; its icon, written by
# the script, stands for the drawer
PKG   = $(O)/pkg/PrismRTG.info
CHECK = $(O)/pkg-check/PrismCheck.info
RELEASE := $(shell sed -n 's/^[#]define PRISM_RELEASE *"\(.*\)"/\1/p' src/prism.h)
ADF   = $(O)/PrismRTG-$(RELEASE).adf
LHA   = $(O)/PrismRTG-$(RELEASE).lha

# what tools/mkpkg.sh and tools/mkcheck.sh copy from out/; keep in step
PKG_IN   = PrismD PrismPrefs PrismSetup PrismProbe PrismImport Picasso96API.library PrismMon \
           PrismBench PrismTest \
           $(if $(HAVE_AMISSL),PrismUpdate) \
           $(foreach d,PICASSO2 ZZ9000 Z3660 P96 UAEGFX SAGA MINIMIG,Drivers/$(d).driver)
CHECK_IN = tmpltest p96test vramcheck PrismBench PrismProbe PrismTest prismdiag lastalert prismstate

pkg: $(PKG)
$(PKG): $(addprefix $(O)/,$(PKG_IN)) $(filter-out dist/check,$(wildcard dist/*)) $(wildcard catalogs/*.ct) \
        LICENSE tools/mkpkg.sh tools/mkguide.py tools/mkcatalog.py
	@sh tools/mkpkg.sh

pkg-check: $(CHECK)
$(CHECK): $(addprefix $(O)/,$(CHECK_IN)) $(wildcard dist/check/*) LICENSE tools/mkcheck.sh | $(PKG)
	@sh tools/mkcheck.sh

# the install floppy (needs amitools' xdftool) and the archive PrismUpdate
# fetches, both from out/pkg
floppy: $(ADF)
$(ADF): $(PKG) dist/Disk.info tools/mkadf.sh
	@sh tools/mkadf.sh

package: $(LHA)
$(LHA): $(PKG) tools/mklha.sh
	@sh tools/mklha.sh

# ---- extras ----------------------------------------------------------------

trace:
	sh tools/mktrace.sh

# the host tests (vamos fixtures, stack audit, catalog, AmigaGuide)
check:
	sh tests/architecture.sh
	sh tests/catalog.sh
	sh tests/guide.sh

test: check

clean:
	$(call say,CLEAN   $(O))
	$(Q)rm -rf $(O)

$(DEP):
	$(Q)mkdir -p $@

-include $(wildcard $(DEP)/*.d)
