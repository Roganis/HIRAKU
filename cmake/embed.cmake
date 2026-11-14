# SPDX-License-Identifier: GPL-3.0-or-later
# A file's bytes as a C++ array, for a program to carry it inside itself:
#   cmake -DIN=FILE -DOUT=FILE.cpp -DNAME=symbol -P cmake/embed.cmake
# OUT defines `extern const unsigned char NAME[]` and `extern const unsigned long NAME_size`.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR size "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}.new" "// made from ${NAME}'s file by cmake/embed.cmake\nextern const unsigned char ${NAME}[];\nextern const unsigned long ${NAME}_size;\nconst unsigned char ${NAME}[] = {\n${bytes}0};\nconst unsigned long ${NAME}_size = ${size};\n")
file(COPY_FILE "${OUT}.new" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.new")
