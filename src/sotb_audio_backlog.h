/* Audio queue level control of a port: the queue is steered to two buffers, and a queue far above that means the
 * device stopped pulling (sleep, endpoint switch, backend stall), so the stale audio is dropped instead of played
 * late. Pure arithmetic on the stream format; no SDL and no device. */
#ifndef SOTB_AUDIO_BACKLOG_H
#define SOTB_AUDIO_BACKLOG_H

#include <stddef.h>
#include <stdint.h>

/* A queue this much above the target is stale. Normal drain quanta (about 21 ms) and scheduling jitter stay below. */
#define SOTB_AUDIO_BACKLOG_THRESHOLD_MS 100
#define SOTB_AUDIO_MAX_CHANNELS 8
#define SOTB_AUDIO_MAX_SAMPLE_BYTES 4
#define SOTB_AUDIO_MAX_RATE 768000

/* Bytes of one frame, or 0 for a format that cannot be measured. */
static inline int sotb_audio_frame_bytes(int channels, int sample_bytes) {
    if (channels<1 || channels>SOTB_AUDIO_MAX_CHANNELS || sample_bytes<1 || sample_bytes>SOTB_AUDIO_MAX_SAMPLE_BYTES) return 0;
    return channels*sample_bytes;
}

/* The playing time of `queued_bytes` in microseconds, or -1 for an invalid format or a negative size. Whole frames only. */
static inline int64_t sotb_audio_queued_us(int64_t queued_bytes, int rate, int channels, int sample_bytes) {
    const int frame=sotb_audio_frame_bytes(channels,sample_bytes);
    if (!frame || rate<=0 || rate>SOTB_AUDIO_MAX_RATE || queued_bytes<0) return -1;
    const int64_t frames=queued_bytes/frame;
    if (frames>INT64_MAX/1000000) return -1;
    return frames*1000000/rate;
}

/* The threshold in bytes, or -1 for an invalid format. */
static inline int64_t sotb_audio_backlog_slack_bytes(int rate, int channels, int sample_bytes) {
    const int frame=sotb_audio_frame_bytes(channels,sample_bytes);
    if (!frame || rate<=0 || rate>SOTB_AUDIO_MAX_RATE) return -1;
    return (int64_t)rate*SOTB_AUDIO_BACKLOG_THRESHOLD_MS/1000*frame;
}

/* Whether `queued_bytes` is more than the threshold above `target_bytes`. Exactly at the limit is not stale; an invalid
 * format or size is never stale, so nothing is dropped on bad information. */
static inline int sotb_audio_backlog_stale(int64_t queued_bytes, int64_t target_bytes, int rate, int channels, int sample_bytes) {
    const int64_t slack=sotb_audio_backlog_slack_bytes(rate,channels,sample_bytes);
    if (slack<0 || queued_bytes<0 || target_bytes<0 || target_bytes>INT64_MAX-slack) return 0;
    return queued_bytes>target_bytes+slack;
}

/* The steering state: the lowest queue level of each window of buffers and the cadence correction it asks for. */
typedef struct { int64_t adjust_ns; int window_min, window_count; } SotbAudioLevel;
#define SOTB_AUDIO_WINDOW_BUFFERS 32

typedef struct {
    int clear;          /* stale backlog: the caller clears the stream before queueing */
    int stale_bytes;    /* bytes above the target that were dropped */
    int silence_bytes;  /* silence to queue first so the device does not run dry */
} SotbAudioLevelStep;

/* One buffer of `bytes` about to be queued while the stream holds `queued` bytes. The target is two buffers.
 * The cadence correction to add to the next deadline is left in `level->adjust_ns`. */
static inline SotbAudioLevelStep sotb_audio_level_step(SotbAudioLevel *level, int queued, int bytes, int rate,
                                                       int channels, int sample_bytes, uint64_t period_ns) {
    SotbAudioLevelStep step={0,0,0};
    const int low=2*bytes;
    if (sotb_audio_backlog_stale(queued,low,rate,channels,sample_bytes)) {
        step.clear=1;
        step.stale_bytes=queued-low;
        queued=0;
        level->window_count=0;
        level->adjust_ns=0;
    }
    if (queued<bytes) {
        step.silence_bytes=low-queued<bytes ? low-queued : bytes;
        queued=low;
    }
    if (!level->window_count || queued<level->window_min) level->window_min=queued;
    if (++level->window_count==SOTB_AUDIO_WINDOW_BUFFERS) {
        level->adjust_ns=level->window_min>low+2*bytes ? (int64_t)(period_ns/SOTB_AUDIO_WINDOW_BUFFERS)
                       : level->window_min<low ? -(int64_t)(period_ns/SOTB_AUDIO_WINDOW_BUFFERS) : 0;
        level->window_count=0;
    }
    return step;
}

/* Recovery messages: the first one is logged, then at most one per interval with the number held back. A backend that
 * keeps stalling therefore does not flood the log. */
#define SOTB_AUDIO_LOG_INTERVAL_NS 5000000000ull
typedef struct { uint64_t last_ns; unsigned suppressed; int logged; } SotbAudioLogLimit;

/* True when a message may be written now; `*suppressed` receives how many were held back since the last one. */
static inline int sotb_audio_log_due(SotbAudioLogLimit *limit, uint64_t now_ns, unsigned *suppressed) {
    if (limit->logged && now_ns-limit->last_ns<SOTB_AUDIO_LOG_INTERVAL_NS) {
        ++limit->suppressed;
        return 0;
    }
    if (suppressed) *suppressed=limit->suppressed;
    limit->suppressed=0;
    limit->last_ns=now_ns;
    limit->logged=1;
    return 1;
}

#endif
