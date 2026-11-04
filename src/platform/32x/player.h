/* player.h - SegaTube's movie player and library (master SH-2). */
#ifndef PLAYER_H
#define PLAYER_H

#include "io.h"

#define PLAY_BACK   0   /* back to the library */
#define PLAY_END    1   /* the movie ended */
#define PLAY_ERROR  2

/* Play movie m in profile kind (KIND_FILM/FULL, falls back to the other if
 * the movie lacks it). */
int  play_movie(int m, int kind);
void library_run(void);

/* shared helpers (player.c) */
void ui_cmd(int cmd, uint16_t p1, uint16_t p2, uint16_t p3);
extern uint16_t pad_pressed, pad_held;
void pad_poll(void);
void pad_flush(void);

#endif
