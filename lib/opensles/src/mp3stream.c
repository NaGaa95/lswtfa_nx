/* mp3stream.c -- MP3 streaming decode for OpenSL ES ANDROIDFD players (TFA music)
 *
 * See mp3stream.h. Decoding runs in the buffer-queue completion callback (the
 * mixer thread), the same place the engine would refill a PCM buffer queue, so
 * no extra thread is needed. minimp3 is fast enough to decode a buffer's worth
 * of frames well within an audio period.
 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"

#include "sles_allinclusive.h"
#include "mp3stream.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int debugPrintf(char *text, ...);
// internal buffer-queue enqueue (declared in IBufferQueue.c)
extern SLresult IBufferQueue_Enqueue(SLBufferQueueItf self, const void *pBuffer, SLuint32 size);

// the SDL output rate (mirrors the static helper in IBufferQueue.c)
static int mp3_out_rate_hz(void)
{
    return (&_opensles_user_freq != NULL && _opensles_user_freq > 0) ? _opensles_user_freq : 44100;
}

// ~8 MP3 frames per enqueued buffer (~0.2s at 44.1 kHz); 6-slot ring keeps a few
// buffers in flight without ever reusing one that is still queued.
#define FRAMES_PER_BUF 8
#define BUF_INT16      (FRAMES_PER_BUF * MINIMP3_MAX_SAMPLES_PER_FRAME)
#define RING           MP3STREAM_NUM_BUFFERS

typedef struct {
    mp3dec_t  dec;
    uint8_t  *data;      // whole MP3 region in memory
    size_t    size;
    size_t    pos;       // decode cursor
    int       hz, channels;
    int       eof;
    int16_t  *ring[RING];
    int       ring_idx;
    CAudioPlayer *ap;
} Mp3Stream;

static int is_mp3_player(CAudioPlayer *ap)
{
    return ap != NULL &&
           ap->mDataSource.mLocator.mLocatorType == SL_DATALOCATOR_ANDROIDFD &&
           ap->mBufferQueue.mContext != NULL;
}

// read [fd @ offset, length] fully into a fresh buffer; returns bytes read (<=0 on error)
static long read_region(int fd, long long offset, long long length, uint8_t **out)
{
    *out = NULL;
    int dfd = dup(fd);                 // own file position, leave the engine's fd alone
    if (dfd < 0)
        return -1;
    if (lseek(dfd, (off_t)offset, SEEK_SET) == (off_t)-1) { close(dfd); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)length);
    if (buf == NULL) { close(dfd); return -1; }
    size_t got = 0;
    while (got < (size_t)length) {
        ssize_t n = read(dfd, buf + got, (size_t)length - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(dfd);
    if (got == 0) { free(buf); return -1; }
    *out = buf;
    return (long)got;
}

// decode one buffer's worth of frames into the next ring slot and enqueue it
static void fill_one(Mp3Stream *s)
{
    if (s->eof)
        return;
    // honour the engine's loop request (SLSeekItf SetLoop): music loops, one-shot
    // streamed sounds play once and finish so the engine isn't left waiting.
    const int loop = s->ap->mSeek.mLoopEnabled ? 1 : 0;
    int16_t *out = s->ring[s->ring_idx];
    s->ring_idx = (s->ring_idx + 1) % RING;

    int written = 0; // int16 samples written so far
    while (written + MINIMP3_MAX_SAMPLES_PER_FRAME <= BUF_INT16) {
        if (s->pos >= s->size) {
            if (loop) { s->pos = 0; mp3dec_init(&s->dec); }
            else      { s->eof = 1; break; }
        }
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s->dec, s->data + s->pos,
                                          (int)(s->size - s->pos), out + written, &info);
        if (info.frame_bytes == 0) { s->eof = 1; break; } // no more frames
        s->pos += (size_t)info.frame_bytes;
        if (samples > 0)
            written += samples * info.channels;
    }
    if (written > 0)
        IBufferQueue_Enqueue((SLBufferQueueItf)&s->ap->mBufferQueue, out,
                             (SLuint32)(written * (int)sizeof(int16_t)));
}

// buffer-queue completion callback: a buffer drained -> decode + enqueue another
static void mp3_bq_callback(SLBufferQueueItf caller, void *context)
{
    (void)caller;
    Mp3Stream *s = (Mp3Stream *)context;
    if (s != NULL)
        fill_one(s);
}

SLresult mp3stream_setup(CAudioPlayer *ap, int fd, long long offset, long long length)
{
    if (ap == NULL || fd < 0 || length <= 0)
        return SL_RESULT_PARAMETER_INVALID;

    Mp3Stream *s = (Mp3Stream *)calloc(1, sizeof(Mp3Stream));
    if (s == NULL)
        return SL_RESULT_MEMORY_FAILURE;
    s->ap = ap;

    long got = read_region(fd, offset, length, &s->data);
    if (got <= 0) { free(s); return SL_RESULT_IO_ERROR; }
    s->size = (size_t)got;

    // probe the first decodable frame to learn sample rate / channel count
    mp3dec_init(&s->dec);
    {
        int16_t probe[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info;
        size_t p = 0;
        int samples = 0;
        memset(&info, 0, sizeof(info));
        while (p < s->size) {
            samples = mp3dec_decode_frame(&s->dec, s->data + p, (int)(s->size - p), probe, &info);
            if (info.frame_bytes == 0) break;
            p += (size_t)info.frame_bytes;
            if (samples > 0) break;
        }
        if (samples <= 0 || info.hz <= 0 || (info.channels != 1 && info.channels != 2)) {
            free(s->data); free(s);
            return SL_RESULT_CONTENT_UNSUPPORTED;
        }
        s->hz = info.hz;
        s->channels = info.channels;
    }

    // allocate the PCM ring; restart the decoder at the top for playback
    for (int i = 0; i < RING; i++) {
        s->ring[i] = (int16_t *)malloc((size_t)BUF_INT16 * sizeof(int16_t));
        if (s->ring[i] == NULL) {
            for (int j = 0; j < i; j++) free(s->ring[j]);
            free(s->data); free(s);
            return SL_RESULT_MEMORY_FAILURE;
        }
    }
    s->pos = 0;
    mp3dec_init(&s->dec);

    // The engine does NOT expose SL_IID_BUFFERQUEUE for a file (ANDROIDFD)
    // source, so construct() never initialised this interface -- its mThis (used
    // by interface_lock_exclusive -> object's mutex) is NULL. Initialise the
    // fields the mixer + Enqueue rely on; mNumBuffers/mArray/mFront/mRear were
    // already set up in IEngine_CreateAudioPlayer.
    ap->mBufferQueue.mThis           = (IObject *)&ap->mObject;
    ap->mBufferQueue.mState.count    = 0;
    ap->mBufferQueue.mState.playIndex = 0;
    ap->mBufferQueue.mClearRequested = SL_BOOLEAN_FALSE;

    // configure the buffer queue: enqueued PCM is at the MP3's native rate/channels;
    // IBufferQueue_Enqueue resamples/upmixes to the 44.1 kHz stereo output.
    ap->mBufferQueue.samplerate = (uint32_t)s->hz * 1000u;
    ap->mBufferQueue.channels   = s->channels;
    ap->mBufferQueue.bps        = 16;
    ap->mBufferQueue.mCallback  = mp3_bq_callback;
    ap->mBufferQueue.mContext   = s;
    ap->mNumChannels       = 2;
    ap->mSampleRateMilliHz = (SLuint32)mp3_out_rate_hz() * 1000u;

    debugPrintf("SL: mp3stream %d Hz %dch, %u bytes -> streaming\n",
                s->hz, s->channels, (unsigned)s->size);
    return SL_RESULT_SUCCESS;
}

void mp3stream_prime(CAudioPlayer *ap)
{
    if (!is_mp3_player(ap))
        return;
    Mp3Stream *s = (Mp3Stream *)ap->mBufferQueue.mContext;
    for (int i = 0; i < 3 && !s->eof; i++)  // pre-fill so playback starts with data
        fill_one(s);
}

void mp3stream_destroy(CAudioPlayer *ap)
{
    if (!is_mp3_player(ap))
        return;
    Mp3Stream *s = (Mp3Stream *)ap->mBufferQueue.mContext;
    ap->mBufferQueue.mContext  = NULL;
    ap->mBufferQueue.mCallback = NULL;
    for (int i = 0; i < RING; i++)
        free(s->ring[i]);
    free(s->data);
    free(s);
}
