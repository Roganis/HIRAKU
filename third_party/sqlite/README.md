# SQLite

`sqlite3.c` and `sqlite3.h`, version 3.54.1 (2026-10-16), from the project's amalgamation as released:
https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip (2990966 bytes, SHA3-256
`92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`, as https://www.sqlite.org/download.html gives it).
SHA-256 of the two files here: `sqlite3.c` `0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52`, `sqlite3.h` `d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c`.

SQLite is in the public domain (https://www.sqlite.org/copyright.html): there is no licence file.

For `hiraku-server` only, and only where the system has no SQLite 3 to link (the Windows build): a
build that finds the system's uses that. No core and no desktop package has it.
