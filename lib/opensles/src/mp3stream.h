/* mp3stream.h -- MP3 streaming decode for OpenSL ES ANDROIDFD audio players.
 *
 * TFA plays music/streamed audio by creating an OpenSL AudioPlayer with an
 * SL_DATALOCATOR_ANDROIDFD source: it hands us a file descriptor + (offset,
 * length) into the OBB and expects the platform to decode the MP3. This module
 * reads that MP3 region, decodes it with minimp3, and feeds the resulting PCM
 * into the player's buffer queue so the existing mixer plays it.
 *
 * NOTE: do not include sles_allinclusive.h here -- the OpenSL sources include it
 * (guard-less) before this header, so we only forward-declare CAudioPlayer.
 */
#ifndef MP3STREAM_H
#define MP3STREAM_H

#include <SLES/OpenSLES.h>   /* SLresult */

struct CAudioPlayer_struct;

/* number of buffer-queue slots reserved for a streamed MP3 player */
#define MP3STREAM_NUM_BUFFERS 6

/* Attach an MP3 decoder to a freshly-constructed ANDROIDFD audio player. Reads
 * the MP3 from [fd @ offset, length], probes its format, and configures the
 * player's buffer queue. Does NOT enqueue yet (see mp3stream_prime). On success
 * the decoder is stored in ap->mBufferQueue.mContext. Returns SL_RESULT_SUCCESS
 * on success; on failure the player is left silent (caller should not fail). */
SLresult mp3stream_setup(struct CAudioPlayer_struct *ap, int fd, long long offset, long long length);

/* Enqueue the initial buffers (call once the player object is published). */
void mp3stream_prime(struct CAudioPlayer_struct *ap);

/* Free the decoder attached to an ANDROIDFD player (call from Destroy, after the
 * mixer track has been unlinked in PreDestroy). */
void mp3stream_destroy(struct CAudioPlayer_struct *ap);

#endif
