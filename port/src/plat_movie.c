/*
 * Movies on PC: Sony's libmpeg (sceMpeg*) as the game's movie player uses it (src/sys/movie.c).
 *
 * On the PS2 the game reads the .PSS file itself, hands the bytes to the library to split off the video stream,
 * and asks for one decoded picture per frame. A picture comes back as 16 x 16 blocks of RGBA, columns first, which
 * the game uploads straight into the frame buffer. The sound is a separate ADX file (port/src/gs/snd_adx.c).
 *
 * Here the pictures come from the port's own decoder (plat_mpeg2.c: both movies are intra-only MPEG-2), reading
 * the movie file the game opened last (the path from plat_file.c): each sceMpegGetPicture decodes one picture and
 * rearranges it into the block order. The bytes the game feeds to sceMpegDemuxPss are ignored. Without a window a
 * movie ends at once, except when recorded input is played back (the movie has to take the same number of frames).
 *
 * PS Vita (PORT_VITA): one core decodes about a third of the pictures a second the
 * film has, so the pictures are decoded ahead of time on the other cores (Mpeg2Player, plat_mpeg2.c) and the
 * game's thread only collects the one whose turn it is. The picture then goes straight to the renderer
 * (Vita_MovieFrame): the game's buffer is not filled, and the upload of that buffer into GS memory, which the
 * game still sends every frame, is recognised by its address and not carried out (Vita_MovieDirect).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOVIE_W 512
#define MOVIE_H 448

extern char gPortMoviePath[512];
extern int GsGpu_Enabled(void);

typedef struct Mpeg2 Mpeg2;
extern Mpeg2 *Mpeg2_Open(const char *path);
extern int Mpeg2_Next(Mpeg2 *m, uint8_t *rgba);
extern int Mpeg2_Width(const Mpeg2 *m);
extern int Mpeg2_Height(const Mpeg2 *m);
extern void Mpeg2_Close(Mpeg2 *m);

static Mpeg2 *sMovie;
static int sStarted, sEnd;
static uint8_t sFrame[MOVIE_W * MOVIE_H * 4];

#ifdef PORT_VITA
typedef struct Mpeg2Player Mpeg2Player;
extern Mpeg2Player *Mpeg2Player_Open(const char *path, int workers);
extern int Mpeg2Player_Next(Mpeg2Player *p, const uint8_t **rgba, unsigned patience);
extern int Mpeg2Player_Workers(const Mpeg2Player *p, int priority[2]);
extern int Mpeg2Player_Width(const Mpeg2Player *p);
extern int Mpeg2Player_Height(const Mpeg2Player *p);
extern void Mpeg2Player_Stats(Mpeg2Player *p, unsigned stat[4], unsigned *decodeUs);
extern void Mpeg2Player_Close(Mpeg2Player *p);
extern int gVglSafe; /* vita_vgl.c; 64: the film as before (decoded on the game's thread, uploaded by the game) */
extern void Vita_Log(const char *fmt, ...);

static Mpeg2Player *sPlayer;
static const uint8_t *sPicture;            /* the picture to show (the player's, or sFrame) */
static int sFrameNew;                      /* it has not been taken by the renderer yet */
static const uint8_t *sGameBuf, *sGameEnd; /* the game's picture buffer, while a player runs */

const uint8_t *Vita_MovieFrame(void) { /* the decoded picture (512 x 448, R G B 255), once per picture */
    if (!sFrameNew) {
        return NULL;
    }
    sFrameNew = 0;
    return sPicture;
}

/* 1: `p` is data of the film's picture on its way into GS memory (the renderer has the picture already). */
int Vita_MovieDirect(const void *p) {
    return sPlayer != NULL && (const uint8_t *)p >= sGameBuf && (const uint8_t *)p < sGameEnd;
}

int Vita_MoviePlaying(void) {
    return sPlayer != NULL || sMovie != NULL;
}

/* For the log: 0 when no film is playing in the background. */
int Vita_MovieStats(unsigned stat[4], unsigned *decodeUs) {
    if (sPlayer == NULL) {
        return 0;
    }
    Mpeg2Player_Stats(sPlayer, stat, decodeUs);
    return 1;
}
#endif

static void movie_close(void) {
#ifdef PORT_VITA
    if (sPlayer != NULL) {
        unsigned st[4], us;
        Mpeg2Player_Stats(sPlayer, st, &us);
        Vita_Log("vita: film closed: %u pictures shown, %u decoded too late, %u left out, %u asked for early; %u ms per picture\n",
                 st[0], st[1], st[2], st[3], us / 1000);
        Mpeg2Player_Close(sPlayer);
        sPlayer = NULL;
    }
    sPicture = NULL;
    sFrameNew = 0;
    sGameBuf = sGameEnd = NULL;
#endif
    if (sMovie != NULL) {
        Mpeg2_Close(sMovie);
        sMovie = NULL;
    }
    sStarted = 0;
    sEnd = 0;
}

static void movie_start(void) {
    sStarted = 1;
    sEnd = 1;
    /* without a window too when recorded input is played back: the movie has to take the same number of frames */
    if ((!GsGpu_Enabled() && getenv("BT3_PAD_PLAY") == NULL) || gPortMoviePath[0] == '\0' || getenv("BT3_NOMOVIE") != NULL) {
        return;
    }
#ifdef PORT_VITA
    if (!(gVglSafe & 64)) {
        sPlayer = Mpeg2Player_Open(gPortMoviePath, 3);
        if (sPlayer != NULL) {
            int prio[2], n = Mpeg2Player_Workers(sPlayer, prio);
            Vita_Log("vita: film %s: decoded on %d more threads (priority %d; the game's thread %d)\n", gPortMoviePath, n, prio[1], prio[0]);
            sEnd = 0;
            return;
        }
        Vita_Log("vita: film %s: no player; decoded on the game's thread\n", gPortMoviePath);
    }
#endif
    sMovie = Mpeg2_Open(gPortMoviePath);
    sEnd = sMovie == NULL;
}

int sceMpegInit(void) { return 0; }
int sceMpegCreate(void *mp, void *work, int size) { (void)mp; (void)work; (void)size; movie_close(); return 0; }
int sceMpegDelete(void *mp) { (void)mp; movie_close(); return 0; }
int sceMpegReset(void *mp) { (void)mp; movie_close(); return 0; }
void *sceMpegAddCallback(void *mp, int type, void *cb, void *data) { (void)mp; (void)type; (void)cb; (void)data; return NULL; }
void *sceMpegAddStrCallback(void *mp, int type, int ch, void *cb, void *data) { (void)mp; (void)type; (void)ch; (void)cb; (void)data; return NULL; }
int sceMpegDemuxPss(void *mp, void *data, int size) { (void)mp; (void)data; (void)size; return 0; }

int sceMpegIsEnd(void *mp) {
    (void)mp;
    if (!sStarted) {
        movie_start();
    }
    return sEnd;
}

/* One picture into `rgb`: `blocks` 16 x 16 RGBA blocks of 0x400 bytes, columns first (the order the game's upload
   chain expects); alpha 0x80. */
int sceMpegGetPicture(void *mp, void *rgb, int blocks) {
    uint8_t *out = rgb;
    int cols = MOVIE_W / 16, rows = MOVIE_H / 16, i, j, y, x;

    (void)mp;
    if (!sStarted) {
        movie_start();
    }
#ifdef PORT_VITA
    if (sPlayer != NULL) {
        const uint8_t *pic = NULL;
        /* (the game's frame has time to spare: a picture a worker is just finishing is worth a few milliseconds) */
        int r = Mpeg2Player_Next(sPlayer, &pic, 8000);

        if (r < 0 || (r > 0 && (Mpeg2Player_Width(sPlayer) != MOVIE_W || Mpeg2Player_Height(sPlayer) != MOVIE_H))) {
            sEnd = 1;
            return -1;
        }
        if (r > 0) {
            sPicture = pic;
            sFrameNew = 1;
        }
        sGameBuf = out;
        sGameEnd = out + (size_t)(blocks > 0 ? blocks : 0) * 0x400;
        return 0;
    }
#endif
    if (sMovie == NULL || !Mpeg2_Next(sMovie, sFrame) || Mpeg2_Width(sMovie) != MOVIE_W || Mpeg2_Height(sMovie) != MOVIE_H) {
        sEnd = 1;
        return -1;
    }
#ifdef PORT_VITA /* the renderer takes the picture from here (Vita_MovieFrame), not back out of GS memory */
    {
        extern int gVitaGsSkip;
        if (gVitaGsSkip) {
            return 0; /* stepped over (plat_mpeg2.c): the game's buffer keeps the last picture */
        }
        sPicture = sFrame;
        sFrameNew = 1;
    }
#endif
    for (i = 0; i < cols; i++) {
        for (j = 0; j < rows && i * rows + j < blocks; j++) {
            uint8_t *b = out + (size_t)(i * rows + j) * 0x400;
            for (y = 0; y < 16; y++) {
                const uint8_t *s = &sFrame[((j * 16 + y) * MOVIE_W + i * 16) * 4];
                for (x = 0; x < 16; x++) {
                    b[(y * 16 + x) * 4 + 0] = s[x * 4 + 0];
                    b[(y * 16 + x) * 4 + 1] = s[x * 4 + 1];
                    b[(y * 16 + x) * 4 + 2] = s[x * 4 + 2];
                    b[(y * 16 + x) * 4 + 3] = 0x80;
                }
            }
        }
    }
    return 0;
}
