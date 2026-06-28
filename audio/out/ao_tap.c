/*
 * Audio output using the Terminal Audio Protocol (TAP)
 * See https://gist.github.com/sopyb/ec682c9dbb1899f70039b6e81b0b546c
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>

#include <libavutil/base64.h>

#include "config.h"

#if HAVE_POSIX
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

#if HAVE_POSIX_SHM
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include "mpv_talloc.h"
#include "options/m_option.h"
#include "common/common.h"
#include "common/msg.h"
#include "audio/format.h"
#include "misc/bstr.h"
#include "misc/random.h"
#include "osdep/timer.h"
#include "ao.h"
#include "internal.h"


#define TAP_APC_OPEN  "\033_A"
#define TAP_ST "\033\\"

#define TAP_CHUNK_RAW  3072

#define TAP_B64_SIZE   (AV_BASE64_SIZE(TAP_CHUNK_RAW))

static const bstr DCS_TMUX_PREFIX   = bstr0_lit("\033Ptmux;\033");
static const bstr DCS_SCREEN_PREFIX = bstr0_lit("\033P\033");
static const bstr DCS_SUFFIX        = bstr0_lit("\033\\");


struct tap_caps {
    bool v1;
    bool invalid_v1;
    int  rates[16];
    int  nrates;
    int  channels[8];
    int  nchannels;
    bool has_s16le;
    bool has_u8;
    bool has_f32le;
};

struct priv {
    bool opt_use_shm;
    bool opt_auto_mux;
    int  opt_buffer_ms;

    struct tap_caps caps;
    const char *format_str;

    mp_rand_state rng;
    uint32_t stream_id;
    bool     stream_open;
    bool     playing;
    bool     paused;

    uint8_t *chunk_buf;
    int      chunk_used;

#if HAVE_POSIX_SHM
    bool     use_shm;
    char    *shm_path;
    int      shm_fd;
    void    *shm_ptr;
    size_t   shm_size;
#endif

    bstr dcs_prefix;
    bstr dcs_suffix;

    double last_time;
    double buffered;

    char b64_buf[TAP_B64_SIZE];
};


static void tap_write_raw(const void *buf, size_t len)
{
#if HAVE_POSIX
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, p, len);
        if (n <= 0)
            return;
        p   += n;
        len -= n;
    }
#else
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
#endif
}

static void tap_write_bstr(bstr s)
{
    tap_write_raw(s.start, s.len);
}

static void tap_emit(struct priv *p, const char *data, size_t len)
{
    if (p->dcs_prefix.len) {
        tap_write_bstr(p->dcs_prefix);
        tap_write_raw(data, len);
        tap_write_bstr(p->dcs_suffix);
    } else {
        tap_write_raw(data, len);
    }
}

static void tap_send(struct priv *p, const char *ctl,
                     const char *payload, int plen)
{
    size_t ctl_len     = strlen(ctl);
    size_t total       = 3 + ctl_len + (payload ? 1 + plen : 0) + 2;
    char  *buf         = talloc_size(NULL, total + 1);
    char  *pos         = buf;

    memcpy(pos, TAP_APC_OPEN, 3);  pos += 3;
    memcpy(pos, ctl, ctl_len);     pos += ctl_len;
    if (payload && plen > 0) {
        *pos++ = ';';
        memcpy(pos, payload, plen); pos += plen;
    }
    memcpy(pos, TAP_ST, 2); pos += 2;
    *pos = '\0';

    tap_emit(p, buf, (size_t)(pos - buf));
    talloc_free(buf);
}


static int parse_pipe_ints(const char *val, int *out, int maxn)
{
    int   n   = 0;
    char *tmp = talloc_strdup(NULL, val);
    char *tok = tmp;
    while (tok && n < maxn) {
        char *sep = strchr(tok, '|');
        if (sep) *sep = '\0';
        int v = atoi(tok);
        if (v > 0)
            out[n++] = v;
        tok = sep ? sep + 1 : NULL;
    }
    talloc_free(tmp);
    return n;
}

static bool parse_tap_caps(struct tap_caps *caps, const char *raw)
{
    int protocol_version = 0;
    bool have_rates = false;
    bool have_channels = false;
    bool have_formats = false;

    const char *ok = strstr(raw, "OK;");
    if (!ok)
        return true;

    char *kv = talloc_strdup(NULL, ok + 3);
    char *tok = kv;
    while (tok) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = '\0';
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            const char *key = tok;
            const char *val = eq + 1;
            if (strcmp(key, "V") == 0) {
                protocol_version = atoi(val);
            } else if (strcmp(key, "r") == 0) {
                caps->nrates = parse_pipe_ints(val, caps->rates,
                                               MP_ARRAY_SIZE(caps->rates));
                have_rates = true;
            } else if (strcmp(key, "c") == 0) {
                caps->nchannels = parse_pipe_ints(val, caps->channels,
                                                   MP_ARRAY_SIZE(caps->channels));
                have_channels = true;
            } else if (strcmp(key, "s") == 0) {
                caps->has_s16le = strstr(val, "raw/s16le") != NULL;
                caps->has_u8    = strstr(val, "raw/u8")    != NULL;
                caps->has_f32le = strstr(val, "raw/f32le") != NULL;
                have_formats = true;
            }
        }
        tok = comma ? comma + 1 : NULL;
    }
    talloc_free(kv);

    if (protocol_version == 1) {
        bool invalid = !have_rates || caps->nrates <= 0 ||
                       !have_channels || caps->nchannels <= 0 ||
                       !have_formats;
        caps->v1 = !invalid;
        caps->invalid_v1 = invalid;
        return !invalid;
    }

    if (!have_rates || caps->nrates <= 0) {
        caps->rates[0] = 44100;
        caps->rates[1] = 48000;
        caps->nrates = 2;
    }

    if (!have_channels || caps->nchannels <= 0) {
        caps->channels[0] = 1;
        caps->channels[1] = 2;
        caps->nchannels = 2;
    }

    if (!have_formats)
        caps->has_s16le = true;

    return true;
}

#if HAVE_POSIX
static char *read_tty_response(int fd, int timeout_ms)
{
    char    buf[4096];
    int     len      = 0;
    int64_t deadline = mp_time_ns() + (int64_t)timeout_ms * 1000000LL;

    while (len < (int)(sizeof(buf) - 1)) {
        int64_t now     = mp_time_ns();
        int     ms_left = (int)((deadline - now) / 1000000LL);
        if (ms_left <= 0)
            break;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, ms_left) <= 0)
            break;
        if (!(pfd.revents & POLLIN))
            break;

        ssize_t n = read(fd, buf + len, sizeof(buf) - len - 1);
        if (n <= 0)
            break;
        len += (int)n;
        buf[len] = '\0';

        if (strstr(buf, TAP_ST))
            break;
    }

    if (len <= 0)
        return NULL;
    buf[len] = '\0';
    return talloc_strdup(NULL, buf);
}

static bool parse_tap_transport_error(const char *resp, uint32_t stream_id,
                                      char *err_out, size_t err_out_sz)
{
    if (!resp)
        return false;

    char id_prefix[64];
    snprintf(id_prefix, sizeof(id_prefix), "Aa=t,i=%" PRIu32 ";", stream_id);

    const char *cur = resp;
    while ((cur = strstr(cur, TAP_APC_OPEN)) != NULL) {
        const char *payload = cur + 3;
        const char *end = strstr(payload, TAP_ST);
        if (!end)
            break;

        size_t payload_len = end > payload ? (size_t)(end - payload) : 0;
        if (payload_len > 0) {
            size_t n = MPMIN(payload_len, 511);
            char tmp[512];
            memcpy(tmp, payload, n);
            tmp[n] = '\0';

            char *p = strstr(tmp, id_prefix);
            if (p == tmp) {
                const char *code = p + strlen(id_prefix);
                if (code[0] == 'E') {
                    if (err_out && err_out_sz > 0) {
                        snprintf(err_out, err_out_sz, "%s", code);
                    }
                    return true;
                }
            }
        }

        cur = end + 2;
    }

    return false;
}

static bool poll_tap_transport_error(struct ao *ao, struct priv *p,
                                     uint32_t stream_id, int timeout_ms,
                                     char *err_out, size_t err_out_sz)
{
    (void)ao;

    int tty_fd = open("/dev/tty", O_RDONLY | O_NOCTTY | O_CLOEXEC);
    if (tty_fd < 0)
        return false;

    struct termios old_term, raw;
    if (tcgetattr(tty_fd, &old_term) < 0) {
        close(tty_fd);
        return false;
    }
    raw = old_term;
    cfmakeraw(&raw);
    tcsetattr(tty_fd, TCSAFLUSH, &raw);

    char *resp = read_tty_response(tty_fd, timeout_ms);

    tcsetattr(tty_fd, TCSAFLUSH, &old_term);
    close(tty_fd);

    if (!resp)
        return false;

    bool has_error = parse_tap_transport_error(resp, stream_id, err_out, err_out_sz);
    talloc_free(resp);
    return has_error;
}

static bool probe_tap(struct ao *ao, struct priv *p)
{
    if (!isatty(STDOUT_FILENO))
        return false;

    int tty_fd = open("/dev/tty", O_RDONLY | O_NOCTTY | O_CLOEXEC);
    if (tty_fd < 0)
        return false;

    struct termios old_term, raw;
    if (tcgetattr(tty_fd, &old_term) < 0) {
        close(tty_fd);
        return false;
    }
    raw = old_term;
    cfmakeraw(&raw);
    tcsetattr(tty_fd, TCSAFLUSH, &raw);

    const char *query = TAP_APC_OPEN "a=q,q=0" TAP_ST "\033[c";
    tap_emit(p, query, strlen(query));

    char *resp = read_tty_response(tty_fd, 500);

    tcsetattr(tty_fd, TCSAFLUSH, &old_term);
    close(tty_fd);

    if (!resp)
        return false;

    char *apc = strstr(resp, TAP_APC_OPEN);
    bool ok = false;
    if (apc) {
        char *end = strstr(apc, TAP_ST);
        if (end) {
            char *payload = apc + 3;
            *end = '\0';
            if (strstr(payload, "a=q") && strstr(payload, "OK")) {
                if (!parse_tap_caps(&p->caps, payload)) {
                    MP_ERR(ao, "TAP probe: V=1 response missing mandatory capability keys\n");
                    ok = false;
                } else {
                    ok = true;
                }
            }
        }
    }

    talloc_free(resp);

    if (ok) {
        MP_VERBOSE(ao, "TAP probe succeeded: %d rates, %d channels\n",
                   p->caps.nrates, p->caps.nchannels);
    } else {
        MP_VERBOSE(ao, "TAP probe: terminal did not respond (non-TAP terminal?)\n");
    }
    return ok;
}
#endif


static void drain_time(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (p->paused)
        return;
    double now = mp_time_sec();
    if (p->buffered > 0) {
        p->buffered -= (now - p->last_time) * ao->samplerate;
        if (p->buffered < 0)
            p->buffered = 0;
    }
    p->last_time = now;
}


static void emit_inline_chunk(struct ao *ao,
                               const uint8_t *raw, int len,
                               bool first, bool more)
{
    struct priv *p = ao->priv;

    int b64_len = 0;
    if (raw && len > 0) {
        av_base64_encode(p->b64_buf, TAP_B64_SIZE, raw, len);
        b64_len = (int)strlen(p->b64_buf);
    }

    char ctl[256];
    if (first) {
        snprintf(ctl, sizeof(ctl),
                 "a=t,i=%" PRIu32 ",s=%s,r=%d,c=%d,B=%d,P=1,q=2,m=%d",
                 p->stream_id, p->format_str,
                 ao->samplerate, ao->channels.num,
                 p->opt_buffer_ms, more ? 1 : 0);
    } else {
        snprintf(ctl, sizeof(ctl),
                 "a=t,i=%" PRIu32 ",q=2,m=%d",
                 p->stream_id, more ? 1 : 0);
    }

    tap_send(p, ctl, b64_len > 0 ? p->b64_buf : NULL, b64_len);
}

static void flush_chunk(struct ao *ao, bool final)
{
    struct priv *p = ao->priv;
    if (p->chunk_used == 0 && !p->stream_open && !final)
        return;

    emit_inline_chunk(ao, p->chunk_buf, p->chunk_used,
                      !p->stream_open, !final);
    p->stream_open = true;
    p->chunk_used  = 0;
}


#if HAVE_POSIX_SHM
static void emit_shm_chunk(struct ao *ao,
                            const uint8_t *raw, int len,
                            bool first, bool more)
{
    struct priv *p = ao->priv;

    if (p->shm_fd < 0) {
        p->shm_fd = shm_open(p->shm_path,
                             O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
        if (p->shm_fd < 0)
            goto fallback;
    }

    if ((size_t)len > p->shm_size) {
        if (p->shm_ptr) {
            munmap(p->shm_ptr, p->shm_size);
            p->shm_ptr  = NULL;
            p->shm_size = 0;
        }
        if (ftruncate(p->shm_fd, len) < 0)
            goto fallback_close;
        p->shm_ptr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                          MAP_SHARED, p->shm_fd, 0);
        if (p->shm_ptr == MAP_FAILED) {
            p->shm_ptr  = NULL;
            p->shm_size = 0;
            goto fallback_close;
        }
        p->shm_size = len;
    } else {
        if (ftruncate(p->shm_fd, len) < 0)
            goto fallback_close;
    }

    memcpy(p->shm_ptr, raw, len);

    const char *shm_name = p->shm_path;
    char ctl[512];
    if (first) {
        snprintf(ctl, sizeof(ctl),
                 "a=t,i=%" PRIu32 ",s=%s,r=%d,c=%d,B=%d,P=1"
                 ",t=s,S=0,O=0,q=1,m=%d",
                 p->stream_id, p->format_str,
                 ao->samplerate, ao->channels.num,
                 p->opt_buffer_ms, more ? 1 : 0);
    } else {
        snprintf(ctl, sizeof(ctl),
                 "a=t,i=%" PRIu32 ",t=s,S=0,O=0,q=1,m=%d",
                 p->stream_id, more ? 1 : 0);
    }
    tap_send(p, ctl, shm_name, (int)strlen(shm_name));

    char tap_err[128] = {0};
    if (poll_tap_transport_error(ao, p, p->stream_id, 10,
                                 tap_err, sizeof(tap_err))) {
        MP_WARN(ao, "TAP SHM transport failed (%s), falling back to inline\n",
                tap_err[0] ? tap_err : "terminal error");
        if (p->shm_ptr && p->shm_ptr != MAP_FAILED) {
            munmap(p->shm_ptr, p->shm_size);
            p->shm_ptr = NULL;
            p->shm_size = 0;
        }
        if (p->shm_fd >= 0) {
            close(p->shm_fd);
            p->shm_fd = -1;
        }
        p->use_shm = false;
        emit_inline_chunk(ao, raw, len, first, more);
    }
    return;

fallback_close:
    close(p->shm_fd);
    p->shm_fd = -1;
fallback:
    emit_inline_chunk(ao, raw, len, first, more);
}
#endif

static void dispatch_chunk(struct ao *ao,
                            const uint8_t *raw, int len,
                            bool first, bool more)
{
#if HAVE_POSIX_SHM
    struct priv *p = ao->priv;
    if (p->use_shm) {
        emit_shm_chunk(ao, raw, len, first, more);
        return;
    }
#endif
    emit_inline_chunk(ao, raw, len, first, more);
}


static uint32_t new_stream_id(struct priv *p)
{
    uint64_t v;
    do {
        v = mp_rand_next(&p->rng) & 0xFFFFFFFFU;
    } while (v == 0);
    return (uint32_t)v;
}

static void delete_stream(struct priv *p)
{
    if (!p->stream_open)
        return;
    char ctl[64];
    snprintf(ctl, sizeof(ctl), "a=d,i=%" PRIu32 ",q=2", p->stream_id);
    tap_send(p, ctl, NULL, 0);
    p->stream_open = false;
}

static void close_stream(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!p->stream_open)
        return;

#if HAVE_POSIX_SHM
    if (p->use_shm) {
        delete_stream(p);
        return;
    }
#endif

    if (p->chunk_used > 0) {
        flush_chunk(ao, true);
    } else {
        emit_inline_chunk(ao, NULL, 0, false, false);
    }
    delete_stream(p);
}


static int init(struct ao *ao)
{
    struct priv *p = ao->priv;

    p->rng       = mp_rand_seed(0);
    p->stream_id = new_stream_id(p);

    if (p->opt_auto_mux) {
#if HAVE_POSIX
        if (getenv("TMUX")) {
            p->dcs_prefix = DCS_TMUX_PREFIX;
            p->dcs_suffix = DCS_SUFFIX;
        } else if (getenv("STY")) {
            p->dcs_prefix = DCS_SCREEN_PREFIX;
            p->dcs_suffix = DCS_SUFFIX;
        }
#endif
    }

#if HAVE_POSIX
    if (!probe_tap(ao, p)) {
        if (p->caps.invalid_v1) {
            MP_ERR(ao, "Terminal returned invalid TAP V=1 capabilities\n");
        } else {
            MP_ERR(ao, "Terminal does not advertise TAP support\n");
        }
        return -1;
    }
#else
    MP_ERR(ao, "TAP probe is not supported on this platform\n");
    return -1;
#endif

    if (p->caps.has_f32le && ao->format == AF_FORMAT_FLOAT) {
        ao->format     = AF_FORMAT_FLOAT;
        p->format_str  = "raw/f32le";
    } else if (p->caps.has_u8 && ao->format == AF_FORMAT_U8) {
        ao->format     = AF_FORMAT_U8;
        p->format_str  = "raw/u8";
    } else {
        ao->format     = AF_FORMAT_S16;
        p->format_str  = "raw/s16le";
    }


    ao->samplerate = 48000;

    mp_chmap_from_channels(&ao->channels, ao->channels.num <= 1 ? 1 : 2);

    ao->device_buffer = ao->samplerate * p->opt_buffer_ms / 1000;


#if HAVE_POSIX_SHM
    if (p->opt_use_shm) {
        p->shm_path = talloc_asprintf(p, "/mpv-tap-%08x",
                                      (unsigned)p->stream_id);
        p->shm_fd   = -1;
        p->use_shm  = true;
        MP_VERBOSE(ao, "Using SHM transport: %s\n", p->shm_path);
    }
#else
    if (p->opt_use_shm)
        MP_WARN(ao, "POSIX shared memory not available; falling back to inline\n");
#endif

    p->chunk_buf = talloc_array(p, uint8_t, TAP_CHUNK_RAW);

    MP_VERBOSE(ao, "TAP AO: format=%s rate=%d channels=%d%s\n",
               p->format_str, ao->samplerate, ao->channels.num,
#if HAVE_POSIX_SHM
               p->use_shm ? " (SHM)" :
#endif
               " (inline)");

    p->last_time = mp_time_sec();
    return 0;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    close_stream(ao);

#if HAVE_POSIX_SHM
    if (p->shm_ptr && p->shm_ptr != MAP_FAILED)
        munmap(p->shm_ptr, p->shm_size);
    if (p->shm_fd >= 0)
        close(p->shm_fd);
    if (p->shm_path)
        shm_unlink(p->shm_path);
#endif
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;
    drain_time(ao);
    p->playing  = true;
    p->last_time = mp_time_sec();
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;

    close_stream(ao);

    p->chunk_used = 0;
    p->buffered   = 0;
    p->playing    = false;
    p->paused     = false;

    p->stream_id = new_stream_id(p);
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;

    if (!p->stream_open)
        return true;

    if (p->paused != paused) {
        char ctl[64];
        snprintf(ctl, sizeof(ctl),
                 "a=p,i=%" PRIu32 ",p=%d,q=2",
                 p->stream_id, paused ? 2 : 1);
        tap_send(p, ctl, NULL, 0);

        drain_time(ao);
        p->paused    = paused;
        p->last_time = mp_time_sec();
    }
    return true;
}

static bool audio_write(struct ao *ao, void **data, int samples)
{
    struct priv *p = ao->priv;

#if HAVE_POSIX_SHM
    if (p->use_shm) {
        const uint8_t *raw = (const uint8_t *)data[0];
        int len = samples * ao->sstride;
        dispatch_chunk(ao, raw, len, !p->stream_open, true);
        p->stream_open = true;
        drain_time(ao);
        p->buffered += samples;
        return true;
    }
#endif

    const uint8_t *src = (const uint8_t *)data[0];
    int len = samples * ao->sstride;
    int pos = 0;

    while (pos < len) {
        int room = TAP_CHUNK_RAW - p->chunk_used;
        int take = MPMIN(room, len - pos);
        memcpy(p->chunk_buf + p->chunk_used, src + pos, take);
        p->chunk_used += take;
        pos           += take;

        if (p->chunk_used == TAP_CHUNK_RAW)
            flush_chunk(ao, false);
    }

    drain_time(ao);
    p->buffered += samples;
    return true;
}

static void get_state(struct ao *ao, struct mp_pcm_state *state)
{
    struct priv *p = ao->priv;

    drain_time(ao);

    int free_samples = ao->device_buffer - (int)p->buffered;
    if (ao->sstride > 0)
        free_samples = free_samples / ao->sstride * ao->sstride;

    state->free_samples   = MPMAX(0, free_samples);
    state->queued_samples = (int)p->buffered;
    state->delay          = p->buffered / (double)ao->samplerate;
    state->playing        = p->playing && p->buffered > 0;
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_tap = {
    .description = "Terminal Audio Protocol (TAP) output",
    .name        = "tap",
    .init        = init,
    .uninit      = uninit,
    .start       = start,
    .reset       = reset,
    .set_pause   = set_pause,
    .write       = audio_write,
    .get_state   = get_state,
    .priv_size   = sizeof(struct priv),
    .priv_defaults = &(const struct priv){
        .opt_use_shm   = false,
        .opt_auto_mux  = true,
        .opt_buffer_ms = 200,
#if HAVE_POSIX_SHM
        .shm_fd        = -1,
#endif
    },
    .options = (const struct m_option[]) {
        {"use-shm",      OPT_BOOL(opt_use_shm)},
        {"auto-multiplexer-passthrough", OPT_BOOL(opt_auto_mux)},
        {"buffer",       OPT_INT(opt_buffer_ms),
                         M_RANGE(0, 10000)},
        {0}
    },
    .options_prefix = "ao-tap",
};
