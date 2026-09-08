# Repo-level convenience wrapper. Each firmware/<target>/ directory has its
# own Makefile for building/flashing that specific target -- see
# firmware/*/README.md. This one just runs the cross-target build
# verification suite.
#
#   make test          # incremental build check of every firmware target
#   make test-clean    # full clean rebuild of every firmware target first
#   make test-audio    # .nib codec decode-correctness regression (no
#                       # hardware, no Salamander download needed --
#                       # see tools/sfz_preprocessor/test_soundbank_regression.py)

.PHONY: test test-clean test-audio

test:
	./firmware/test-builds.sh

test-clean:
	./firmware/test-builds.sh --clean

test-audio:
	@if [ -x tools/sfz_preprocessor/venv/bin/python3 ]; then \
		tools/sfz_preprocessor/venv/bin/python3 tools/sfz_preprocessor/test_soundbank_regression.py; \
	else \
		echo "SKIP: tools/sfz_preprocessor/venv not set up -- see tools/sfz_preprocessor/README.md"; \
	fi
