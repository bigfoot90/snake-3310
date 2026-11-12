/*
 * Snake 3310 - Flipper Zero
 * Copyright (C) 2026 Damian Długosz
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Snake in stile "cellulare monocromatico anni 2000".
 *  - Corpo del serpente con texture a linee diagonali
 *  - 3 campi: senza bordi, bordi esterni, 4 quadranti
 *  - Bonus animali (ragno / topo / ranocchia) che si muovono lentamente: +50 punti, nessuna crescita
 *  - Record per ogni campo salvati su SD
 */

#include <furi.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "splash.h"

/* ------------------------------------------------------------------ */
/* Geometria                                                           */
/* ------------------------------------------------------------------ */

#define COLS 31
#define ROWS 14
#define CELL 4
#define OX 2 /* x pixel della colonna 0 */
#define OY 7 /* y pixel della riga 0 */
#define MAX_LEN (COLS * ROWS)

/* Cornice sui bordi del display: (0,6) - (127,63).
 * Il punteggio occupa le righe 0-4, la riga 5 resta vuota.
 * Griglia 31x14 celle da 4 px: x 2..125, y 7..62 */
#define FRAME_X0 0
#define FRAME_Y0 6
#define FRAME_X1 127
#define FRAME_Y1 63

/* Quadranti: muro centrale */
#define WALL_COL 15
#define WALL_ROW 6

#define FAT_GROW 1 /* boccone: allunga il serpente quando arriva alla coda */
#define FAT_BONUS 2 /* preda bonus: solo pancia, nessuna crescita */

#define OCC_WALL 1
#define OCC_SNAKE 2

#define FOOD_PER_BONUS_MIN 4
#define FOOD_PER_BONUS_MAX 7
#define BONUS_LIFE 20 /* unita' del countdown mostrato in alto */
#define BONUS_STEPS_PER_UNIT 4 /* passi del serpente per unita' */
#define BONUS_MOVE_EVERY 4 /* l'animale si muove ogni N passi del serpente */
#define BONUS_POINTS 50

#define SAVE_PATH APP_DATA_PATH("snake_3310.save")
#define SAVE_MAGIC 0x534E4B31u /* "SNK1" */

typedef enum {
    DirUp,
    DirRight,
    DirDown,
    DirLeft,
} Dir;

typedef enum {
    ModeOpen, /* senza bordi: si esce da un lato e si rientra dall'altro */
    ModeBox, /* bordi esterni: toccare il bordo = game over */
    ModeQuad, /* muri interni a croce, bordi esterni passanti */
    ModeMaze, /* labirinto "Girandola": solo muri interni, bordi passanti */
    ModeCount,
} Mode;

typedef enum {
    ScreenSplash,
    ScreenMenu,
    ScreenPlay,
    ScreenPause,
    ScreenDying,
    ScreenOver,
} Screen;

typedef enum {
    AnimalNone,
    AnimalSpider,
    AnimalMouse,
    AnimalFrog,
} AnimalKind;

typedef struct {
    int8_t x;
    int8_t y;
} Pt;

typedef struct {
    uint32_t magic;
    uint8_t mode;
    uint8_t speed;
    uint8_t sound; /* 0 = mai salvato (default attivo), 1 = attivi, 2 = disattivati */
    uint8_t reserved;
    uint32_t best[ModeCount];
} SaveData;

typedef struct {
    FuriMutex* mutex;
    NotificationApp* notif;

    Screen screen;
    Mode mode;
    uint8_t speed; /* 1..9 */
    bool sound;
    uint8_t menu_sel;

    /* serpente: buffer circolare, body[head] e' la testa */
    Pt body[MAX_LEN];
    uint8_t fat[MAX_LEN]; /* pancia sul segmento: FAT_GROW boccone, FAT_BONUS preda */
    uint16_t head;
    uint16_t len;
    uint16_t grow;
    Dir dir;
    Dir dir_queue[2];
    uint8_t dir_queue_n;

    uint8_t occ[ROWS][COLS];

    Pt food;
    bool has_food;

    AnimalKind animal;
    Pt animal_pos; /* occupa (x,y) e (x+1,y) */
    bool animal_face_right;
    uint8_t animal_frame;
    uint8_t animal_life;
    uint8_t animal_sub;
    uint8_t animal_move_cnt;
    uint8_t food_since_bonus;
    uint8_t food_for_next_bonus;

    uint32_t score;
    uint32_t best[ModeCount];
    bool new_record;
    bool won;

    uint8_t dying_frame;
    uint32_t last_step;
    uint32_t splash_start;
} Game;

static const char* const mode_names[ModeCount] = {
    "Senza bordi",
    "Bordi",
    "Quadranti",
    "Labirinto",
};

/* ------------------------------------------------------------------ */
/* Sprite                                                              */
/* ------------------------------------------------------------------ */

/* Animali 8x4 pixel, occupano due celle orizzontali. Una riga = 8 bit, MSB a sinistra. */
static const uint8_t spr_spider[2][4] = {
    {0xA5, 0x7E, 0xBD, 0x42},
    {0x42, 0xBD, 0x7E, 0xA5},
};

/* topo rivolto a sinistra; specchiato quando va a destra */
/* ranocchia vista di lato, rivolta a sinistra; frame 1 = salto con zampe distese */
static const uint8_t spr_frog[2][4] = {
    {0x40, 0xFC, 0x7F, 0x9B},
    {0x40, 0xFC, 0x7D, 0x83},
};

static const uint8_t spr_mouse[2][4] = {
    {0x60, 0xBC, 0xFD, 0x4A},
    {0x60, 0xBC, 0xFD, 0x25},
};

/* cifre 3x5, 3 bit per riga (MSB a sinistra) */
static const uint8_t digits3x5[10][5] = {
    {7, 5, 5, 5, 7},
    {2, 6, 2, 2, 7},
    {7, 1, 7, 4, 7},
    {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7},
    {7, 1, 1, 1, 1},
    {7, 5, 7, 5, 7},
    {7, 5, 7, 1, 7},
};

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

static uint32_t rnd(uint32_t n) {
    if(n == 0) return 0;
    return furi_hal_random_get() % n;
}

static Pt dir_delta(Dir d) {
    switch(d) {
    case DirUp:
        return (Pt){0, -1};
    case DirRight:
        return (Pt){1, 0};
    case DirDown:
        return (Pt){0, 1};
    default:
        return (Pt){-1, 0};
    }
}

static bool dir_opposite(Dir a, Dir b) {
    return ((a + 2) & 3) == b;
}

static uint32_t step_interval_ms(uint8_t speed) {
    /* velocita' 1 = 300 ms, velocita' 9 = 60 ms */
    return 300 - (uint32_t)(speed - 1) * 30;
}

static Pt seg(const Game* g, uint16_t i) {
    /* i = 0 testa, i = len-1 coda */
    return g->body[(g->head + MAX_LEN - i) % MAX_LEN];
}

static bool in_bounds(int x, int y) {
    return x >= 0 && x < COLS && y >= 0 && y < ROWS;
}

static bool on_animal(const Game* g, int x, int y) {
    if(g->animal == AnimalNone) return false;
    return y == g->animal_pos.y && (x == g->animal_pos.x || x == g->animal_pos.x + 1);
}

static bool cell_free(const Game* g, int x, int y) {
    if(!in_bounds(x, y)) return false;
    if(g->occ[y][x]) return false;
    if(g->has_food && g->food.x == x && g->food.y == y) return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* Salvataggio                                                         */
/* ------------------------------------------------------------------ */

static void save_load(Game* g) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    SaveData sd;
    memset(&sd, 0, sizeof(sd));
    if(storage_file_open(file, SAVE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        /* accetta anche i salvataggi vecchi, con meno record (prima del Labirinto) */
        size_t n = storage_file_read(file, &sd, sizeof(sd));
        if(n >= offsetof(SaveData, best) + 3 * sizeof(uint32_t) && sd.magic == SAVE_MAGIC) {
            if(sd.mode < ModeCount) g->mode = sd.mode;
            if(sd.speed >= 1 && sd.speed <= 9) g->speed = sd.speed;
            memcpy(g->best, sd.best, sizeof(g->best));
            g->sound = sd.sound != 2;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void save_store(const Game* g) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    SaveData sd;
    memset(&sd, 0, sizeof(sd));
    sd.magic = SAVE_MAGIC;
    sd.mode = g->mode;
    sd.speed = g->speed;
    sd.sound = g->sound ? 1 : 2;
    memcpy(sd.best, g->best, sizeof(sd.best));
    if(storage_file_open(file, SAVE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, &sd, sizeof(sd));
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* ------------------------------------------------------------------ */
/* Logica di gioco                                                     */
/* ------------------------------------------------------------------ */

/* Suono del bonus animale: arpeggio Do-Mi-Sol-Do con vibrazione */
static const NotificationSequence seq_bonus = {
    &message_green_255,
    &message_vibro_on,
    &message_note_c5,
    &message_delay_50,
    &message_vibro_off,
    &message_note_e5,
    &message_delay_50,
    &message_note_g5,
    &message_delay_50,
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_green_0,
    NULL,
};

/* Bonus senza suoni: solo vibrazione e led */
static const NotificationSequence seq_bonus_mute = {
    &message_green_255,
    &message_vibro_on,
    &message_delay_50,
    &message_vibro_off,
    &message_delay_100,
    &message_green_0,
    NULL,
};

/* Boccone: stesso suono del bonus ma piu' breve (due note rapide, niente vibrazione) */
static const NotificationSequence seq_eat = {
    &message_green_255,
    &message_note_c5,
    &message_delay_25,
    &message_note_e5,
    &message_delay_25,
    &message_sound_off,
    &message_green_0,
    NULL,
};

static void place_food(Game* g) {
    uint16_t free_cells = 0;
    for(int y = 0; y < ROWS; y++)
        for(int x = 0; x < COLS; x++)
            if(!g->occ[y][x] && !on_animal(g, x, y)) free_cells++;

    if(free_cells == 0) {
        g->has_food = false;
        return;
    }
    uint16_t pick = rnd(free_cells);
    for(int y = 0; y < ROWS; y++) {
        for(int x = 0; x < COLS; x++) {
            if(!g->occ[y][x] && !on_animal(g, x, y)) {
                if(pick == 0) {
                    g->food = (Pt){x, y};
                    g->has_food = true;
                    return;
                }
                pick--;
            }
        }
    }
}

static bool animal_fits(const Game* g, int x, int y) {
    return cell_free(g, x, y) && cell_free(g, x + 1, y);
}

static void spawn_animal(Game* g) {
    /* conta le posizioni valide (2 celle libere affiancate) */
    uint16_t count = 0;
    for(int y = 0; y < ROWS; y++)
        for(int x = 0; x < COLS - 1; x++)
            if(animal_fits(g, x, y)) count++;
    if(count == 0) return;

    uint16_t pick = rnd(count);
    for(int y = 0; y < ROWS; y++) {
        for(int x = 0; x < COLS - 1; x++) {
            if(animal_fits(g, x, y)) {
                if(pick == 0) {
                    g->animal = (AnimalKind)(AnimalSpider + rnd(3));
                    g->animal_pos = (Pt){x, y};
                    g->animal_face_right = rnd(2);
                    g->animal_frame = 0;
                    g->animal_life = BONUS_LIFE;
                    g->animal_sub = 0;
                    g->animal_move_cnt = 0;
                    return;
                }
                pick--;
            }
        }
    }
}

static void move_animal(Game* g) {
    /* una casella per volta, in direzione casuale, solo su celle libere */
    Dir order[4] = {DirUp, DirRight, DirDown, DirLeft};
    for(int i = 3; i > 0; i--) {
        int j = rnd(i + 1);
        Dir t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    /* ogni tanto resta fermo */
    if(rnd(5) == 0) return;

    for(int i = 0; i < 4; i++) {
        Pt d = dir_delta(order[i]);
        int nx = g->animal_pos.x + d.x;
        int ny = g->animal_pos.y + d.y;
        if(animal_fits(g, nx, ny)) {
            g->animal_pos = (Pt){nx, ny};
            if(d.x > 0) g->animal_face_right = true;
            if(d.x < 0) g->animal_face_right = false;
            g->animal_frame ^= 1;
            return;
        }
    }
}

/* Labirinto "Girandola" (31x14, simmetrico ruotandolo di 180 gradi).
 * '#' = muro. Nessun muro sul bordo: si esce da un lato e si rientra dall'altro. */
static const char* const maze_map[ROWS] = {
    "...............#...............",
    "...............#...............",
    "...............#...............",
    ".....########.....#####........",
    ".....#.........................",
    ".....#.........................",
    ".............###...............",
    "...............###.............",
    ".........................#.....",
    ".........................#.....",
    "........#####.....########.....",
    "...............#...............",
    "...............#...............",
    "...............#...............",
};

static void build_walls(Game* g) {
    memset(g->occ, 0, sizeof(g->occ));
    if(g->mode == ModeQuad) {
        for(int y = 0; y < ROWS; y++)
            g->occ[y][WALL_COL] = OCC_WALL;
        for(int x = 0; x < COLS; x++)
            g->occ[WALL_ROW][x] = OCC_WALL;
    } else if(g->mode == ModeMaze) {
        for(int y = 0; y < ROWS; y++)
            for(int x = 0; x < COLS; x++)
                if(maze_map[y][x] == '#') g->occ[y][x] = OCC_WALL;
    }
}

/* muri del labirinto: 2 px al centro della cella, uniti ai muri vicini */
static bool is_wall(const Game* g, int x, int y) {
    return in_bounds(x, y) && (g->occ[y][x] & OCC_WALL);
}

static void draw_maze_walls(Canvas* c, const Game* g) {
    for(int y = 0; y < ROWS; y++) {
        for(int x = 0; x < COLS; x++) {
            if(!is_wall(g, x, y)) continue;
            int cx = OX + x * CELL;
            int cy = OY + y * CELL;
            canvas_draw_box(c, cx + 1, cy + 1, 2, 2);
            if(is_wall(g, x + 1, y)) canvas_draw_box(c, cx + 3, cy + 1, 2, 2);
            if(is_wall(g, x, y + 1)) canvas_draw_box(c, cx + 1, cy + 3, 2, 2);
        }
    }
}

static void game_reset(Game* g) {
    build_walls(g);

    int sx, sy;
    if(g->mode == ModeQuad) {
        sx = 6; /* quadrante in alto a sinistra */
        sy = 2;
    } else {
        sx = 10;
        sy = 7;
    }

    g->len = 5;
    g->head = g->len - 1;
    for(int i = 0; i < g->len; i++) {
        /* body[0] = coda ... body[len-1] = testa */
        g->body[i] = (Pt){sx - (g->len - 1 - i), sy};
        g->occ[sy][sx - (g->len - 1 - i)] |= OCC_SNAKE;
    }
    g->grow = 0;
    memset(g->fat, 0, sizeof(g->fat));
    g->dir = DirRight;
    g->dir_queue_n = 0;

    g->animal = AnimalNone;
    g->food_since_bonus = 0;
    g->food_for_next_bonus =
        FOOD_PER_BONUS_MIN + rnd(FOOD_PER_BONUS_MAX - FOOD_PER_BONUS_MIN + 1);

    g->has_food = false;
    place_food(g);

    g->score = 0;
    g->new_record = false;
    g->won = false;
    g->dying_frame = 0;
    g->last_step = furi_get_tick();
    g->screen = ScreenPlay;
}

static void game_end(Game* g) {
    if(g->score > g->best[g->mode]) {
        g->best[g->mode] = g->score;
        g->new_record = true;
    }
    save_store(g);
}

static void start_dying(Game* g) {
    notification_message(g->notif, &sequence_single_vibro);
    notification_message(g->notif, &sequence_blink_red_100);
    g->screen = ScreenDying;
    g->dying_frame = 0;
    g->last_step = furi_get_tick();
}

static void game_step(Game* g) {
    /* direzione dalla coda di input */
    if(g->dir_queue_n > 0) {
        Dir nd = g->dir_queue[0];
        g->dir_queue[0] = g->dir_queue[1];
        g->dir_queue_n--;
        if(!dir_opposite(nd, g->dir)) g->dir = nd;
    }

    Pt h = seg(g, 0);
    Pt d = dir_delta(g->dir);
    int nx = h.x + d.x;
    int ny = h.y + d.y;

    if(!in_bounds(nx, ny)) {
        if(g->mode == ModeBox) {
            start_dying(g);
            return;
        }
        nx = (nx + COLS) % COLS;
        ny = (ny + ROWS) % ROWS;
    }

    /* il serpente si allunga quando la coda raggiunge il boccone ingoiato */
    uint16_t tail_idx = (g->head + MAX_LEN - (g->len - 1)) % MAX_LEN;
    bool growing = g->grow > 0 || g->fat[tail_idx] == FAT_GROW;
    Pt tail = seg(g, g->len - 1);
    if(!growing) g->occ[tail.y][tail.x] &= ~OCC_SNAKE;

    if(g->occ[ny][nx] & (OCC_WALL | OCC_SNAKE)) {
        if(!growing) g->occ[tail.y][tail.x] |= OCC_SNAKE;
        start_dying(g);
        return;
    }

    g->head = (g->head + 1) % MAX_LEN;
    g->body[g->head] = (Pt){nx, ny};
    g->fat[g->head] = 0;
    g->occ[ny][nx] |= OCC_SNAKE;
    if(growing) {
        g->len++;
        if(g->fat[tail_idx] == FAT_GROW)
            g->fat[tail_idx] = 0;
        else
            g->grow--;
    }

    /* cibo */
    if(g->has_food && g->food.x == nx && g->food.y == ny) {
        g->score += g->speed;
        g->fat[g->head] = FAT_GROW;
        g->has_food = false;
        if(g->sound)
            notification_message(g->notif, &seq_eat);
        else
            notification_message(g->notif, &sequence_blink_green_10);

        if(g->animal == AnimalNone) {
            g->food_since_bonus++;
            if(g->food_since_bonus >= g->food_for_next_bonus) {
                g->food_since_bonus = 0;
                g->food_for_next_bonus =
                    FOOD_PER_BONUS_MIN + rnd(FOOD_PER_BONUS_MAX - FOOD_PER_BONUS_MIN + 1);
                spawn_animal(g);
            }
        }
        place_food(g);
        if(!g->has_food) {
            /* campo pieno: vittoria */
            g->won = true;
            game_end(g);
            g->screen = ScreenOver;
            return;
        }
    }

    /* bonus animale */
    if(g->animal != AnimalNone) {
        if(on_animal(g, nx, ny)) {
            g->score += BONUS_POINTS;
            g->fat[g->head] = FAT_BONUS;
            g->animal = AnimalNone;
            if(g->sound)
                notification_message(g->notif, &seq_bonus);
            else
                notification_message(g->notif, &seq_bonus_mute);
        } else {
            if(++g->animal_move_cnt >= BONUS_MOVE_EVERY) {
                g->animal_move_cnt = 0;
                move_animal(g);
            }
            if(++g->animal_sub >= BONUS_STEPS_PER_UNIT) {
                g->animal_sub = 0;
                if(g->animal_life > 0) g->animal_life--;
                if(g->animal_life == 0) g->animal = AnimalNone;
            }
        }
    }
}

static void queue_dir(Game* g, Dir d) {
    Dir last = g->dir_queue_n ? g->dir_queue[g->dir_queue_n - 1] : g->dir;
    if(d == last || dir_opposite(d, last)) return;
    if(g->dir_queue_n < 2) g->dir_queue[g->dir_queue_n++] = d;
}

/* ------------------------------------------------------------------ */
/* Disegno                                                             */
/* ------------------------------------------------------------------ */

/* Grafica del serpente, celle 4x4 (il corpo occupa le 2 righe/colonne centrali).
 * Tutti i disegni sono nel riferimento "testa verso sinistra" e vengono
 * ruotati/specchiati in base alla direzione verso la testa (vedi xform).
 *
 *   ...#....                    .##.  <- rigonfiamento (boccone)
 *   .##.#.##.###.###.###.###.###.#.##.##..
 *   .###.##.#.###.###.###.###.##.##.####..
 *    testa  corpo ...                .##.   coda
 */
static const char* const bmp_head_closed[4] = {"...#", ".##.", ".###", "...."};
static const char* const bmp_head_open[4] = {".#.#", "..#.", "..##", ".#.."};
static const char* const bmp_body[4] = {"....", "#.##", "##.#", "...."};
static const char* const bmp_tail[4] = {"....", "##..", "####", "...."};
/* curve: dipendono solo dai due lati collegati */
static const char* const bmp_corner_lu[4] = {".##.", "#.#.", "##..", "...."};
static const char* const bmp_corner_ru[4] = {".##.", ".#.#", "..##", "...."};
static const char* const bmp_corner_ld[4] = {"....", "##..", "#.#.", ".##."};
static const char* const bmp_corner_rd[4] = {"....", "..##", ".#.#", ".##."};
static const char* const bmp_food[4] = {"..#.", ".#.#", "..#.", "...."};

/* dal riferimento "verso sinistra" alla direzione d */
static void xform(Dir d, int x, int y, int* px, int* py) {
    switch(d) {
    case DirRight:
        *px = 3 - x;
        *py = y;
        break;
    case DirUp:
        *px = y;
        *py = x;
        break;
    case DirDown:
        *px = y;
        *py = 3 - x;
        break;
    default:
        *px = x;
        *py = y;
        break;
    }
}

static void draw_bmp4(Canvas* c, int cx, int cy, const char* const bmp[4], Dir d) {
    for(int y = 0; y < 4; y++) {
        for(int x = 0; x < 4; x++) {
            if(bmp[y][x] != '#') continue;
            int px, py;
            xform(d, x, y, &px, &py);
            canvas_draw_dot(c, cx + px, cy + py);
        }
    }
}

/* striscia orizzontale di corpo lunga w pixel (usata nel menu), testa a destra */
static void draw_body_strip(Canvas* c, int x, int y, int w) {
    for(int i = 0; i < w; i++) {
        int lx = 3 - ((w - 1 - i) & 3);
        if(bmp_body[1][3 - lx] == '#') canvas_draw_dot(c, x + i, y);
        if(bmp_body[2][3 - lx] == '#') canvas_draw_dot(c, x + i, y + 1);
    }
}

static void draw_head_at(Canvas* c, int cx, int cy, Dir dir, bool open) {
    draw_bmp4(c, cx, cy, open ? bmp_head_open : bmp_head_closed, dir);
}

static void draw_number(Canvas* c, int x, int y, uint32_t value, int min_digits) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%0*lu", min_digits, (unsigned long)value);
    for(int k = 0; buf[k]; k++) {
        const uint8_t* g = digits3x5[buf[k] - '0'];
        for(int r = 0; r < 5; r++)
            for(int b = 0; b < 3; b++)
                if(g[r] & (4 >> b)) canvas_draw_dot(c, x + k * 4 + b, y + r);
    }
}

static void draw_sprite8x4(Canvas* c, int x, int y, const uint8_t* rows, bool mirror) {
    for(int r = 0; r < 4; r++)
        for(int b = 0; b < 8; b++) {
            int bit = mirror ? b : (7 - b);
            if(rows[r] & (1 << bit)) canvas_draw_dot(c, x + b, y + r);
        }
}

static const uint8_t* animal_rows(const Game* g) {
    if(g->animal == AnimalSpider) return spr_spider[g->animal_frame & 1];
    if(g->animal == AnimalFrog) return spr_frog[g->animal_frame & 1];
    return spr_mouse[g->animal_frame & 1];
}

/* direzione dalla cella a verso la cella vicina b, tenendo conto del wrap */
static Dir dir_to(Pt a, Pt b) {
    int dx = b.x - a.x, dy = b.y - a.y;
    if(dx > 1) dx = -1;
    if(dx < -1) dx = 1;
    if(dy > 1) dy = -1;
    if(dy < -1) dy = 1;
    if(dx > 0) return DirRight;
    if(dx < 0) return DirLeft;
    if(dy > 0) return DirDown;
    return DirUp;
}

static bool dir_vertical(Dir d) {
    return d == DirUp || d == DirDown;
}

static void draw_segment(Canvas* c, const Game* g, uint16_t i) {
    Pt p = seg(g, i);
    int cx = OX + p.x * CELL;
    int cy = OY + p.y * CELL;

    Dir a = dir_to(p, seg(g, i - 1)); /* verso la testa */
    bool is_tail = (i + 1 >= g->len);

    if(is_tail) {
        draw_bmp4(c, cx, cy, bmp_tail, a);
    } else {
        Dir b = dir_to(p, seg(g, i + 1)); /* verso la coda */
        if(dir_vertical(a) == dir_vertical(b)) {
            draw_bmp4(c, cx, cy, bmp_body, a);
        } else {
            bool left = (a == DirLeft || b == DirLeft);
            bool up = (a == DirUp || b == DirUp);
            const char* const* bmp = left ? (up ? bmp_corner_lu : bmp_corner_ld) :
                                            (up ? bmp_corner_ru : bmp_corner_rd);
            draw_bmp4(c, cx, cy, bmp, DirLeft);
        }
    }

    if(g->fat[(g->head + MAX_LEN - i) % MAX_LEN]) {
        /* rigonfiamento: 2 px sopra e 2 sotto (o ai lati se verticale) */
        static const char* const bump[4] = {".##.", "....", "....", ".##."};
        draw_bmp4(c, cx, cy, bump, a);
    }
}

static bool mouth_open(const Game* g) {
    Pt h = seg(g, 0);
    Pt d = dir_delta(g->dir);
    int nx = (h.x + d.x + COLS) % COLS;
    int ny = (h.y + d.y + ROWS) % ROWS;
    if(g->has_food && g->food.x == nx && g->food.y == ny) return true;
    return on_animal(g, nx, ny);
}

static void draw_snake(Canvas* c, const Game* g) {
    for(uint16_t i = 1; i < g->len; i++)
        draw_segment(c, g, i);
    Pt h = seg(g, 0);
    draw_head_at(c, OX + h.x * CELL, OY + h.y * CELL, g->dir, mouth_open(g));
}

static void draw_dotted_frame(Canvas* c) {
    for(int x = FRAME_X0; x <= FRAME_X1; x += 2) {
        canvas_draw_dot(c, x, FRAME_Y0);
        canvas_draw_dot(c, x, FRAME_Y1);
    }
    for(int y = FRAME_Y0; y <= FRAME_Y1; y += 2) {
        canvas_draw_dot(c, FRAME_X0, y);
        canvas_draw_dot(c, FRAME_X1, y);
    }
}

static void draw_field(Canvas* c, const Game* g) {
    /* intestazione: punteggio a sinistra, bonus + countdown a destra */
    draw_number(c, 1, 0, g->score, 4);
    if(g->animal != AnimalNone) {
        draw_sprite8x4(c, 108, 1, animal_rows(g), false);
        draw_number(c, 119, 0, g->animal_life, 2);
    }

    switch(g->mode) {
    case ModeBox:
        canvas_draw_frame(c, FRAME_X0, FRAME_Y0, FRAME_X1 - FRAME_X0 + 1, FRAME_Y1 - FRAME_Y0 + 1);
        break;
    case ModeOpen:
        draw_dotted_frame(c);
        break;
    case ModeQuad: {
        draw_dotted_frame(c);
        int wx = OX + WALL_COL * CELL;
        int wy = OY + WALL_ROW * CELL;
        canvas_draw_box(c, wx + 1, FRAME_Y0 + 1, 2, FRAME_Y1 - FRAME_Y0 - 1);
        canvas_draw_box(c, FRAME_X0 + 1, wy + 1, FRAME_X1 - FRAME_X0 - 1, 2);
        break;
    }
    case ModeMaze:
        draw_dotted_frame(c);
        draw_maze_walls(c, g);
        break;
    default:
        break;
    }

    /* cibo: piccolo rombo */
    if(g->has_food) {
        draw_bmp4(c, OX + g->food.x * CELL, OY + g->food.y * CELL, bmp_food, DirLeft);
    }

    if(g->animal != AnimalNone) {
        int ax = OX + g->animal_pos.x * CELL;
        int ay = OY + g->animal_pos.y * CELL;
        bool mirror = (g->animal != AnimalSpider) && g->animal_face_right;
        draw_sprite8x4(c, ax, ay, animal_rows(g), mirror);
    }

    bool show_snake = true;
    if(g->screen == ScreenDying) show_snake = (g->dying_frame & 1) == 0;
    if(show_snake) draw_snake(c, g);
}

static void draw_menu(Canvas* c, const Game* g) {
    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 1, AlignCenter, AlignTop, "SNAKE 3310");

    /* serpentello decorativo con la stessa texture */
    draw_body_strip(c, 24, 14, 76);
    draw_head_at(c, 100, 13, DirRight, true);
    /* boccone davanti alla bocca */
    draw_bmp4(c, 104, 13, bmp_food, DirRight);

    canvas_set_font(c, FontSecondary);
    char buf[32];
    const int y0 = 27;
    const int dy = 9;

    canvas_draw_str(c, 8, y0, "Gioca");
    snprintf(buf, sizeof(buf), "Campo: %s", mode_names[g->mode]);
    canvas_draw_str(c, 8, y0 + dy, buf);
    snprintf(buf, sizeof(buf), "Velocita': %u", g->speed);
    canvas_draw_str(c, 8, y0 + dy * 2, buf);
    snprintf(buf, sizeof(buf), "Suoni: %s", g->sound ? "Si'" : "No");
    canvas_draw_str(c, 8, y0 + dy * 3, buf);

    /* cursore */
    int cy = y0 + dy * g->menu_sel - 6;
    canvas_draw_box(c, 2, cy + 1, 3, 3);

    if(g->menu_sel > 0) {
        canvas_draw_str(c, 116, y0 + dy * g->menu_sel, "<>");
    }

    snprintf(buf, sizeof(buf), "Record: %04lu", (unsigned long)g->best[g->mode]);
    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, buf);
}

static void draw_box_msg(Canvas* c, int y, int h) {
    canvas_set_color(c, ColorWhite);
    canvas_draw_box(c, 4, y, 120, h);
    canvas_set_color(c, ColorBlack);
    canvas_draw_frame(c, 4, y, 120, h);
    canvas_draw_frame(c, 6, y + 2, 116, h - 4);
}

static void draw_callback(Canvas* c, void* ctx) {
    Game* g = ctx;
    furi_mutex_acquire(g->mutex, FuriWaitForever);
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);

    char buf[32];
    switch(g->screen) {
    case ScreenSplash:
        canvas_draw_xbm(c, 0, 0, 128, 64, splash_xbm);
        break;
    case ScreenMenu:
        draw_menu(c, g);
        break;
    case ScreenPlay:
    case ScreenDying:
        draw_field(c, g);
        break;
    case ScreenPause:
        draw_field(c, g);
        draw_box_msg(c, 18, 30);
        canvas_set_font(c, FontPrimary);
        canvas_draw_str_aligned(c, 64, 22, AlignCenter, AlignTop, "PAUSA");
        canvas_set_font(c, FontSecondary);
        canvas_draw_str_aligned(c, 64, 35, AlignCenter, AlignTop, "OK:gioca  Back:menu");
        break;
    case ScreenOver:
        draw_field(c, g);
        draw_box_msg(c, 8, 48);
        canvas_set_font(c, FontPrimary);
        canvas_draw_str_aligned(
            c, 64, 12, AlignCenter, AlignTop, g->won ? "HAI VINTO!" : "GAME OVER");
        canvas_set_font(c, FontSecondary);
        snprintf(buf, sizeof(buf), "Punti: %04lu", (unsigned long)g->score);
        canvas_draw_str_aligned(c, 64, 24, AlignCenter, AlignTop, buf);
        if(g->new_record) {
            canvas_draw_str_aligned(c, 64, 33, AlignCenter, AlignTop, "Nuovo record!");
        } else {
            snprintf(buf, sizeof(buf), "Record: %04lu", (unsigned long)g->best[g->mode]);
            canvas_draw_str_aligned(c, 64, 33, AlignCenter, AlignTop, buf);
        }
        canvas_draw_str_aligned(c, 64, 43, AlignCenter, AlignTop, "OK:ancora  Back:menu");
        break;
    }

    furi_mutex_release(g->mutex);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

/* ritorna false per uscire dall'app */
static bool handle_input(Game* g, const InputEvent* ev) {
    bool press = ev->type == InputTypePress;
    bool press_or_repeat = press || ev->type == InputTypeRepeat;
    bool short_press = ev->type == InputTypeShort;

    switch(g->screen) {
    case ScreenSplash:
        if(short_press && ev->key == InputKeyBack) {
            return false;
        }
        if(short_press) g->screen = ScreenMenu;
        break;

    case ScreenMenu:
        if(press_or_repeat && ev->key == InputKeyUp) {
            g->menu_sel = (g->menu_sel + 3) % 4;
        } else if(press_or_repeat && ev->key == InputKeyDown) {
            g->menu_sel = (g->menu_sel + 1) % 4;
        } else if(press_or_repeat && (ev->key == InputKeyLeft || ev->key == InputKeyRight)) {
            int delta = ev->key == InputKeyRight ? 1 : -1;
            if(g->menu_sel == 1) {
                g->mode = (Mode)((g->mode + ModeCount + delta) % ModeCount);
            } else if(g->menu_sel == 2) {
                int s = g->speed + delta;
                if(s < 1) s = 9;
                if(s > 9) s = 1;
                g->speed = s;
            } else if(g->menu_sel == 3) {
                g->sound = !g->sound;
                if(g->sound) notification_message(g->notif, &seq_eat);
            }
        } else if(short_press && ev->key == InputKeyOk) {
            if(g->menu_sel == 3) {
                g->sound = !g->sound;
                if(g->sound) notification_message(g->notif, &seq_eat);
            } else {
                game_reset(g);
            }
        } else if(short_press && ev->key == InputKeyBack) {
            save_store(g);
            return false;
        }
        break;

    case ScreenPlay:
        if(press) {
            switch(ev->key) {
            case InputKeyUp:
                queue_dir(g, DirUp);
                break;
            case InputKeyDown:
                queue_dir(g, DirDown);
                break;
            case InputKeyLeft:
                queue_dir(g, DirLeft);
                break;
            case InputKeyRight:
                queue_dir(g, DirRight);
                break;
            default:
                break;
            }
        }
        if(short_press && (ev->key == InputKeyOk || ev->key == InputKeyBack)) {
            g->screen = ScreenPause;
        }
        break;

    case ScreenPause:
        if(short_press && ev->key == InputKeyOk) {
            g->screen = ScreenPlay;
            g->last_step = furi_get_tick();
        } else if(short_press && ev->key == InputKeyBack) {
            game_end(g);
            g->screen = ScreenMenu;
        }
        break;

    case ScreenDying:
        break;

    case ScreenOver:
        if(short_press && ev->key == InputKeyOk) {
            game_reset(g);
        } else if(short_press && ev->key == InputKeyBack) {
            g->screen = ScreenMenu;
        }
        break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int32_t snake_3310_app(void* p) {
    UNUSED(p);

    Game* g = malloc(sizeof(Game));
    memset(g, 0, sizeof(Game));
    g->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    g->notif = furi_record_open(RECORD_NOTIFICATION);
    g->screen = ScreenSplash;
    g->splash_start = furi_get_tick();
    g->mode = ModeBox;
    g->speed = 5;
    g->sound = true;
    save_load(g);

    FuriMessageQueue* queue = furi_message_queue_alloc(16, sizeof(InputEvent));

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, g);
    view_port_input_callback_set(view_port, input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    notification_message(g->notif, &sequence_display_backlight_enforce_on);

    bool running = true;
    while(running) {
        InputEvent ev;
        if(furi_message_queue_get(queue, &ev, 10) == FuriStatusOk) {
            furi_mutex_acquire(g->mutex, FuriWaitForever);
            running = handle_input(g, &ev);
            furi_mutex_release(g->mutex);
        }

        furi_mutex_acquire(g->mutex, FuriWaitForever);
        uint32_t now = furi_get_tick();
        if(g->screen == ScreenSplash) {
            if(now - g->splash_start >= furi_ms_to_ticks(4000)) g->screen = ScreenMenu;
        } else if(g->screen == ScreenPlay) {
            if(now - g->last_step >= furi_ms_to_ticks(step_interval_ms(g->speed))) {
                g->last_step = now;
                game_step(g);
            }
        } else if(g->screen == ScreenDying) {
            if(now - g->last_step >= furi_ms_to_ticks(180)) {
                g->last_step = now;
                if(++g->dying_frame >= 8) {
                    game_end(g);
                    g->screen = ScreenOver;
                }
            }
        }
        furi_mutex_release(g->mutex);

        view_port_update(view_port);
    }

    notification_message(g->notif, &sequence_display_backlight_enforce_auto);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(queue);
    furi_mutex_free(g->mutex);
    free(g);
    return 0;
}
