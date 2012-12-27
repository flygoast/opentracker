/* This file add persistence of peers and torrents in memory on the fly.
 * Author: FengGu <flygoast@126.com>
 *
 * This software was written by Dirk Engling <erdgeist@erdgeist.org>
 It is considered beerware. Prost. Skol. Cheers or whatever.

 $id$ */

/* System */
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>
#include <inttypes.h>
#include <assert.h>
#ifdef WANT_SYSLOGS
#include <syslog.h>
#endif

/* Libowfat */
#include "byte.h"
#include "io.h"
#include "ip4.h"
#include "ip6.h"

/* Opentracker */
#include "trackerlogic.h"
#include "ot_mutex.h"
#include "ot_accesslist.h"
#include "ot_persist.h"

#ifdef PERSIST_LOGGING
#define LOG_ERR( ... ) fprintf( stderr, __VA_ARGS__ )
#else
#define LOG_ERR( ... )
#endif /* NO_PERSIST_LOGGING */

/* Just for file corruption checking  */
#define OT_DUMP_TORRENT     0xfe
#define OT_DUMP_EOF         0xff

#define OT_DUMP_IDENTI              "OPENTRACKER"
#define OT_DUMP_IDENTI_LEN          (sizeof(OT_DUMP_IDENTI) - 1)
#define OT_DUMP_VERSION             "0001"
#define OT_DUMP_VERSION_LEN         (sizeof(OT_DUMP_VERSION) - 1)
#define OT_DUMP_IDENTI_VERSION      (OT_DUMP_IDENTI OT_DUMP_VERSION)
#define OT_DUMP_IDENTI_VERSION_LEN  (sizeof(OT_DUMP_IDENTI_VERSION) - 1)

typedef struct dump_saveparam {
    time_t  seconds;
    int     changes;
} dump_saveparam_t;

char *  g_persistfile;
int     g_persistmode = PMODE_NULL;
static int     dump_dirty;
static int     dump_lastsave;
static size_t  saveparam_len;
static dump_saveparam_t *saveparams;

static int persist_add_peer(ot_hash *hash, ot_peerlist *peer_list, ot_peer *peer) {
  int         exactmatch, delta_torrentcount = 0;
  ot_torrent *torrent;
  ot_peer    *peer_dest;

  ot_vector  *torrents_list = mutex_bucket_lock_by_hash(*hash);

  if( !accesslist_hashisvalid( hash ) ) {
    mutex_bucket_unlock_by_hash( *hash, 0 );
    return 0;
  }

  torrent = vector_find_or_insert( torrents_list, (void*)hash, sizeof( ot_torrent ), OT_HASH_COMPARE_SIZE, &exactmatch );
  if( !torrent ) {
    mutex_bucket_unlock_by_hash( *hash, 0 );
    return 0;
  }

  if( !exactmatch ) {
    /* Create a new torrent entry, then */
    memcpy( torrent->hash, hash, sizeof(ot_hash) );

    if( !( torrent->peer_list = malloc( sizeof (ot_peerlist) ) ) ) {
      vector_remove_torrent( torrents_list, torrent );
      mutex_bucket_unlock_by_hash( *hash, 0 );
      return 0;
    }

    byte_zero( torrent->peer_list, sizeof( ot_peerlist ) );
    delta_torrentcount = 1;
  }

  torrent->peer_list->base = peer_list->base;

  /* Check for peer in torrent */
  peer_dest = vector_find_or_insert_peer( &(torrent->peer_list->peers), peer, &exactmatch );
  if( !peer_dest ) {
    mutex_bucket_unlock_by_hash( *hash, delta_torrentcount );
    return 0;
  }

  /* If we hadn't had a match create peer there */
  if( !exactmatch ) {
    torrent->peer_list->peer_count++;
    if( OT_PEERFLAG(peer) & PEER_FLAG_COMPLETED ) {
      torrent->peer_list->down_count++;
    }
    if( OT_PEERFLAG(peer) & PEER_FLAG_SEEDING )
      torrent->peer_list->seed_count++;
  } else {
    LOG_ERR("You should get here!\n");
    assert(0);
  }

  memcpy( peer_dest, peer, sizeof(ot_peer) );

  mutex_bucket_unlock_by_hash( *hash, delta_torrentcount );
  return 0;
}

static int persist_load_peers(FILE *fp, ot_hash *hash, ot_peerlist *peer_list) {
  unsigned int count;
  unsigned int i;
  ot_peer  peer;

  if (fread(&count, sizeof(unsigned int), 1, fp) == 0) goto rerr;
  if (count == 0) return 0;

  for (i = 0; i < count; ++i) {
    if (fread(&peer, sizeof(ot_peer), 1, fp) != sizeof(ot_peer)) goto rerr;
    if (persist_add_peer(hash, peer_list, &peer) < 0) {
      LOG_ERR("persist_add_peer failed\n");
      return -1;
    }
  }
  return 0;

rerr:
  LOG_ERR("%s\n", strerror(errno));
  return -1;
}

static int persist_load_torrent(FILE *fp) {
  ot_hash       hash;
  ot_peerlist   peer_list;

  /* load torrent hash */
  if (fread(&hash, sizeof(ot_hash), 1, fp) != sizeof(ot_hash)) goto rerr;

  /*
   * load peer_list data:
   *
   * struct ot_peerlist {
   *   ot_time        base;
   *   size_t         seed_count;
   *   size_t         peer_count;
   *   size_t         down_count;
   *   ot_vector      peers;
   * }
   *
   */
  if (fread(&peer_list.base, sizeof(ot_time), 1, fp) != sizeof(ot_time)) goto rerr;
  if (fread(&peer_list.seed_count, sizeof(ot_time), 1, fp) != sizeof(ot_time)) goto rerr;
  if (fread(&peer_list.peer_count, sizeof(ot_time), 1, fp) != sizeof(ot_time)) goto rerr;
  if (fread(&peer_list.down_count, sizeof(ot_time), 1, fp) != sizeof(ot_time)) goto rerr;
  if (persist_load_peers(fp, &hash, &peer_list) < 0) goto rerr;

  return 0;

rerr:
  LOG_ERR("%s\n", strerror(errno));
  return -1;
}

int persist_load_file() {
  FILE     *fp;
  uint8_t   buf[1024];
  int       version;

  if (!g_persistfile) {
    g_persistfile = strdup("opentracker.odb");
  }

  fp = fopen(g_persistfile, "r");
  if (!fp) {
    LOG_ERR("%s\n", strerror(errno));
    return 0;
  }

  if (fread(buf, OT_DUMP_IDENTI_LEN, 1, fp) == 0) {
    LOG_ERR("%s\n", strerror(errno));
    goto rerr;
  }

  buf[OT_DUMP_IDENTI_LEN] = '\0';
  if (memcmp(buf, OT_DUMP_IDENTI, OT_DUMP_IDENTI_LEN) != 0) {
    LOG_ERR("%s\n", strerror(errno));
    goto rerr;
  }

  version = atoi((const char *)buf + OT_DUMP_IDENTI_LEN);
  if (version != 1) {
    LOG_ERR("Can't handle ODB format version %d\n", version);
    goto rerr;
  }

  for ( ; ; ) {
    if (fread(buf, 1, 1, fp) == 0) goto rerr;
    if (buf[0] != OT_DUMP_TORRENT && buf[0] != OT_DUMP_EOF) {
      LOG_ERR("ODB file corrupted\n");
      goto rerr;
    }

    if (buf[0] == OT_DUMP_EOF) {
      break;
    }
    if (persist_load_torrent(fp) < 0) goto rerr;
  }

  LOG_ERR("Load ODB file success\n");
  fclose(fp);
  return 0;

rerr:
  fclose(fp);
  return -1;
}

void persist_change(struct ot_workstruct *ws) {
  (void)ws; /* In "dump" mode, don't use this param */
  switch (g_persistmode) {
  case PMODE_NULL:
    return;
  case PMODE_DUMP:
    ++dump_dirty;
    break;
  default:
    assert(0);
    break;
  }
  return;
}

void persist_append_save_param(time_t seconds, int changes) {
  saveparams = realloc(saveparams, sizeof(dump_saveparam_t) * (saveparam_len + 1));
  if (!saveparams) {
    LOG_ERR("out of memory\n");
    exit(123);
  }

  saveparams[saveparam_len].seconds = seconds;
  saveparams[saveparam_len].changes = changes;
  ++saveparam_len;
}

static int persist_dump_peers(ot_peerlist *peer_list, FILE *fp ) {
  unsigned int bucket, num_buckets = 1;
  ot_vector    *bucket_list = &peer_list->peers;
  unsigned int count = 0;

  if( OT_PEERLIST_HASBUCKETS(peer_list) ) {
    num_buckets = bucket_list->size;
    bucket_list = (ot_vector *)bucket_list->data;
  }

  /* write peers count */
  for (bucket = 0; bucket < num_buckets; ++bucket) {
    count += bucket_list[bucket].size;
  }
  if (fwrite(&count, sizeof(unsigned int), 1, fp) == 0) goto werr;

  for (bucket = 0; bucket < num_buckets; ++bucket) {
    ot_peer *peers = (ot_peer*)bucket_list[bucket].data;
    size_t  peer_count = bucket_list[bucket].size;

    while( peer_count-- ) {
      if (fwrite(peers, sizeof(ot_peer), 1, fp) == 0) goto werr;
    }
  }
  return 0;

werr:
  return -1;
}

static int persist_dump_torrent(ot_torrent* torrent, FILE *fp ) {
  uint8_t c;
  ot_peerlist *peer_list = torrent->peer_list;
  ot_hash     *hash = &torrent->hash;

  /* Write TORRENT opcode */
  c = OT_DUMP_TORRENT;
  if (fwrite(&c, 1, 1, fp) != 1) goto werr;

  /* Write torrent hash */
  if (fwrite(hash, sizeof(ot_hash), 1, fp) == 0) goto werr;

  /*
   * write peer_list data:
   *
   * struct ot_peerlist {
   *   ot_time        base;
   *   size_t         seed_count;
   *   size_t         peer_count;
   *   size_t         down_count;
   *   ot_vector      peers;
   * }
   *
   */
  if (fwrite(&peer_list->base, sizeof(ot_time), 1, fp) == 0) goto werr;
  if (fwrite(&peer_list->seed_count, sizeof(size_t), 1, fp) == 0) goto werr;
  if (fwrite(&peer_list->peer_count, sizeof(size_t), 1, fp) == 0) goto werr;
  if (fwrite(&peer_list->down_count, sizeof(size_t), 1, fp) == 0) goto werr;
  if (persist_dump_peers(peer_list, fp) < 0) goto werr;

  return 0;

werr:
  return -1;
}

static int persist_dump_make() {
  int bucket;
  size_t j;
  uint8_t c;
  FILE *fp;
  char tmpfile[256];

  snprintf(tmpfile, 256, "temp-%u.otdb", (unsigned int)g_now_seconds);

  fp = fopen(tmpfile, "w");
  if (!fp) return -1;

  /* write identifier and version */
  if (fwrite(OT_DUMP_IDENTI_VERSION, OT_DUMP_IDENTI_VERSION_LEN, 1, fp) == 0) goto werr;

  /* Dump torrents and peers */
  for(bucket=0; bucket < OT_BUCKET_COUNT; ++bucket ) {
    ot_vector  *torrents_list = mutex_bucket_lock( bucket );
    ot_torrent *torrents = (ot_torrent*)(torrents_list->data);

    for( j=0; j < torrents_list->size; ++j )
      if( persist_dump_torrent( torrents + j, fp ) < 0 )
        goto werr;

    mutex_bucket_unlock( bucket, 0 );
    if( !g_opentracker_running ) goto werr;
  }

  /* EOF opcode */
  c = OT_DUMP_EOF;
  if (fwrite(&c, 1, 1, fp) != 1) goto werr;

  /* Make sure data will not remain on the OS's output buffers. */
  fflush(fp);
  fsync(fileno(fp));
  fclose(fp);

  /* Use RENAME to make sure the dump file is changed atomically 
   * only if the generate dump file is ok. */
  if (!g_persistfile) {
      g_persistfile = strdup("opentracker.odb");
  }

  if (rename(tmpfile, g_persistfile) < 0) {
    unlink(tmpfile);
    return -1;
  }

  dump_dirty = 0;
  dump_lastsave = g_now_seconds;
  return 0;

werr:
  fclose(fp);
  unlink(tmpfile);
  return -1;
}

int persist_set_mode(char *value) {
  while( isspace(*value) ) ++value;
  if (!strcmp(value, "null")) {
    g_persistmode = PMODE_NULL;
  } else if (!strcmp(value, "dump")) {
    g_persistmode = PMODE_DUMP;
  } else {
    return -1;
  }
  return 0;
}

static void * persist_worker( void * args ) {
  size_t i = 0;
  (void)args;

  while (1) {
    if (g_persistmode == PMODE_DUMP) {
      for (i = 0; i < saveparam_len; ++i) {
        dump_saveparam_t *sp = saveparams + i;
        if (dump_dirty >= sp->changes && g_now_seconds - dump_lastsave > sp->seconds) {
          persist_dump_make();
        }
      }
      if( !g_opentracker_running ) return NULL;
      sleep(1);
    } else {
      return NULL;
    }
  }
}

static pthread_t thread_id;
void persist_init( ) {
  if (g_persistmode != PMODE_NULL) 
    pthread_create( &thread_id, NULL, persist_worker, NULL );
}

void persist_deinit( ) {
  if (g_persistmode != PMODE_NULL)
    pthread_cancel( thread_id );
  if (saveparams) free(saveparams);
  if (g_persistfile) free(g_persistfile);
}

const char *g_version_persist_c = "$Source: ot_persist.c Added by FengGu <flygoast@126.com>,v $: $Revision: 0.01 $\n";
