/* Game state the platform layer needs: in a battle, in the fight itself (as port/vita/vita_main.c). */
#include <stddef.h>
#include "battle/btl_seq.h"

int Vita_InFight(void) { return gBtlSeq != NULL && gBtlSeq->state == 3; }
int Vita_InBattle(void) { return gBtlSeq != NULL && gBtlSeq->state >= 0 && gBtlSeq->state <= 6; }
