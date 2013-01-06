/* This fill add persistence of peers and torrents in memory on the fly.
 * Author: FengGu <flygoast@126.com>
 *
 * This software was written by Dirk Engling <erdgeist@erdgeist.org>
 *    It is considered beerware. Prost. Skol. Cheers or whatever.
 *
 *       $id$ */

#ifndef __OT_PERSIST_H_
#define __OT_PERSIST_H_

#ifdef WANT_PERSISTENCE

#ifdef WANT_V6
#error "Persist don't surport IPV6"
#endif

extern char *g_persistfile;

typedef enum {
    PMODE_NULL = 0x00,
    PMODE_DUMP = 0x01,
} ot_pmode;

void persist_init();
void persist_deinit();
int persist_load_file();
void persist_change(struct ot_workstruct *ws);
int persist_set_mode(char *value);
void persist_append_save_param(time_t seconds, int changes);

#else

#define persist_init()
#define persist_deinit()

#endif /* WANT_PERSISTENCE */

#endif /* __OT_PERSIST_H_ */
