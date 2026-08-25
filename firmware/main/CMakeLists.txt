# Which bring-up sketch to build. The build order walks these in sequence:
#   capture  - step 2, mic to serial level meter
#   playback - step 3, tone through the amp plus mute-pin discipline
#
#   idf.py build                      # capture, the default
#   idf.py -DSKETCH=playback build    # playback
#
# One project rather than two keeps the pin definitions and sdkconfig in a
# single place, so the sketches cannot drift apart on wiring.
set(SKETCH "capture" CACHE STRING "bring-up sketch: capture | playback")

idf_component_register(SRCS "${SKETCH}_main.c"
                       INCLUDE_DIRS ".")
