// hwopus: el decodificador Opus «por hardware» de la Switch (IHardwareOpusDecoderManager / IHardwareOpusDecoder),
// sobre libopus. CRIWARE (SMRPG) lo usa para su audio; con el stub de registro el tamaño del búfer de trabajo era 0 y
// nunca se abría un decodificador.
//
// Comandos (como yuzu / Ryujinx):
//   gestor (switchbrew): 0 OpenHardwareOpusDecoder, 1 GetWorkBufferSize, 2 OpenOpusDecoderForMultiStream,
//           3 GetWorkBufferSizeForMultiStream, 4 OpenHardwareOpusDecoderEx, 5 GetWorkBufferSizeEx,
//           6 OpenHardwareOpusDecoderForMultiStreamEx, 7 GetWorkBufferSizeForMultiStreamEx, 8 GetWorkBufferSizeExEx,
//           9 GetWorkBufferSizeForMultiStreamExEx (los parámetros multistream van en un búfer X)
//   decodificador: 0 DecodeInterleavedOld, 1 SetContext, 2 DecodeInterleavedForMultiStreamOld,
//           3 SetContextForMultiStream, 4/5 ...WithPerfOld, 6/7 ...WithPerfAndResetOld, 8 DecodeInterleaved,
//           9 DecodeInterleavedForMultiStream
// Cada paquete de entrada lleva la cabecera de Nintendo: tamaño y «final range» en big-endian (8 bytes), y luego el
// paquete Opus. La salida es PCM de 16 bits intercalado; se devuelve lo consumido (cabecera + paquete), las muestras
// por canal y, en las formas «Perf», el tiempo.
// LARECOMP_HWOPUS_LOG=1: cada orden al registro.
#include <opus/opus.h>
#include <opus/opus_multistream.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

typedef struct {
    OpusDecoder *dec;
    OpusMSDecoder *ms;
    uint32_t rate, channels;
    int nerr;
} HwOpus;

static int hw_log = -1;
static bool hwlog(void) {
    if (hw_log < 0) hw_log = getenv("LARECOMP_HWOPUS_LOG") && atoi(getenv("LARECOMP_HWOPUS_LOG")) != 0;
    return hw_log;
}

static bool valid_rate(uint32_t r) { return r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 48000; }

// El tamaño del búfer de trabajo (como yuzu: el estado del decodificador + una trama + margen). El juego reserva esa
// memoria y la pasa como memoria de transferencia; aquí no se usa (el estado vive en el anfitrión).
static uint32_t work_size(uint32_t rate, uint32_t channels, uint32_t streams, uint32_t stereo, bool large) {
    int st = streams ? opus_multistream_decoder_get_size((int)streams, (int)stereo) : opus_decoder_get_size((int)channels);
    if (st < 0) st = 0x8000;
    uint32_t frame = large ? 5760 : 1920;
    uint32_t div = rate ? 48000 / rate : 1;
    if (!div) div = 1;
    uint32_t pcm = (frame * channels / div * 2 + 63) & ~63u;
    return (uint32_t)st + pcm + 0x600;
}

static void hwopus_close(Service *s) {
    HwOpus *h = s->state;
    if (!h) return;
    if (h->dec) opus_decoder_destroy(h->dec);
    if (h->ms) opus_multistream_decoder_destroy(h->ms);
    free(h);
    s->state = NULL;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Decodifica un paquete. Devuelve el resultado de la orden (0 = bien).
static uint32_t hw_decode(HwOpus *h, IpcReq *req, IpcResp *resp, bool perf, bool reset) {
    IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
    const uint8_t *src = in ? (const uint8_t *)(uintptr_t)in->addr : NULL;
    int16_t *pcm = out ? (int16_t *)(uintptr_t)out->addr : NULL;
    uint64_t in_size = in ? in->size : 0, out_size = out ? out->size : 0;
    if (reset) {
        if (h->dec) opus_decoder_ctl(h->dec, OPUS_RESET_STATE);
        if (h->ms) opus_multistream_decoder_ctl(h->ms, OPUS_RESET_STATE);
    }
    if (!src || in_size < 8) {
        if (h->nerr++ < 8) LOG("[hwopus] entrada demasiado corta (%llu bytes)\n", (unsigned long long)in_size);
        return 0x14e6f;  // (ResultInvalidOpusDataSize: módulo 111, descripción 167)
    }
    uint32_t size = be32(src);
    if ((uint64_t)size + 8 > in_size) {
        if (h->nerr++ < 8) LOG("[hwopus] paquete de %u bytes en una entrada de %llu\n", size, (unsigned long long)in_size);
        return 0x14e6f;
    }
    int max = (int)(out_size / (2 * h->channels));
    if (max > 5760 * (int)h->channels) max = 5760 * (int)h->channels;
    int n = h->ms ? opus_multistream_decode(h->ms, src + 8, (opus_int32)size, pcm, max, 0)
                  : opus_decode(h->dec, src + 8, (opus_int32)size, pcm, max, 0);
    if (n < 0) {  // un paquete que libopus no acepta: 20 ms de silencio (como nxnative)
        if (h->nerr++ < 8) LOG("[hwopus] libopus rechaza un paquete de %u bytes: %s\n", size, opus_strerror(n));
        n = (int)(h->rate / 50);
        if (n > max) n = max;
        if (pcm && n > 0) memset(pcm, 0, (size_t)n * h->channels * 2);
    }
    OUT(uint32_t, 0) = size + 8;     // bytes consumidos
    OUT(uint32_t, 4) = (uint32_t)n;  // muestras por canal
    if (perf) { OUT(uint64_t, 8) = 0; resp_size(resp, 16); }
    else resp_size(resp, 8);
    if (hwlog()) LOG("[hwopus] decode %u bytes -> %d muestras (%u ch, salida %llu bytes%s)\n", size, n, h->channels,
                     (unsigned long long)out_size, reset ? ", reset" : "");
    return 0;
}

static void hwopus_decoder(Service *s, IpcReq *req, IpcResp *resp) {
    HwOpus *h = s->state;
    switch (req->cmd) {
    case 0: case 2: resp->result = hw_decode(h, req, resp, false, false); break;  // DecodeInterleaved(ForMultiStream)Old
    case 1: case 3: break;                                                         // SetContext(ForMultiStream)
    case 4: case 5: resp->result = hw_decode(h, req, resp, true, false); break;   // ...WithPerfOld
    case 6: case 7: case 8: case 9:                                                // ...WithPerfAndResetOld, DecodeInterleaved
        resp->result = hw_decode(h, req, resp, true, req->data_size >= 1 && IN(uint8_t, 0) != 0);
        break;
    default: LOG("[hwopus] decodificador: orden %u desconocida (ok)\n", req->cmd);
    }
}

static Service *open_single(uint32_t rate, uint32_t channels) {
    int err = 0;
    OpusDecoder *d = opus_decoder_create((opus_int32)rate, (int)channels, &err);
    if (!d) { LOG("[hwopus] opus_decoder_create(%u Hz, %u ch): %s\n", rate, channels, opus_strerror(err)); return NULL; }
    HwOpus *h = calloc(1, sizeof *h);
    h->dec = d; h->rate = rate; h->channels = channels;
    Service *svc = service_new("IHardwareOpusDecoder", hwopus_decoder, h);
    svc->on_close = hwopus_close;
    LOG("[hwopus] decodificador abierto: %u Hz, %u canales\n", rate, channels);
    return svc;
}

// Parámetros multistream (0x110 o, en la forma Ex, 0x118 bytes): tasa, canales, flujos, flujos estéreo,
// [Ex: frame grande + 7 de relleno], tabla de canales (256 bytes).
static Service *open_multi(const uint8_t *p, uint64_t n, bool ex) {
    if (!p || n < (ex ? 0x18u : 0x10u)) { LOG("[hwopus] parámetros multistream ausentes\n"); return NULL; }
    uint32_t rate = *(const uint32_t *)p, ch = *(const uint32_t *)(p + 4), streams = *(const uint32_t *)(p + 8),
             stereo = *(const uint32_t *)(p + 12);
    const uint8_t *map = p + (ex ? 0x18 : 0x10);
    int err = 0;
    OpusMSDecoder *d = opus_multistream_decoder_create((opus_int32)rate, (int)ch, (int)streams, (int)stereo, map, &err);
    if (!d) {
        LOG("[hwopus] opus_multistream_decoder_create(%u Hz, %u ch, %u flujos, %u estéreo): %s\n", rate, ch, streams, stereo,
            opus_strerror(err));
        return NULL;
    }
    HwOpus *h = calloc(1, sizeof *h);
    h->ms = d; h->rate = rate; h->channels = ch;
    Service *svc = service_new("IHardwareOpusDecoder", hwopus_decoder, h);
    svc->on_close = hwopus_close;
    LOG("[hwopus] decodificador multistream abierto: %u Hz, %u canales, %u flujos (%u estéreo)\n", rate, ch, streams, stereo);
    return svc;
}

SERVICE("hwopus", hwopus_manager) {
    (void)s;
    IpcBuf *x = in_buf(req, 0);
    const uint8_t *xp = x && x->size ? (const uint8_t *)(uintptr_t)x->addr : NULL;
    uint64_t xn = x ? x->size : 0;
    if (hwlog())
        LOG("[hwopus] gestor: orden %u (%u bytes: %08x %08x %08x %08x; x %llu bytes)\n", req->cmd, req->data_size,
            req->data_size >= 4 ? IN(uint32_t, 0) : 0, req->data_size >= 8 ? IN(uint32_t, 4) : 0,
            req->data_size >= 12 ? IN(uint32_t, 8) : 0, req->data_size >= 16 ? IN(uint32_t, 12) : 0, (unsigned long long)xn);
    switch (req->cmd) {
    case 0: case 4: {  // OpenHardwareOpusDecoder(Ex)(parámetros, tamaño de la memoria de transferencia, su handle)
        uint32_t rate = IN(uint32_t, 0), ch = IN(uint32_t, 4);
        if (!valid_rate(rate) || ch < 1 || ch > 2) { resp->result = 0x14c6f; LOG("[hwopus] parámetros no válidos: %u Hz, %u ch\n", rate, ch); break; }
        Service *svc = open_single(rate, ch);
        if (!svc) { resp->result = 0x14c6f; break; }
        ipc_return_object(resp, svc);
        break;
    }
    case 1: case 5: case 8: {  // GetWorkBufferSize / Ex / ExEx
        uint32_t rate = IN(uint32_t, 0), ch = IN(uint32_t, 4);
        bool large = req->cmd != 1 && req->data_size >= 9 && IN(uint8_t, 8) != 0;
        if (!valid_rate(rate) || ch < 1 || ch > 2) { resp->result = 0x14c6f; LOG("[hwopus] tamaño: parámetros no válidos: %u Hz, %u ch\n", rate, ch); break; }
        OUT(uint32_t, 0) = work_size(rate, ch, 0, 0, large);
        resp_size(resp, 4);
        break;
    }
    case 2: case 6: {  // OpenHardwareOpusDecoderForMultiStream(Ex)(tamaño de la memoria de transferencia; X: parámetros)
        Service *svc = open_multi(xp, xn, req->cmd == 6);
        if (!svc) { resp->result = 0x14c6f; break; }
        ipc_return_object(resp, svc);
        break;
    }
    case 3: case 7: case 9: {  // GetWorkBufferSizeForMultiStream / Ex / ExEx (X: parámetros)
        if (!xp || xn < 0x10) { resp->result = 0x14c6f; LOG("[hwopus] tamaño multistream sin parámetros\n"); break; }
        uint32_t rate = *(const uint32_t *)xp, ch = *(const uint32_t *)(xp + 4), streams = *(const uint32_t *)(xp + 8),
                 stereo = *(const uint32_t *)(xp + 12);
        bool large = req->cmd != 3 && xn >= 0x11 && xp[0x10] != 0;
        OUT(uint32_t, 0) = work_size(rate, ch, streams, stereo, large);
        if (hwlog())
            LOG("[hwopus] tamaño multistream: %u Hz, %u ch, %u flujos (%u estéreo)%s, tabla %02x %02x %02x %02x -> %u bytes\n",
                rate, ch, streams, stereo, large ? ", tramas grandes" : "", xn > 0x1b ? xp[0x18] : 0, xn > 0x1b ? xp[0x19] : 0,
                xn > 0x1b ? xp[0x1a] : 0, xn > 0x1b ? xp[0x1b] : 0, OUT(uint32_t, 0));
        resp_size(resp, 4);
        break;
    }
    default: LOG("[hwopus] gestor: orden %u desconocida (ok)\n", req->cmd);
    }
}
