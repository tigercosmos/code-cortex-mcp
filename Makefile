# Makefile — convenience wrapper over the CMake build (scripts/build.sh).
#
# The canonical build system is CMake (see CMakeLists.txt / scripts/build.sh).
# This thin Makefile exists so `make` and `make install` work from a clean
# checkout. `make install` puts the binary on the system PATH AND deploys the
# embedded skill (plus agent MCP config) via the binary's own `install`
# subcommand.
#
#   make                          # build build/c/code-cortex-mcp
#   make install                  # install binary -> $(BINDIR) and the skill
#   make install PREFIX=$$HOME/.local
#   sudo make install             # system-wide binary (see skill note below)
#   make uninstall
#   make clean
#
# The skill is per-user (written under $CLAUDE_CONFIG_DIR, or ~/.claude). When
# run under sudo, the skill step is executed as $SUDO_USER so it lands in your
# home, not root's. A bare `make install` to /usr/local needs write access —
# use `sudo make install` for a system prefix, or `make install PREFIX=$$HOME/.local`
# to avoid sudo entirely. With DESTDIR set (staged/packaging installs) only the
# binary is staged; the skill step is skipped — run
# `$(BINDIR)/code-cortex-mcp install` yourself after the package is live.

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DESTDIR ?=
BIN       := build/c/code-cortex-mcp
INSTALLED := $(DESTDIR)$(BINDIR)/code-cortex-mcp

.PHONY: all build install uninstall clean test

all: build

build:
	scripts/build.sh

$(BIN):
	scripts/build.sh

# Install the binary onto the system PATH, then deploy the embedded skill +
# agent configuration through the binary's own installer.
install: $(BIN)
	mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m 0755 "$(BIN)" "$(INSTALLED)"
	@echo "==> Installed binary: $(INSTALLED)"
	@if [ -n "$(DESTDIR)" ]; then \
		echo "==> DESTDIR set; staged binary only. Run '$(BINDIR)/code-cortex-mcp install' after staging to deploy the skill."; \
	elif [ -n "$$SUDO_USER" ]; then \
		echo "==> Installing skill + agent config as $$SUDO_USER (not root): $(INSTALLED) install"; \
		sudo -u "$$SUDO_USER" "$(INSTALLED)" install -y; \
	else \
		echo "==> Installing skill + agent config: $(INSTALLED) install"; \
		"$(INSTALLED)" install -y; \
	fi

uninstall:
	-"$(INSTALLED)" uninstall -y
	rm -f "$(INSTALLED)"
	@echo "==> Removed $(INSTALLED)"

clean:
	rm -rf build

test:
	scripts/test.sh
