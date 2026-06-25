/* fmv.c -- cutscene (.mp4) playback, replacing the engine's fnaFMV_* layer.
 *
 * On Android the engine decodes cutscenes via Java (MediaPlayer -> SurfaceTexture,
 * sampled by a GL_OES_EGL_image_external shader); neither exists here, so we decode
 * the mp4 with ffmpeg and draw it on our own sampler2D quad. The mp4s are loose
 * files at GAMEDATA_DIR/cutscenes/<name>.mp4.
 *
 * We hook only fnaFMV_Open/Finished/Close (the rest are ret-stubs too small to
 * hook): fnaFMV_Open plays the whole movie synchronously, fnaFMV_Finished then
 * reports it done. Audio (AAC) is decoded up front and streamed out by the OpenSL
 * SDL mixer via fmv_audio_override, which also mutes the game's music meanwhile.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>

#include "util.h"
#include "config.h"   // screen_width / screen_height, GAMEDATA_DIR
#include "hooks.h"

// ---------------------------------------------------------------------------
// byte-window AVIO: ffmpeg reads the mp4 through this (base=0, len=filesize for
// a loose cutscene file). Kept as a window reader for simplicity/robustness.
// ---------------------------------------------------------------------------
typedef struct { FILE *f; long long base, len, pos; } ObbIO;

static int io_read(void *opaque, uint8_t *buf, int want) {
  ObbIO *io = opaque;
  long long rem = io->len - io->pos;
  if (rem <= 0) return AVERROR_EOF;
  if (want > rem) want = (int)rem;
  if (fseek(io->f, (long)(io->base + io->pos), SEEK_SET) != 0) return AVERROR(EIO);
  size_t got = fread(buf, 1, (size_t)want, io->f);
  if (got == 0) return AVERROR_EOF;
  io->pos += (long long)got;
  return (int)got;
}

static int64_t io_seek(void *opaque, int64_t off, int whence) {
  ObbIO *io = opaque;
  if (whence == AVSEEK_SIZE) return io->len;
  long long np;
  if (whence == SEEK_SET)      np = off;
  else if (whence == SEEK_CUR) np = io->pos + off;
  else if (whence == SEEK_END) np = io->len + off;
  else return -1;
  if (np < 0 || np > io->len) return -1;
  io->pos = np;
  return np;
}

// ---------------------------------------------------------------------------
// GL: one sampler2D-textured fullscreen quad (created lazily, reused per movie)
// ---------------------------------------------------------------------------
static const char *VS_SRC =
    "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }";
static const char *FS_SRC =
    "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;"
    "void main(){ gl_FragColor = texture2D(uTex, vUV); }";

static GLuint g_prog, g_tex;
static GLint g_aPos, g_aUV, g_uTex;
static int g_gl_ready;

static GLuint fmv_compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512]; GLsizei n = 0;
    glGetShaderInfoLog(s, sizeof log, &n, log);
    debugPrintf("FMV: %s shader COMPILE FAILED: %s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
  }
  return s;
}

static void fmv_gl_init(void) {
  if (g_gl_ready) return;
  GLuint vs = fmv_compile(GL_VERTEX_SHADER, VS_SRC);
  GLuint fs = fmv_compile(GL_FRAGMENT_SHADER, FS_SRC);
  g_prog = glCreateProgram();
  glAttachShader(g_prog, vs);
  glAttachShader(g_prog, fs);
  glLinkProgram(g_prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = 0;
  glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[512]; GLsizei n = 0;
    glGetProgramInfoLog(g_prog, sizeof log, &n, log);
    debugPrintf("FMV: program LINK FAILED: %s\n", log);
  }
  g_aPos = glGetAttribLocation(g_prog, "aPos");
  g_aUV  = glGetAttribLocation(g_prog, "aUV");
  g_uTex = glGetUniformLocation(g_prog, "uTex");
  glGenTextures(1, &g_tex);
  glBindTexture(GL_TEXTURE_2D, g_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  g_gl_ready = 1;
}

// Draw the current texture, letterboxed to preserve the movie's aspect ratio.
static void fmv_draw(int vidw, int vidh) {
  int sw = screen_width > 0 ? screen_width : 1280;
  int sh = screen_height > 0 ? screen_height : 720;
  // fit vidw:vidh into sw:sh, centered (letterbox/pillarbox)
  float sa = (float)sw / (float)sh, va = (float)vidw / (float)vidh;
  float hx = 1.0f, hy = 1.0f;
  if (va > sa) hy = sa / va;   // wider than screen -> bars top/bottom
  else         hx = va / sa;   // taller -> bars left/right
  const float quad[] = {        // x, y, u, v   (V flipped: image top -> screen top)
    -hx, -hy, 0.0f, 1.0f,
     hx, -hy, 1.0f, 1.0f,
    -hx,  hy, 0.0f, 0.0f,
     hx,  hy, 1.0f, 0.0f,
  };
  // present to the screen, not the engine's offscreen scene FBO (which is bound
  // during the engine's render and would otherwise swallow our draw).
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, sw, sh);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(g_prog);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray((GLuint)g_aPos);
  glVertexAttribPointer((GLuint)g_aPos, 2, GL_FLOAT, GL_FALSE, 16, quad);
  glEnableVertexAttribArray((GLuint)g_aUV);
  glVertexAttribPointer((GLuint)g_aUV, 2, GL_FLOAT, GL_FALSE, 16, quad + 2);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_tex);
  glUniform1i(g_uTex, 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// The Fusion engine caches its GL state and skips redundant binds, so the state
// our quad leaves behind (program, textures, attrib arrays, caps, viewport...)
// must be restored EXACTLY or the engine renders with stale state afterwards
// (symptom: all textures vanish). Snapshot before drawing, restore after.
static struct {
  GLint program, activeTex, tex0, arrayBuf, fbo, viewport[4];
  GLboolean depth, cull, blend, scissor;
  struct { GLint en, buf, size, type, norm, stride; void *ptr; } va[2];
} g_sv;

static void fmv_save_gl_state(void) {
  glGetIntegerv(GL_CURRENT_PROGRAM, &g_sv.program);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &g_sv.activeTex);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &g_sv.tex0);
  glActiveTexture((GLenum)g_sv.activeTex);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &g_sv.arrayBuf);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &g_sv.fbo);
  glGetIntegerv(GL_VIEWPORT, g_sv.viewport);
  g_sv.depth   = glIsEnabled(GL_DEPTH_TEST);
  g_sv.cull    = glIsEnabled(GL_CULL_FACE);
  g_sv.blend   = glIsEnabled(GL_BLEND);
  g_sv.scissor = glIsEnabled(GL_SCISSOR_TEST);
  for (int i = 0; i < 2; i++) {
    glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &g_sv.va[i].en);
    glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &g_sv.va[i].buf);
    glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &g_sv.va[i].size);
    glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &g_sv.va[i].type);
    glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &g_sv.va[i].norm);
    glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &g_sv.va[i].stride);
    glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &g_sv.va[i].ptr);
  }
}

static void fmv_restore_gl_state(void) {
  for (int i = 0; i < 2; i++) {
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)g_sv.va[i].buf);
    glVertexAttribPointer((GLuint)i, g_sv.va[i].size, (GLenum)g_sv.va[i].type,
                          (GLboolean)g_sv.va[i].norm, g_sv.va[i].stride, g_sv.va[i].ptr);
    if (g_sv.va[i].en) glEnableVertexAttribArray((GLuint)i);
    else               glDisableVertexAttribArray((GLuint)i);
  }
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)g_sv.arrayBuf);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, (GLuint)g_sv.tex0);
  glActiveTexture((GLenum)g_sv.activeTex);
  glUseProgram((GLuint)g_sv.program);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)g_sv.fbo);
  glViewport(g_sv.viewport[0], g_sv.viewport[1], g_sv.viewport[2], g_sv.viewport[3]);
  if (g_sv.depth)   glEnable(GL_DEPTH_TEST);    else glDisable(GL_DEPTH_TEST);
  if (g_sv.cull)    glEnable(GL_CULL_FACE);     else glDisable(GL_CULL_FACE);
  if (g_sv.blend)   glEnable(GL_BLEND);         else glDisable(GL_BLEND);
  if (g_sv.scissor) glEnable(GL_SCISSOR_TEST);  else glDisable(GL_SCISSOR_TEST);
}

// ---------------------------------------------------------------------------
// skip input
// ---------------------------------------------------------------------------
static PadState g_pad;
static int g_pad_ready;
static int fmv_skip_pressed(void) {
  if (!g_pad_ready) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    g_pad_ready = 1;
  }
  padUpdate(&g_pad);
  u64 d = padGetButtonsDown(&g_pad);
  return (d & (HidNpadButton_B | HidNpadButton_A | HidNpadButton_Plus)) != 0;
}

// Set while a cutscene is playing so the main render loop (a DIFFERENT thread:
// the engine triggers cutscenes from an internal thread) pauses its own
// render+present -- otherwise it presents black/paused frames to the same back
// buffer, interleaved with ours, causing heavy flicker. We become the sole
// presenter for the duration.
volatile int g_fmv_active = 0;

// ---------------------------------------------------------------------------
// cutscene audio: decoded up front into one buffer, then streamed out by the
// OpenSL SDL mixer callback (sl_switch.c calls fmv_audio_override), which
// overwrites the game mix while a movie plays -- so the cutscene's audio is
// heard and the background music goes quiet for the duration.
// ---------------------------------------------------------------------------
static int16_t          *g_audio;        // S16 interleaved stereo @ g_fmv_out_rate
static uint64_t          g_audio_frames; // total stereo frames in g_audio
static volatile uint64_t g_apos;         // playback cursor (advanced by audio thread)
static volatile int      g_audio_on;     // gate checked by the audio thread
extern int               g_fmv_out_rate; // SDL device sample rate (sl_switch.c)

// Audio-thread entry. Overwrites `stream` with the cutscene's PCM and returns 1
// (game mix silenced); returns 0 when no cutscene audio is active.
int fmv_audio_override(void *stream, int len_bytes) {
  if (!g_audio_on || g_audio == NULL) return 0;
  const int frames = len_bytes / 4;        // 16-bit stereo = 4 bytes/frame
  int16_t *out = (int16_t *)stream;
  uint64_t pos = g_apos, n = g_audio_frames;
  for (int i = 0; i < frames; i++) {
    if (pos < n) { out[2 * i] = g_audio[2 * pos]; out[2 * i + 1] = g_audio[2 * pos + 1]; pos++; }
    else { out[2 * i] = 0; out[2 * i + 1] = 0; }   // silence past end of track
  }
  g_apos = pos;
  return 1;
}

// Decode the whole audio stream up front into g_audio, resampled to the output
// format. Best-effort: on any failure the cutscene simply plays silent.
static void fmv_decode_audio(AVFormatContext *fmt, int astream) {
  AVStream *st = fmt->streams[astream];
  const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!dec) return;
  AVCodecContext *cc = avcodec_alloc_context3(dec);
  if (!cc) return;
  avcodec_parameters_to_context(cc, st->codecpar);
  if (avcodec_open2(cc, dec, NULL) < 0) { avcodec_free_context(&cc); return; }

  AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
  SwrContext *swr = NULL;
  if (swr_alloc_set_opts2(&swr, &out_ch, AV_SAMPLE_FMT_S16, g_fmv_out_rate,
                          &cc->ch_layout, cc->sample_fmt, cc->sample_rate, 0, NULL) < 0 ||
      swr_init(swr) < 0) {
    if (swr) swr_free(&swr);
    avcodec_free_context(&cc);
    return;
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame  *fr  = av_frame_alloc();
  int16_t *buf = NULL;
  size_t   cap = 0;        // capacity in stereo frames
  uint64_t nframes = 0;
  int oom = 0;

  for (;;) {
    int r = av_read_frame(fmt, pkt);
    if (r >= 0 && pkt->stream_index != astream) { av_packet_unref(pkt); continue; }
    avcodec_send_packet(cc, r < 0 ? NULL : pkt);   // r<0 -> flush
    if (r >= 0) av_packet_unref(pkt);
    while (avcodec_receive_frame(cc, fr) == 0) {
      int out_max = (int)av_rescale_rnd(swr_get_delay(swr, cc->sample_rate) + fr->nb_samples,
                                        g_fmv_out_rate, cc->sample_rate, AV_ROUND_UP);
      if (nframes + (uint64_t)out_max > cap) {
        size_t ncap = cap ? cap : 65536;
        while (ncap < nframes + (uint64_t)out_max) ncap *= 2;
        int16_t *nb = realloc(buf, ncap * 2 * sizeof(int16_t));
        if (!nb) { oom = 1; break; }
        buf = nb; cap = ncap;
      }
      uint8_t *outp = (uint8_t *)(buf + nframes * 2);
      int got = swr_convert(swr, &outp, out_max,
                            (const uint8_t **)fr->data, fr->nb_samples);
      if (got > 0) nframes += (uint64_t)got;
    }
    if (r < 0 || oom) break;
  }

  av_frame_free(&fr);
  av_packet_free(&pkt);
  swr_free(&swr);
  avcodec_free_context(&cc);

  if (buf && nframes > 0) {
    g_audio_frames = nframes;   // set count before pointer (audio thread gates on pointer)
    g_audio = buf;
  } else {
    free(buf);
  }
}

// ---------------------------------------------------------------------------
// play one cutscene, blocking until it ends or is skipped
// ---------------------------------------------------------------------------
static void fmv_play(const char *want) {
  if (!want) return;

  // The engine passes a path like "cutscenes/foo.mp4" or just "foo"; take the
  // basename, drop any extension, and look under GAMEDATA_DIR/cutscenes/.
  char base[256];
  { const char *b = strrchr(want, '/'); b = b ? b + 1 : want;
    snprintf(base, sizeof base, "%s", b);
    char *dot = strrchr(base, '.'); if (dot) *dot = '\0'; }
  char path[320];
  snprintf(path, sizeof path, "%s/cutscenes/%s.mp4", GAMEDATA_DIR, base);

  ObbIO io = {0};
  io.f = fopen(path, "rb");
  if (!io.f) { debugPrintf("FMV: cutscene file not found: %s (from '%s')\n", path, want); return; }
  fseek(io.f, 0, SEEK_END);
  io.len = (long long)ftell(io.f);   // whole file is the window
  fseek(io.f, 0, SEEK_SET);
  io.base = 0;
  setvbuf(io.f, NULL, _IOFBF, 128 * 1024);

  unsigned char *avbuf = av_malloc(64 * 1024);
  AVIOContext *avio = avio_alloc_context(avbuf, 64 * 1024, 0, &io, io_read, NULL, io_seek);
  AVFormatContext *fmt = avformat_alloc_context();
  AVCodecContext *cc = NULL;
  AVFrame *fr = NULL, *rgb = NULL;
  AVPacket *pkt = NULL;
  struct SwsContext *sws = NULL;
  uint8_t *rgba = NULL;
  int vstream = -1;
  int saved_gl = 0;

  if (!avio || !fmt) { debugPrintf("FMV: avio/fmt alloc failed\n"); goto cleanup; }
  fmt->pb = avio;
  if (avformat_open_input(&fmt, NULL, NULL, NULL) != 0) {
    debugPrintf("FMV: avformat_open_input failed\n");
    fmt = NULL;  // open_input frees fmt on failure
    goto cleanup;
  }
  if (avformat_find_stream_info(fmt, NULL) < 0) { debugPrintf("FMV: no stream info\n"); goto cleanup; }
  for (unsigned i = 0; i < fmt->nb_streams; i++)
    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vstream = (int)i; break; }
  if (vstream < 0) { debugPrintf("FMV: no video stream\n"); goto cleanup; }

  AVCodecParameters *cp = fmt->streams[vstream]->codecpar;
  const AVCodec *dec = avcodec_find_decoder(cp->codec_id);
  if (!dec) { debugPrintf("FMV: no decoder for codec %d\n", cp->codec_id); goto cleanup; }
  cc = avcodec_alloc_context3(dec);
  if (!cc) goto cleanup;
  avcodec_parameters_to_context(cc, cp);
  cc->thread_count = 3;
  if (avcodec_open2(cc, dec, NULL) < 0) { debugPrintf("FMV: decoder open failed\n"); goto cleanup; }

  const int W = cc->width, H = cc->height;
  fr  = av_frame_alloc();
  rgb = av_frame_alloc();
  pkt = av_packet_alloc();
  if (!fr || !rgb || !pkt) goto cleanup;
  int rgbsz = av_image_get_buffer_size(AV_PIX_FMT_RGBA, W, H, 1);
  rgba = av_malloc((size_t)rgbsz);
  if (!rgba) goto cleanup;
  av_image_fill_arrays(rgb->data, rgb->linesize, rgba, AV_PIX_FMT_RGBA, W, H, 1);
  sws = sws_getContext(W, H, cc->pix_fmt, W, H, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
  if (!sws) { debugPrintf("FMV: sws_getContext failed\n"); goto cleanup; }

  // Pre-decode the audio track into one buffer (the SDL mixer streams it out),
  // then rewind the demuxer + flush the video decoder for the video pass.
  {
    int astream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
      if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { astream = (int)i; break; }
    if (astream >= 0) {
      fmv_decode_audio(fmt, astream);
      av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(cc);
    }
  }

  // fnaFMV_Open runs during the engine's update phase, before the render thread
  // has taken the one shared GL context this frame -- so take it ourselves before
  // any GL, or shader/texture creation silently fails (no current context).
  egl_gl_acquire();
  fmv_save_gl_state();   // snapshot the engine's GL state before we touch it
  saved_gl = 1;
  fmv_gl_init();
  const AVRational tb = fmt->streams[vstream]->time_base;
  const u64 freq = armGetSystemTickFreq();
  u64 t0 = armGetSystemTick();
  int started = 0, eof = 0;

  while (!eof) {
    if (fmv_skip_pressed()) break;

    // decode the next video frame
    int got = 0;
    while (!got) {
      int r = av_read_frame(fmt, pkt);
      if (r < 0) {                       // end of file: flush the decoder
        avcodec_send_packet(cc, NULL);
        if (avcodec_receive_frame(cc, fr) == 0) got = 1;
        else { eof = 1; break; }
      } else {
        if (pkt->stream_index == vstream && avcodec_send_packet(cc, pkt) == 0) {
          if (avcodec_receive_frame(cc, fr) == 0) got = 1;
        }
        av_packet_unref(pkt);
      }
    }
    if (!got) break;

    // pace to the frame's presentation time
    double pts = (fr->pts == AV_NOPTS_VALUE ? 0.0 : (double)fr->pts) * av_q2d(tb);
    if (!started) {   // start the audio in lockstep with the first video frame
      t0 = armGetSystemTick();
      started = 1;
      g_apos = 0;
      g_audio_on = (g_audio != NULL);
    }
    double elapsed = (double)(armGetSystemTick() - t0) / (double)freq;
    if (pts > elapsed) {
      u64 ns = (u64)((pts - elapsed) * 1.0e9);
      if (ns > 0 && ns < 1000000000ull) svcSleepThread(ns);
    }

    egl_gl_acquire();   // keep the shared context current (egl_present may yield it)
    sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, H,
              rgb->data, rgb->linesize);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    fmv_draw(W, H);
    egl_present();
  }

cleanup:
  // stop cutscene audio and free it, letting the audio thread finish any
  // in-flight callback first (it gates on g_audio_on) before we free the buffer.
  g_audio_on = 0;
  svcSleepThread(20000000ull);   // 20ms
  if (g_audio) { free(g_audio); g_audio = NULL; g_audio_frames = 0; g_apos = 0; }
  // restore the engine's exact GL state so its state cache stays valid (else all
  // textures vanish after a cutscene), then hand the context back.
  if (saved_gl) { egl_gl_acquire(); fmv_restore_gl_state(); }
  egl_gl_ownership_release();
  if (sws) sws_freeContext(sws);
  if (rgba) av_free(rgba);
  if (rgb) av_frame_free(&rgb);
  if (fr) av_frame_free(&fr);
  if (pkt) av_packet_free(&pkt);
  if (cc) avcodec_free_context(&cc);
  if (fmt) avformat_close_input(&fmt);
  if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
  if (io.f) fclose(io.f);
}

// ---------------------------------------------------------------------------
// fnaFMV_* replacements (hooked over the engine bodies in game.c)
// ---------------------------------------------------------------------------
// fnaFMV_Open(const char *name, bool, const fnaFMVTRACKCHANNEL*, unsigned, const char*)
void *fmv_hook_open(const char *name, int loop, const void *a, unsigned b, const char *c) {
  (void)loop; (void)a; (void)b; (void)c;
  g_fmv_active = 1;    // pause the main render loop's present (sole presenter = us)
  fmv_play(name);
  g_fmv_active = 0;
  return (void *)1;   // non-NULL handle; engine never dereferences it (we own all fnaFMV)
}
unsigned char fmv_hook_finished(void *h) { (void)h; return 1; }  // already played in Open
void fmv_hook_close(void *h) { (void)h; }
