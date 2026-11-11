#include "common.h"
#include "sys/gfx.h"
#include "sys/gfx_ot.h"
#include "sys/heap.h"

/*
 * Ordering table for depth-sorted primitives (0x102458..0x102F28). Lives for the length of a battle.
 *
 * Memory (Ot_Init): one block of 0x73000 bytes of packet space followed by the table, 0x1002 slots of
 * 0x10 bytes. A slot holds two chains ("layers"), each {head, tail}. Every chain starts with a fixed
 * 0x40-byte head packet (Ot_NewHead) that sets ALPHA_1 / ALPHA_2 for its layer:
 *   layer 0: ALPHA_1 0x44 = (Cs - Cd) * As + Cd (normal),   ALPHA_2 0x42 = (0 - Cs) * As + Cd (subtractive)
 *   layer 1: ALPHA_1 0x48 = (Cs - 0) * As + Cd (additive),  ALPHA_2 0x49 = (Cd - 0) * As + Cd
 * so a primitive picks its blend mode by the layer it is queued on and by drawing with context 1 or 2.
 * gOtFirst is slot 0, gOtZ the 0x1000 depth slots, gOtLast the final slot.
 *
 * Queueing is done by the effect code directly (it uses gOtCur and the three slot pointers; Ot_Add /
 * Ot_AddChain are the out-of-line versions): a packet is a DMA-style header {qwc, next} + GIF data taken
 * from gOtCur, linked at the tail of a chain.
 *
 * Ot_Draw, called once per battle frame after the opaque passes: links all non-empty chains in table
 * order (slot 0 first, then depth slot 0x000 up to 0xFFF, then the last slot; layer 0 before layer 1
 * within a slot), and unless the battle hides them (0x23FBF8) copies their GIF data into the display list
 * between Ot_AddEnv and Ot_AddRestoreEnv. The head packets of empty chains are skipped, those of used
 * chains are sent, which is how the blend mode gets set. Then every chain is emptied.
 * Because the list is drawn in increasing slot order, low slot numbers are the far ones (painter's order);
 * that the index grows towards the camera is an inference from this, the users were not decompiled here.
 *
 * State of the sorted pass (Ot_AddEnv, both GS contexts): FRAME = current buffer, ZBUF with depth writes
 * masked, XYOFFSET default, SCISSOR = the scissor of gBtlCamView (so split-screen halves clip),
 * ALPHA 0x44, TEST = depth test GEQUAL plus 0xC (alpha test method bits GEQUAL, not enabled) | `test`,
 * TEX1 bilinear, CLAMP repeat, FBA off, COLCLAMP on. Afterwards (Ot_AddRestoreEnv, context 1 only apart
 * from ALPHA): ALPHA 0x44, depth writes still masked, depth test GREATER, TEX1 0 (point sampling,
 * as written), COLCLAMP on, CLAMP clamp/clamp.
 */

/* libc */
extern void *memset(void *dst, s32 c, u32 size);
extern void *memcpy(void *dst, const void *src, u32 size);

/* Other modules (local declarations). */
extern s32 BtlStage_IsReady(void);

/* Battle camera view being drawn; only its scissor rectangle is used here. */
typedef struct OtView {
    /* 0x000 */ u8 unk0[0x200];
    /* 0x200 */ s32 scissorX0;
    /* 0x204 */ s32 scissorX1;
    /* 0x208 */ s32 scissorY0;
    /* 0x20C */ s32 scissorY1;
} OtView;
extern OtView *gBtlCamView;

/* Allocates the table, its packet memory and the head packets of both layers. */
void Ot_Init(void) {
    void *buf;

    gOt = Heap_Alloc(sizeof(Ot), 0x20, 0, HEAP_ANY);
    memset(gOt, 0, sizeof(Ot));
    gOt->bufSize = Ot_Align64(0x68FFF);
    gOt->bufSize += 0xA000;
    buf = Heap_Alloc(Ot_Align64(gOt->bufSize + OT_ENTRY_COUNT * sizeof(OtEntry)), 0x20, 0, HEAP_ANY);
    memset(buf, 0, 4);
    Ot_SetBuffer(buf, gOt->bufSize);
    Ot_ResetCursor();
    Ot_InitLayers();
    Ot_ResetEntries(gOtFirst->layer, OT_ENTRY_COUNT);
}

/* Frees everything Ot_Init allocated. */
void Ot_Term(void) {
    Ot_FreeLayers();
    Heap_Free(gOt->buf);
    Heap_Free(gOt);
    gOt = NULL;
}

/* Default blend equations of the two layers (see the table at the top). */
void Ot_SetDefaultAlpha(void) {
    gOt->alpha1[0] = 0x44;
    gOt->alpha1[1] = 0x48;
    gOt->alpha2[0] = 0x42;
    gOt->alpha2[1] = 0x49;
}

/* Rewinds the packet allocation cursor. */
void Ot_ResetCursor(void) {
    gOtCur = gOt->buf;
}

/* Empties the table for the next frame: reselects the three slot pointers, marks every chain empty, rewinds the cursor. Returns 1. */
/* Cleanup pass 2: the two extra copies are made by cse, not written in the source: each of the three loads
   recomputes `cur * 4 + gOt` as its own temporary and cse turns the later ones into copies of the one before
   (`ot = gOt; i = ot->cur; ... ot->slots[i + 1]; ot->slots[i + 2]` reproduces the copy for the third load: 10 of
   24). What is missing is the first load in the same operand order (`ot->slots[i]` expands to `ot + i * 4`, the
   other two to `i * 4 + ot`, so the first is a second addu instead of the source of the chain). Not found:
   `*(i + ot->slots)`, `(&ot->slots[i])[k]`, byte arithmetic, an inline accessor with the index as a parameter,
   pointer copies (a = b = c), post-increments and a 12-byte struct copy all give other code. */
#if 0
/* Differs: the original computes `gOt + gOt->cur * 4` once but then copies it to a new register for each
   of the next two loads (addu v1,a2,v0 / move a3,v1 / move a2,a3), so it is two instructions longer. */
s32 Ot_Reset(void) {
    OtSlot **slots;

    gOt->cur = 0;
    slots = &gOt->slots[gOt->cur];
    gOtFirst = slots[0];
    gOtZ = slots[1];
    gOtLast = slots[2];
    Ot_ResetEntries(gOtFirst->layer, OT_ENTRY_COUNT);
    Ot_ResetCursor();
    return 1;
}
#endif
INCLUDE_ASM("asm/nonmatchings/sys/gfx_ot", Ot_Reset);

/* Marks `count` chains empty (tail = head). */
s32 Ot_ResetEntries(OtEntry *entry, s32 count) {
    OtPrim **tail = &entry->tail;
    s32 i;

    for (i = 0; i < count; i++, tail += 2) {
        *tail = entry->head;
        entry++;
    }
    return 1;
}

/* Changes a layer's ALPHA_1 value and rewrites its head packets. */
void Ot_SetLayerAlpha(s32 layer, u64 *alpha) {
    gOt->alpha1[layer] = *alpha;
    Ot_BuildHeads(layer);
}

/* Appends one packet to a chain. */
void Ot_Add(OtPrim *prim, OtEntry *entry) {
    entry->tail->next = prim;
    entry->tail = prim;
}

/* Appends an already linked run of packets (first..last) to a chain. */
void Ot_AddChain(OtPrim *first, OtPrim *last, OtEntry *entry) {
    entry->tail->next = first;
    entry->tail = last;
}

/* Joins every non-empty chain, in table order, into one list (gOtHead..gOtTail). */
void Ot_Link(OtEntry *entry, s32 count) {
    s32 i;

    gOtHead = NULL;
    gOtTail = NULL;
    for (i = 0; i < count && gOtHead == NULL; i++, entry++) {
        if (entry->tail != entry->head) {
            gOtTail = entry->tail;
            gOtHead = entry->head;
        }
    }
    for (; i < count; i++, entry++) {
        if (entry->tail != entry->head) {
            gOtTail->next = entry->head;
            gOtTail = entry->tail;
        }
    }
}

/* Draws what was queued this frame (unless the battle hides it) and empties the table. */
s32 Ot_Draw(void) {
    Ot_Link(gOtFirst->layer, OT_ENTRY_COUNT);
    if (BtlStage_IsReady()) {
        Ot_Send();
    }
    return Ot_Reset();
}

#ifdef PORT_VITA
/* The sea of Kame House is ~900 triangles a frame, each queued twice in the slot of its depth: once in
   layer 0 (blended over the picture) and once in layer 1 (added). Sent in the table's order, the two blend modes take
   turns slot after slot, and the renderer has to begin a new batch of triangles at every turn (500 a picture, the
   drawing thread's time there). Where the table holds nothing but the sea's triangles and the slots' head packets
   between two other packets, every triangle of layer 0 of that stretch is sent first and then every one of layer 1:
   a surface in one plane covers each pixel once, so no pixel sees its passes in another order (and the additions
   are clamped per pixel as before). Each sea triangle sets its own PRIM and TEX0 (its palette loaded each time, which
   is checked); what the GS holds after the stretch is put back as the original order leaves it: the last head packet
   (the blend of the layer), then PRIM, TEX0, RGBAQ and ST of the stretch's last triangle. bt3.txt searun=0: off. */
int gVitaSeaRun = 1;
int gVitaSeaMark;
unsigned gVitaOtRun[6]; /* (the log) sea packets; stretches put together, their packets; stretches left; other packets after a stretch */
#define OT_SEA_L0 0x5EA00000u
static s32 Ot_IsHead(OtPrim *p) {
    s32 l;
    for (l = 0; l < OT_LAYER_COUNT; l++) {
        if ((u8 *)p >= (u8 *)gOt->headBuf[l] && (u8 *)p < (u8 *)gOt->headBuf[l] + OT_SLOT_COUNT * OT_HEAD_SIZE) {
            return 1 + l;
        }
    }
    return 0;
}
static s32 Ot_IsSea(OtPrim *p) {
    return p->qwc == 7 && (p->vif[0] & ~1u) == OT_SEA_L0 ? 1 + (s32)(p->vif[0] & 1) : 0;
}
static u8 *Ot_Copy(u8 *dst, OtPrim *p) {
    memcpy(dst, p + 1, p->qwc << 4);
    return dst + (p->qwc << 4);
}
extern void Vita_Log(const char *fmt, ...);
extern int snprintf(char *s, unsigned n, const char *fmt, ...);
/* the table's packets in order, by kind, run-length: a = head of layer 0, b = of layer 1, S / s = the
   sea's layer 0 / 1, P / p = another surface's, o<qwc> = anything else */
static void Ot_LogKinds(void) {
    char line[1000];
    s32 len = 0, run = 0, n = 0;
    char last = 0;
    u32 lastQ = 0;
    OtPrim *p;
    for (p = gOtHead; p != NULL; p = p->next) {
        s32 h = Ot_IsHead(p);
        char c = h == 1 ? 'a' : h == 2 ? 'b' : (p->vif[0] & ~1u) == OT_SEA_L0 ? ((p->vif[0] & 1) ? 's' : 'S')
               : (p->vif[0] & ~1u) == 0x5EB00000u ? ((p->vif[0] & 1) ? 'p' : 'P') : 'o';
        n++;
        if (c == last && (c != 'o' || p->qwc == lastQ)) {
            run++;
            continue;
        }
        if (last != 0 && len < (s32)sizeof(line) - 24) {
            len += last == 'o' ? snprintf(line + len, sizeof(line) - (unsigned)len, "o%u", lastQ) : snprintf(line + len, sizeof(line) - (unsigned)len, "%c", last);
            if (run > 1) {
                len += snprintf(line + len, sizeof(line) - (unsigned)len, "%d", run);
            }
            line[len++] = ' ';
            line[len] = 0;
        }
        last = c;
        lastQ = p->qwc;
        run = 1;
        if (len >= (s32)sizeof(line) - 24) {
            Vita_Log("vita:   ot: %s\n", line);
            len = 0;
            line[0] = 0;
        }
    }
    if (last != 0 && len < (s32)sizeof(line) - 24) {
        len += snprintf(line + len, sizeof(line) - (unsigned)len, "%c%d", last, run);
    }
    Vita_Log("vita:   ot: %s (%d packets)\n", line, n);
}
static u8 *Ot_SendRuns(u8 *dst) {
    OtPrim *p = gOtHead, *q, *r, *lastHead, *lastSea, *firstHead[2];
    s32 n[2], ok, k;
    static u32 frames;

    if (++frames == 1500 || frames == 3300) {
        Ot_LogKinds();
    }

    while (p != NULL) {
        if (!Ot_IsHead(p)) {
            if (Ot_IsSea(p)) {
                gVitaOtRun[0]++;
            }
            dst = Ot_Copy(dst, p);
            p = p->next;
            continue;
        }
        /* a stretch from this head: heads and sea triangles only */
        n[0] = n[1] = 0;
        firstHead[0] = firstHead[1] = NULL;
        lastHead = NULL;
        lastSea = NULL;
        ok = 1;
        for (q = p; q != NULL; q = q->next) {
            s32 h = Ot_IsHead(q), sea = h ? 0 : Ot_IsSea(q);
            if (h) {
                if (firstHead[h - 1] == NULL) {
                    firstHead[h - 1] = q;
                }
                lastHead = q;
            } else if (sea) {
                const u64 *g = (const u64 *)(q + 1);
                n[sea - 1]++;
                lastSea = q;
                if ((g[3] >> 61) != 1) {
                    ok = 0; /* (a palette not loaded by the triangle itself: the order would matter) */
                }
            } else {
                break;
            }
        }
        gVitaOtRun[0] += n[0] + n[1];
        if (q != NULL) {
            gVitaOtRun[4]++;
            if ((q->vif[0] & ~1u) == 0x5EB00000u) {
                gVitaOtRun[5]++; /* (another surface's triangle) */
            }
        }
        if (n[0] + n[1] < 4 || n[0] == 0 || n[1] == 0 || !ok || firstHead[0] == NULL || firstHead[1] == NULL) {
            if (n[0] + n[1] >= 4 && !ok) {
                gVitaOtRun[3]++;
            }
            for (r = p; r != q; r = r->next) {
                dst = Ot_Copy(dst, r);
            }
            p = q;
            continue;
        }
        gVitaOtRun[1]++;
        gVitaOtRun[2] += n[0] + n[1];
        for (k = 0; k < 2; k++) {
            dst = Ot_Copy(dst, firstHead[k]);
            for (r = p; r != q; r = r->next) {
                if (Ot_IsSea(r) == k + 1) {
                    dst = Ot_Copy(dst, r);
                }
            }
        }
        dst = Ot_Copy(dst, lastHead);
        {   /* the stretch's last triangle's PRIM, TEX0, RGBAQ and ST */
            const u64 *g = (const u64 *)(lastSea + 1); /* tag, regs, PRIM, TEX0, then 3 x (RGBAQ, ST, XYZF2) */
            u64 *o = (u64 *)dst;
            o[0] = 0x1000000000000004ull;     /* GIF tag: NLOOP 4, NREG 1, PACKED (A+D) */
            o[1] = 0xE;                       /* A+D */
            o[2] = g[2]; o[3] = 0x00;          /* PRIM */
            o[4] = g[3]; o[5] = (g[1] >> 4) & 0xF; /* TEX0_1 or TEX0_2, as the triangle has it */
            o[6] = g[10]; o[7] = 0x01;         /* RGBAQ of its third corner */
            o[8] = g[11]; o[9] = 0x02;         /* ST */
            dst += 5 * 16;
        }
        p = q;
    }
    return dst;
}
#endif

/* Copies the GIF data of every linked packet into the display list, between the sorted-pass state and its restore. */
s32 Ot_Send(void) {
    OtPrim *prim;
    u8 *dst;

    if (gOtTail == NULL) {
        return 0;
    }
    Ot_AddEnv(0);
    dst = (u8 *)Dma_BeginDirect();
#ifdef PORT_VITA
    if (gVitaSeaRun) {
        dst = Ot_SendRuns(dst);
    } else
#endif
    for (prim = gOtHead; prim != NULL; prim = prim->next) {
        memcpy(dst, prim + 1, prim->qwc << 4);
        dst += prim->qwc << 4;
    }
    Dma_EndDirect((u64 *)dst);
    Ot_AddRestoreEnv();
    return 1;
}

/* Queues the GS state of the sorted pass on both contexts: current buffer, view scissor, depth test without depth writes. */
void Ot_AddEnv(s32 test) {
    s32 tst = test | 0x5000C;
    u32 pkt[92] = {
        DMA_TAG_CNT | 22, 0, VIF_FLUSHE, VIF_DIRECT | 22,
        GIF_EOP | 21, 0x10000000, GIF_REG_AD, 0,
        GFX_FRAME_REG(), 0, GS_FRAME_1, 0,
        GFX_FRAME_REG(), 0, GS_FRAME_2, 0,
        GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 0), 0, GS_ZBUF_1, 0,
        GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 0), 0, GS_ZBUF_2, 0,
        GFX_OFX, GFX_OFY, GS_XYOFFSET_1, 0,
        GFX_OFX, GFX_OFY, GS_XYOFFSET_2, 0,
        GS_SET_SCISSOR(gBtlCamView->scissorX0, gBtlCamView->scissorX1, gBtlCamView->scissorY0, gBtlCamView->scissorY1),
        GS_SET_SCISSOR(gBtlCamView->scissorX0, gBtlCamView->scissorX1, gBtlCamView->scissorY0, gBtlCamView->scissorY1) >> 32, GS_SCISSOR_1, 0,
        GS_SET_SCISSOR(gBtlCamView->scissorX0, gBtlCamView->scissorX1, gBtlCamView->scissorY0, gBtlCamView->scissorY1),
        GS_SET_SCISSOR(gBtlCamView->scissorX0, gBtlCamView->scissorX1, gBtlCamView->scissorY0, gBtlCamView->scissorY1) >> 32, GS_SCISSOR_2, 0,
        0x44, 0, GS_ALPHA_1, 0,
        0x44, 0, GS_ALPHA_2, 0,
        GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 0), 1, GS_ZBUF_1, 0,
        GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 0), 1, GS_ZBUF_2, 0,
        tst, (u64)tst >> 32, GS_TEST_1, 0,
        tst, (u64)tst >> 32, GS_TEST_2, 0,
        0x60, 0, GS_TEX1_1, 0,
        0x60, 0, GS_TEX1_1 + 1, 0,
        0, 0, GS_CLAMP_1, 0,
        0, 0, GS_CLAMP_2, 0,
        0, 0, GS_FBA_1, 0,
        0, 0, GS_FBA_2, 0,
        1, 0, GS_COLCLAMP, 0,
    };

    Dma_AddData(pkt, sizeof(pkt));
}

/* Queues the state that follows the sorted pass: normal blending, depth writes off, depth test "greater", point sampling. */
void Ot_AddRestoreEnv(void) {
    u32 pkt[36] = {
        DMA_TAG_CNT | 8, 0, VIF_FLUSHE, VIF_DIRECT | 8,
        GIF_EOP | 7, 0x10000000, GIF_REG_AD, 0,
        0x44, 0, GS_ALPHA_1, 0,
        0x44, 0, GS_ALPHA_2, 0,
        GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 0), 1, GS_ZBUF_1, 0,
        0x70000, 0, GS_TEST_1, 0,
        0, 0, GS_TEX1_1, 0,
        1, 0, GS_COLCLAMP, 0,
        5, 0, GS_CLAMP_1, 0,
    };

    Dma_AddData(pkt, sizeof(pkt));
}

/* Always 0. */
s32 Ot_Stub102C60(void) {
    return 0;
}

/* Always 0. */
s32 Ot_Stub102C68(void) {
    return 0;
}

/* Returns the usable size of the packet memory (0 before Ot_Init). */
s32 Ot_GetCapacity(void) {
    if (gOtCur == NULL) {
        return 0;
    }
    if (gOt == NULL) {
        return 0;
    }
    if (gOt->buf == NULL) {
        return 0;
    }
    return gOt->bufSize - 0x10;
}

/* Records the packet memory and places the table right after its first `size` bytes. */
void Ot_SetBuffer(void *buf, s32 size) {
    gOt->buf = buf;
    gOt->slots[0] = (OtSlot *)((u8 *)buf + size);
    gOt->slots[1] = gOt->slots[0] + 1;
    gOt->slots[2] = gOt->slots[1] + OT_Z_SLOTS;
    gOtFirst = gOt->slots[0];
    gOtZ = gOt->slots[1];
    gOtLast = gOt->slots[2];
}

/* Allocates and fills the head packets of both layers. */
void Ot_InitLayers(void) {
    s32 i;

    Ot_SetDefaultAlpha();
    for (i = 0; i < OT_LAYER_COUNT; i++) {
        gOt->headBuf[i] = Heap_Alloc(OT_SLOT_COUNT * OT_HEAD_SIZE, 0x20, 0, HEAP_ANY);
        memset(gOt->headBuf[i], 0, OT_SLOT_COUNT * OT_HEAD_SIZE);
        Ot_BuildHeads(i);
    }
}

/* Frees the head packets of both layers. */
void Ot_FreeLayers(void) {
    s32 i;

    for (i = 0; i < OT_LAYER_COUNT; i++) {
        if (gOt->headBuf[i] != NULL) {
            Heap_Free(gOt->headBuf[i]);
            gOt->headBuf[i] = NULL;
        }
    }
}

/* Writes one head packet at the cursor: a chain link plus the layer's ALPHA_1 / ALPHA_2 values. */
OtPrim *Ot_NewHead(s32 layer) {
    u32 *p = gOtCur;

    gOtCur = p + OT_HEAD_SIZE / 4;
    p[0] = DMA_TAG_NEXT | 3;
    p[2] = VIF_FLUSHE;
    p[3] = VIF_DIRECT | 3;
    ((u64 *)p)[2] = GIF_TAG(2, 1, 1);
    ((u64 *)p)[3] = GIF_REG_AD;
    ((u64 *)p)[5] = GS_ALPHA_1;
    ((u64 *)p)[7] = GS_ALPHA_2;
    p[1] = 0;
    ((u64 *)p)[4] = gOt->alpha1[layer];
    ((u64 *)p)[6] = gOt->alpha2[layer];
    return (OtPrim *)p;
}

/* (Re)writes the head packet of every slot of one layer and points the table at them. */
void Ot_BuildHeads(s32 layer) {
    u32 *saved = gOtCur;
    OtSlot *slot;
    s32 i;

    gOtCur = gOt->headBuf[layer];
    slot = gOt->slots[0];
    for (i = 0; i < OT_SLOT_COUNT; i++) {
        slot->layer[layer].head = Ot_NewHead(layer);
        slot++;
    }
    gOtCur = saved;
}

/* Rounds a size up to a multiple of 64. */
s32 Ot_Align64(s32 size) {
    if (size & 0x3F) {
        size = size / 64 * 64 + 64;
    }
    return size;
}
