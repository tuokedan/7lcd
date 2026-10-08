set(COMPONENT_SRCS "audio.c")
set(COMPONENT_ADD_INCLUDEDIRS "include")

set(COMPONENT_REQUIRES playlist es8388 board spiffs key helix screen)

register_component()
