/* SPDX-License-Identifier: GPL-3.0-or-later */
/* log.h — one-line diagnostics to stderr, prefixed "vigil: ". */
#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#define vg_log(...) do { fprintf(stderr, "vigil: " __VA_ARGS__); fputc('\n', stderr); } while (0)

#endif /* LOG_H */
