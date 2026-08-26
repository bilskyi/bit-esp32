# Which sketch to build. The build order walks the first three in sequence:
#   capture  - step 2, mic to serial level meter
#   playback - step 3, tone through the amp plus mute-pin discipline
#   voice    - step 4, the whole device
#
# The rest are bench tools:
#   face     - the OLED on its own: every state and emotion, no WiFi
#   diag, stream, mute, led
#
#   idf.py build                      # capture, the default
#   idf.py -DSKETCH=playback build
#   idf.py -DSKETCH=face build
#
# One project rather than several keeps the pin definitions and sdkconfig in a
# single place, so the sketches cannot drift apart on wiring.
set(SKETCH "capture" CACHE STRING "sketch: capture | playback | voice | face | diag | stream | mute | led")

set(SKETCH_SRCS "${SKETCH}_main.c")

# The display is only linked where it is used. capture and playback exist to
# isolate one piece of hardware each, and dragging an I2C driver into them
# would defeat that.
if(SKETCH STREQUAL "face" OR SKETCH STREQUAL "voice")
    list(APPEND SKETCH_SRCS "face.c" "ssd1306.c")
endif()

idf_component_register(SRCS ${SKETCH_SRCS}
                       INCLUDE_DIRS ".")
