# Runs face.c on the laptop.
#
# face.c deliberately depends on nothing, which is what makes this possible:
# the code that drives the panel is the code that gets tested and previewed
# here. No ESP-IDF, no board, no flashing.
#
#   make test       assert the invariants, plain and under ASan/UBSan
#   make asan       just the sanitized rebuild and run
#   make preview    write face-preview.html and open it
#   make            both
#
# The sanitized build exists because plain cc over C11 does not catch
# out-of-bounds reads or writes - it built and passed a heap-buffer-overflow
# in pl_unlock_check for a long time. `make test` runs both builds by
# default so that stays caught; it is not an opt-in extra. But the sanitizer
# runtime isn't everywhere - an older GCC, a minimal CI image, a sandbox -
# so ASAN=0 (make test ASAN=0) drops back to the plain build alone instead
# of failing where a plain build would have worked.

CC           ?= cc
CFLAGS       ?= -std=c11 -O2 -Wall -Wextra -Werror -I../main
ASAN_CFLAGS  := -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g -I../main
BUILD        := build
ASAN_BUILD   := build-asan
ASAN         ?= 1

.PHONY: all test asan preview clean

all: test preview

$(BUILD):
	@mkdir -p $(BUILD)

$(ASAN_BUILD):
	@mkdir -p $(ASAN_BUILD)

$(BUILD)/face_test: ../main/face.c ../main/face.h face_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../main/face.c face_test.c

$(BUILD)/face_preview: ../main/face.c ../main/face.h face_preview.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../main/face.c face_preview.c

$(BUILD)/provision_logic_test: ../main/provision_logic.c ../main/provision_logic.h provision_logic_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../main/provision_logic.c provision_logic_test.c

$(BUILD)/setup_screen_test: ../main/setup_screen.c ../main/setup_screen.h setup_screen_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../main/setup_screen.c setup_screen_test.c

$(ASAN_BUILD)/face_test: ../main/face.c ../main/face.h face_test.c | $(ASAN_BUILD)
	$(CC) $(ASAN_CFLAGS) -o $@ ../main/face.c face_test.c

$(ASAN_BUILD)/provision_logic_test: ../main/provision_logic.c ../main/provision_logic.h provision_logic_test.c | $(ASAN_BUILD)
	$(CC) $(ASAN_CFLAGS) -o $@ ../main/provision_logic.c provision_logic_test.c

$(ASAN_BUILD)/setup_screen_test: ../main/setup_screen.c ../main/setup_screen.h setup_screen_test.c | $(ASAN_BUILD)
	$(CC) $(ASAN_CFLAGS) -o $@ ../main/setup_screen.c setup_screen_test.c

test: $(BUILD)/face_test $(BUILD)/provision_logic_test $(BUILD)/setup_screen_test
	@./$(BUILD)/face_test
	@./$(BUILD)/provision_logic_test
	@./$(BUILD)/setup_screen_test
ifneq ($(ASAN),0)
test: asan
endif

asan: $(ASAN_BUILD)/face_test $(ASAN_BUILD)/provision_logic_test $(ASAN_BUILD)/setup_screen_test
	@./$(ASAN_BUILD)/face_test
	@./$(ASAN_BUILD)/provision_logic_test
	@./$(ASAN_BUILD)/setup_screen_test

preview: $(BUILD)/face_preview preview-template.html
	@./$(BUILD)/face_preview preview-template.html $(BUILD)/face-preview.html
	@echo "wrote $(BUILD)/face-preview.html"

clean:
	rm -rf $(BUILD) $(ASAN_BUILD)
