# Repo-level convenience wrapper. Each firmware/<target>/ directory has its
# own Makefile for building/flashing that specific target -- see
# firmware/*/README.md. This one just runs the cross-target build
# verification suite.
#
#   make test          # incremental build check of every firmware target
#   make test-clean    # full clean rebuild of every firmware target first

.PHONY: test test-clean

test:
	./firmware/test-builds.sh

test-clean:
	./firmware/test-builds.sh --clean
