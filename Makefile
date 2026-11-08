CC ?= cc
LIBS = -lm
ifeq ($(OS),Windows_NT)
LIBS += -lwinmm
endif
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
SRC = $(wildcard src/*.c)
pdpy11: $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS)
convert-to: tools/convert-to.c src/macro.c src/cond.c src/util.c src/text.c src/charset.c src/platform.c src/macro.h src/cond.h
	$(CC) $(CFLAGS) -o $@ tools/convert-to.c src/macro.c src/cond.c src/util.c src/text.c src/charset.c src/platform.c $(LIBS)
clean:
	rm -f pdpy11 convert-to
.PHONY: clean
test: pdpy11 convert-to
	sh tests/run_golden.sh
	sh tests/run_convert.sh
	sh tests/run_obj.sh
	sh tests/run_sav.sh
	sh tests/run_micro11.sh
	sh tests/run_sound.sh
# macOS: one binary for Apple Silicon and Intel
universal: $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -o pdpy11 $(SRC) $(LIBS)

# ---- release builds (release/) ----
RELFLAGS = -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -DNDEBUG
release-macos: $(SRC) $(wildcard src/*.h)
	mkdir -p release
	$(CC) $(RELFLAGS) -arch arm64 -arch x86_64 -mmacos-version-min=10.13 -o release/pdpy11-macos-universal $(SRC) $(LIBS)
	strip -x release/pdpy11-macos-universal
	codesign --force -s - release/pdpy11-macos-universal
