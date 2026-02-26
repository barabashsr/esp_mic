idf_component_register(
    SRCS "main.c" "wifi.c" "audio.c" "sdcard.c" "wav.c" "webserver.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html"
)
