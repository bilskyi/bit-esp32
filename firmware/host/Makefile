# Runs face.c on the laptop.
#
# face.c deliberately depends on nothing, which is what makes this possible:
# the code that drives the panel is the code that gets tested and previewed
# here. No ESP-IDF, no board, no flashing.
#
#   make test       assert the invariants
#   make preview    write face-preview.html and open it
#   make            both

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -I../main
BUILD   := build

.PHONY: all test preview clean

all: test preview

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/face_test: ../main/face.c ../main/face.h face_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../main/face.c face_test.c

$(BUILD)/face_preview: ../main/face.c ../main/face.h face_preview.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../main/face.c face_preview.c

test: $(BUILD)/face_test
	@./$(BUILD)/face_test

preview: $(BUILD)/face_preview preview-template.html
	@./$(BUILD)/face_preview preview-template.html $(BUILD)/face-preview.html
	@echo "wrote $(BUILD)/face-preview.html"

clean:
	rm -rf $(BUILD)
