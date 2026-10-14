# F030SNES: build, run and release all ports. Each game's own Makefile in
# games/<game>/ has the full set of targets; this one forwards the main ones.
#
#   make list               the games, their make targets and programs
#   make <game>             build games/<game> (e.g. make smw)
#   make run-<game>         run it in the patched Hatari
#   make all                build every game that has a translation
#   make release VERSION=v  release/F030SNES-<v>.zip (tools/release.py)

# game:target:program (program relative to games/<game>/build/)
GAMES := \
	smk:smkx:smkx/SMKX.TOS \
	smw:smwx:smwx/SMWX.TOS \
	smw2:yix:yix/YIX.TOS \
	dkc:dkcx:dkcx/DKCX.TOS \
	ct:ctx:ctx/CTX.TOS \
	ff6:ff6x:ff6x/FF6X.TOS \
	smas:smasx:smasx/SMASX.TOS \
	sciv:scivx:scivx/SCIVX.TOS

name = $(word 1,$(subst :, ,$(1)))
target = $(word 2,$(subst :, ,$(1)))
program = $(word 3,$(subst :, ,$(1)))
NAMES := $(foreach g,$(GAMES),$(call name,$(g)))
# SMAS has no translation yet (games/smas/ROADMAP.md, step 1).
BUILT := $(filter-out smas,$(NAMES))

.PHONY: list all release $(NAMES) $(addprefix run-,$(NAMES))

list:
	@$(foreach g,$(GAMES),printf '%-6s make -C games/%-5s %-6s -> games/%s/build/%s\n' \
		$(call name,$(g)) $(call name,$(g)) $(call target,$(g)) $(call name,$(g)) $(call program,$(g));)

all: $(BUILT)

define game_rules
$(call name,$(1)):
	$$(MAKE) -C games/$(call name,$(1)) $(call target,$(1))
run-$(call name,$(1)):
	$$(MAKE) -C games/$(call name,$(1)) run-$(call target,$(1))
endef
$(foreach g,$(GAMES),$(eval $(call game_rules,$(g))))

release:
	@test -n "$(VERSION)" || { echo "usage: make release VERSION=<version>"; exit 1; }
	python3 tools/release.py $(VERSION)
