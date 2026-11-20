/* Audio backlog recovery tests: format-aware queue duration, the recovery threshold, the queue level step, a simulated
 * producer and consumer with a device stall and an endpoint reset, and the recovery log rate limit. Pure arithmetic and
 * a model of the queue: no SDL, no audio device is opened. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sotb_audio_backlog.h"

static int g_checks, g_failures;
#define CHECK(condition) do { ++g_checks; if (!(condition)) { ++g_failures; printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#condition); } } while (0)

typedef struct { int rate, channels, sample_bytes, frames; } Format;
static const Format stereo16={48000,2,2,256}, stereo_f32={48000,2,4,256}, surround_f32={48000,8,4,256}, mono16={48000,1,2,256},
                    cd_stereo16={44100,2,2,441};

static int buffer_bytes(const Format *f) { return f->frames*f->channels*f->sample_bytes; }
static uint64_t period_ns(const Format *f) { return (uint64_t)f->frames*1000000000u/(uint64_t)f->rate; }

static void test_duration(void) {
    CHECK(sotb_audio_queued_us(0,48000,2,2)==0);
    CHECK(sotb_audio_queued_us(192000,48000,2,2)==1000000);          /* one second of 48 kHz stereo 16-bit */
    CHECK(sotb_audio_queued_us(1920,48000,2,2)==10000);
    CHECK(sotb_audio_queued_us(384000,48000,2,4)==1000000);          /* float samples */
    CHECK(sotb_audio_queued_us(1536000,48000,8,4)==1000000);         /* 7.1 float */
    CHECK(sotb_audio_queued_us(96000,48000,1,2)==1000000);           /* mono */
    CHECK(sotb_audio_queued_us(176400,44100,2,2)==1000000);          /* another sample rate */
    CHECK(sotb_audio_queued_us(17640,44100,2,2)==100000);
    CHECK(sotb_audio_queued_us(96000*4,96000,2,2)==1000000);
    CHECK(sotb_audio_queued_us(3,48000,2,2)==0);                     /* a partial frame is not counted */
    CHECK(sotb_audio_queued_us(4,48000,2,2)==20);                    /* one frame */
}

static void test_invalid_formats(void) {
    CHECK(sotb_audio_queued_us(1000,0,2,2)==-1);                     /* zero sample rate */
    CHECK(sotb_audio_queued_us(1000,-48000,2,2)==-1);
    CHECK(sotb_audio_queued_us(1000,SOTB_AUDIO_MAX_RATE+1,2,2)==-1);
    CHECK(sotb_audio_queued_us(1000,48000,0,2)==-1);                 /* channel count */
    CHECK(sotb_audio_queued_us(1000,48000,-2,2)==-1);
    CHECK(sotb_audio_queued_us(1000,48000,SOTB_AUDIO_MAX_CHANNELS+1,2)==-1);
    CHECK(sotb_audio_queued_us(1000,48000,2,0)==-1);                 /* sample size */
    CHECK(sotb_audio_queued_us(1000,48000,2,-1)==-1);
    CHECK(sotb_audio_queued_us(1000,48000,2,SOTB_AUDIO_MAX_SAMPLE_BYTES+1)==-1);
    CHECK(sotb_audio_queued_us(-1,48000,2,2)==-1);
    CHECK(sotb_audio_frame_bytes(2,0)==0 && sotb_audio_frame_bytes(0,2)==0 && sotb_audio_frame_bytes(8,4)==32);
    CHECK(sotb_audio_backlog_slack_bytes(0,2,2)==-1 && sotb_audio_backlog_slack_bytes(48000,0,2)==-1 && sotb_audio_backlog_slack_bytes(48000,2,0)==-1);
    /* An unmeasurable format is never stale: nothing is dropped on bad information. */
    CHECK(!sotb_audio_backlog_stale(1<<30,0,0,2,2) && !sotb_audio_backlog_stale(1<<30,0,48000,0,2) && !sotb_audio_backlog_stale(1<<30,0,48000,2,0));
    CHECK(!sotb_audio_backlog_stale(-1,0,48000,2,2) && !sotb_audio_backlog_stale(100,-1,48000,2,2));
}

static void test_overflow(void) {
    CHECK(sotb_audio_queued_us(INT64_MAX,48000,1,1)==-1);            /* frames * 1e6 would overflow */
    CHECK(sotb_audio_queued_us(INT64_MAX,48000,8,4)==-1);            /* 8-channel float: still too many frames */
    CHECK(sotb_audio_queued_us((INT64_MAX/1000000)*32,48000,8,4)>0); /* large but representable */
    CHECK(sotb_audio_queued_us(INT64_MAX/1000000*2,48000,1,1)==-1);
    CHECK(sotb_audio_queued_us(INT64_MAX/1000000,48000,1,1)>0);
    CHECK(!sotb_audio_backlog_stale(INT64_MAX,INT64_MAX,48000,2,2)); /* target + slack would overflow */
    CHECK(!sotb_audio_backlog_stale(INT64_MAX,INT64_MAX-10,48000,2,2));
    CHECK(sotb_audio_backlog_stale(INT64_MAX,0,48000,2,2));
    CHECK(sotb_audio_backlog_slack_bytes(SOTB_AUDIO_MAX_RATE,8,4)>0);
}

static void test_threshold(void) {
    CHECK(SOTB_AUDIO_BACKLOG_THRESHOLD_MS==100);
    CHECK(sotb_audio_backlog_slack_bytes(48000,2,2)==4800*4);        /* 100 ms of stereo 16-bit */
    CHECK(sotb_audio_backlog_slack_bytes(48000,8,4)==4800*32);
    CHECK(sotb_audio_backlog_slack_bytes(44100,2,2)==4410*4);
    const Format *formats[]={&stereo16,&stereo_f32,&surround_f32,&mono16,&cd_stereo16};
    for (size_t i=0;i<sizeof(formats)/sizeof(*formats);++i) {
        const Format *f=formats[i];
        const int bytes=buffer_bytes(f), low=2*bytes;
        const int64_t slack=sotb_audio_backlog_slack_bytes(f->rate,f->channels,f->sample_bytes);
        CHECK(sotb_audio_queued_us(slack,f->rate,f->channels,f->sample_bytes)==100000);
        CHECK(!sotb_audio_backlog_stale(0,low,f->rate,f->channels,f->sample_bytes));
        CHECK(!sotb_audio_backlog_stale(low,low,f->rate,f->channels,f->sample_bytes));
        CHECK(!sotb_audio_backlog_stale(low+slack-1,low,f->rate,f->channels,f->sample_bytes));
        CHECK(!sotb_audio_backlog_stale(low+slack,low,f->rate,f->channels,f->sample_bytes));   /* exactly at the limit: kept */
        CHECK(sotb_audio_backlog_stale(low+slack+1,low,f->rate,f->channels,f->sample_bytes));  /* above it: stale */
    }
}

static void test_level_step(void) {
    const Format *f=&stereo16;
    const int bytes=buffer_bytes(f), low=2*bytes;
    const int64_t slack=sotb_audio_backlog_slack_bytes(f->rate,f->channels,f->sample_bytes);
    SotbAudioLevel level; memset(&level,0,sizeof(level));
    SotbAudioLevelStep s=sotb_audio_level_step(&level,low,bytes,f->rate,f->channels,f->sample_bytes,period_ns(f));
    CHECK(!s.clear && s.silence_bytes==0 && s.stale_bytes==0);       /* below the threshold: unchanged */
    s=sotb_audio_level_step(&level,low+(int)slack,bytes,f->rate,f->channels,f->sample_bytes,period_ns(f));
    CHECK(!s.clear);
    s=sotb_audio_level_step(&level,low+(int)slack+1,bytes,f->rate,f->channels,f->sample_bytes,period_ns(f));
    CHECK(s.clear && s.stale_bytes==(int)slack+1);
    /* After a clear the steering is forgotten and the stream refills to the target with silence. */
    CHECK(level.window_count==1 && level.adjust_ns==0 && s.silence_bytes==bytes);
    CHECK(level.window_min==low);
    /* Underrun: silence tops the queue up to the target, at most one buffer at a time. */
    memset(&level,0,sizeof(level));
    s=sotb_audio_level_step(&level,0,bytes,f->rate,f->channels,f->sample_bytes,period_ns(f));
    CHECK(!s.clear && s.silence_bytes==bytes);
    s=sotb_audio_level_step(&level,bytes/2,bytes,f->rate,f->channels,f->sample_bytes,period_ns(f));
    CHECK(s.silence_bytes==low-bytes/2 || s.silence_bytes==bytes);
    /* An invalid format never clears. */
    memset(&level,0,sizeof(level));
    s=sotb_audio_level_step(&level,1<<28,bytes,0,f->channels,f->sample_bytes,period_ns(f));
    CHECK(!s.clear);
}

typedef struct {
    Format f; int stream; SotbAudioLevel level; int clears; int64_t dropped_bytes; int max_stream;
} Port;

static void produce(Port *p) {
    const int bytes=buffer_bytes(&p->f);
    SotbAudioLevelStep s=sotb_audio_level_step(&p->level,p->stream,bytes,p->f.rate,p->f.channels,p->f.sample_bytes,period_ns(&p->f));
    if (s.clear) { p->stream=0; ++p->clears; p->dropped_bytes+=s.stale_bytes; }
    p->stream+=s.silence_bytes;
    p->stream+=bytes;
    if (p->stream>p->max_stream) p->max_stream=p->stream;
}

/* The device pulls a quantum of 21 ms every 21 ms unless stalled; the game delivers a buffer every period. */
static void run(Port *p, uint64_t from_ms, uint64_t to_ms, int device_stalled) {
    const int frame=p->f.channels*p->f.sample_bytes;
    const int quantum=(int)((uint64_t)p->f.rate*21/1000)*frame;
    static uint64_t next_produce_ns, next_pull_ms;
    if (from_ms==0) { next_produce_ns=0; next_pull_ms=0; }
    for (uint64_t ms=from_ms;ms<to_ms;++ms) {
        while (next_produce_ns<=ms*1000000u) { produce(p); next_produce_ns+=period_ns(&p->f)+(uint64_t)(p->level.adjust_ns>0 ? p->level.adjust_ns : 0); }
        if (ms>=next_pull_ms) {
            if (!device_stalled) p->stream-= p->stream<quantum ? p->stream : quantum;
            next_pull_ms=ms+21;
        }
    }
}

static void test_normal_playback_never_recovers(void) {
    const Format *formats[]={&stereo16,&stereo_f32,&surround_f32,&mono16};
    for (size_t i=0;i<sizeof(formats)/sizeof(*formats);++i) {
        Port p; memset(&p,0,sizeof(p)); p.f=*formats[i];
        run(&p,0,20000,0);
        CHECK(p.clears==0);
        CHECK(!sotb_audio_backlog_stale(p.max_stream,2*buffer_bytes(&p.f),p.f.rate,p.f.channels,p.f.sample_bytes));
    }
}

static void test_stall_then_recovery(void) {
    Port p; memset(&p,0,sizeof(p)); p.f=stereo_f32;
    run(&p,0,2000,0);
    CHECK(p.clears==0);
    /* The device stops pulling for 600 ms (sleep, endpoint switch). */
    run(&p,2000,2600,1);
    CHECK(p.clears>=1);
    const int cap=2*buffer_bytes(&p.f)+(int)sotb_audio_backlog_slack_bytes(p.f.rate,p.f.channels,p.f.sample_bytes);
    CHECK(p.max_stream<=cap+buffer_bytes(&p.f));                     /* the backlog never grows past the limit by more than a buffer */
    CHECK(sotb_audio_queued_us(p.max_stream,p.f.rate,p.f.channels,p.f.sample_bytes)<=100000+(int64_t)(4*period_ns(&p.f)/1000));
    /* The device comes back: what is queued is current audio, not 600 ms of old sound. */
    const int clears_at_resume=p.clears;
    run(&p,2600,4000,0);
    CHECK(sotb_audio_queued_us(p.stream,p.f.rate,p.f.channels,p.f.sample_bytes)<=100000);
    CHECK(p.clears==clears_at_resume);                               /* steady again: no further recovery */
}

static void test_repeated_recovery(void) {
    Port p; memset(&p,0,sizeof(p)); p.f=stereo16;
    uint64_t t=0;
    for (int round=0;round<5;++round) {
        run(&p,t,t+500,0); t+=500;
        run(&p,t,t+400,1); t+=400;
    }
    run(&p,t,t+1000,0);
    CHECK(p.clears>=5);
    CHECK(p.stream>=0 && p.level.window_count>=0 && p.level.window_count<SOTB_AUDIO_WINDOW_BUFFERS);
    CHECK(sotb_audio_queued_us(p.stream,p.f.rate,p.f.channels,p.f.sample_bytes)<=100000);
    CHECK(p.dropped_bytes>0);
}

static void test_endpoint_reset(void) {
    /* The endpoint is replaced: the new stream starts empty. That is an underrun, not a backlog: silence, no recovery. */
    Port p; memset(&p,0,sizeof(p)); p.f=surround_f32;
    run(&p,0,1000,0);
    const int clears=p.clears;
    p.stream=0;
    run(&p,1000,1500,0);
    CHECK(p.clears==clears);
    CHECK(p.stream>0);
    CHECK(sotb_audio_queued_us(p.stream,p.f.rate,p.f.channels,p.f.sample_bytes)<=100000);
    /* The reset also hands the device a stalled period before it pulls again. */
    run(&p,1500,1900,1);
    run(&p,1900,2500,0);
    CHECK(sotb_audio_queued_us(p.stream,p.f.rate,p.f.channels,p.f.sample_bytes)<=100000);
}

static void test_log_rate_limit(void) {
    SotbAudioLogLimit limit; memset(&limit,0,sizeof(limit));
    unsigned held=99;
    CHECK(sotb_audio_log_due(&limit,1000,&held) && held==0);        /* the first recovery is logged */
    CHECK(!sotb_audio_log_due(&limit,2000,&held));
    CHECK(!sotb_audio_log_due(&limit,SOTB_AUDIO_LOG_INTERVAL_NS,&held));
    CHECK(!sotb_audio_log_due(&limit,SOTB_AUDIO_LOG_INTERVAL_NS+999,&held));
    CHECK(sotb_audio_log_due(&limit,1000+SOTB_AUDIO_LOG_INTERVAL_NS,&held) && held==3);
    CHECK(sotb_audio_log_due(&limit,1000+3*SOTB_AUDIO_LOG_INTERVAL_NS,&held) && held==0);
    /* A backend recovering every few milliseconds writes a handful of lines, not thousands. */
    memset(&limit,0,sizeof(limit));
    int logged=0;
    for (uint64_t t=0;t<60ull*1000000000ull;t+=10000000ull) logged+=sotb_audio_log_due(&limit,t,NULL);
    CHECK(logged>=1 && logged<=13);
    CHECK(sotb_audio_log_due(&limit,60ull*1000000000ull+SOTB_AUDIO_LOG_INTERVAL_NS,&held) && held>0);
}

int main(void) {
    test_duration();
    test_invalid_formats();
    test_overflow();
    test_threshold();
    test_level_step();
    test_normal_playback_never_recovers();
    test_stall_then_recovery();
    test_repeated_recovery();
    test_endpoint_reset();
    test_log_rate_limit();
    printf("audio backlog tests: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures ? 1 : 0;
}
