# shine — MP3-кодировщик с фиксированной точкой (LGPL-2.0, см. COPYING).
# Исходники src/lib из https://github.com/toots/shine, коммит ab5e3526b64af1a2eaa43aa6f441a7312e013519.
idf_component_register(SRCS src/bitstream.c src/huffman.c src/l3bitstream.c src/l3loop.c
                            src/l3mdct.c src/l3subband.c src/layer3.c src/reservoir.c src/tables.c
                       INCLUDE_DIRS src)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-variable -Wno-unused-but-set-variable
                       -Wno-maybe-uninitialized -Wno-sign-compare)
