// a8dj-loopback — measure the Audio 8 DJ through a physical loopback cable.
//
// Plays a test signal on one output pair and records an input pair through
// the driver, then reports:
//   * round-trip latency: the hardware path (sample time out -> sample time
//     in) and what an app actually waits (adds both safety offsets and two
//     buffers, taken from the IOProc's own timestamps);
//   * dropouts: a steady sine is demodulated millisecond by millisecond, and
//     any millisecond whose level or phase jumps is counted;
//   * frequency response: white noise, cross-spectrum / auto-spectrum
//     (Welch), relative to 1 kHz, with coherence so cable noise is visible.
//
// Usage: a8dj-loopback [--out 7] [--in 7] [--buffer 64] [--seconds 30]
//                      [--rate 48000] [--level -12] [--wav file.raw]
// Channels are 1-based; the pair (n, n+1) is driven. Build: scripts/build.sh

#include <Accelerate/Accelerate.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CH 8

static AudioObjectID gDev;
static double gRate = 48000;
static int gOut = 6, gIn = 6;         // 0-based first channel of each pair
static double gLevel = 0.25;          // linear peak for the sine
static size_t gTotal;                 // frames of signal
static float *gRef;                   // what was sent, by sample time - base
static float *gRec, *gRec2;           // what came back on in, in+1
static unsigned char *gHave;          // gRec[i] was delivered by an IOProc
static _Atomic int gStarted, gDone;
static double gBase = -1;             // sample time of the first output frame
static _Atomic long gCycles, gTimeJumps, gOverloads;
static double gLastIn = -1, gLastOut = -1;
static _Atomic long gOutInGapMin = 1L << 40, gOutInGapMax = 0; // Tout - Tin per cycle
static UInt32 gBufferFrames;

// Signal plan, in seconds from the first output frame.
#define T_SETTLE   1.0
#define T_CLICKS   3.0     // 1.0 .. 4.0: bursts every 250 ms
#define T_NOISE    10.0    // 4.0 .. 14.0: white noise
#define F_SINE     997.0   // after that: steady sine

static uint32_t gLcg = 0x12345678u;
static float Noise(void)
{
    gLcg = gLcg * 1664525u + 1013904223u;
    return ((float)(gLcg >> 8) / 8388608.0f - 1.0f);
}

static void BuildSignal(void)
{
    size_t clicksEnd = (size_t)((T_SETTLE + T_CLICKS) * gRate);
    size_t noiseEnd = clicksEnd + (size_t)(T_NOISE * gRate);
    size_t period = (size_t)(0.25 * gRate);
    for (size_t i = 0; i < gTotal; i++) {
        float v = 0;
        if (i < (size_t)(T_SETTLE * gRate)) v = 0;
        else if (i < clicksEnd) {
            // 64-sample noise burst: sharp autocorrelation, no ringing guesswork.
            size_t k = (i - (size_t)(T_SETTLE * gRate)) % period;
            v = k < 64 ? 0.5f * Noise() : 0;
        } else if (i < noiseEnd) {
            v = 0.3f * Noise();
        } else {
            v = (float)(gLevel * sin(2 * M_PI * F_SINE * (double)i / gRate));
        }
        gRef[i] = v;
    }
}

static OSStatus IOProc(AudioObjectID dev, const AudioTimeStamp *now,
                       const AudioBufferList *inData, const AudioTimeStamp *inTime,
                       AudioBufferList *outData, const AudioTimeStamp *outTime, void *ctx)
{
    (void)dev; (void)now; (void)ctx;
    gCycles++;
    double tin = inTime->mSampleTime, tout = outTime->mSampleTime;
    if (gBase < 0) gBase = tout;
    UInt32 n = outData->mNumberBuffers ? outData->mBuffers[0].mDataByteSize / (CH * sizeof(float)) : 0;
    if (gLastOut >= 0 && fabs(tout - (gLastOut + n)) > 0.5) gTimeJumps++;
    gLastOut = tout;
    long gap = (long)(tout - tin);
    if (gap < gOutInGapMin) gOutInGapMin = gap;
    if (gap > gOutInGapMax) gOutInGapMax = gap;

    if (outData->mNumberBuffers) {
        float *o = outData->mBuffers[0].mData;
        memset(o, 0, outData->mBuffers[0].mDataByteSize);
        for (UInt32 i = 0; i < n; i++) {
            double idx = tout - gBase + i;
            float v = (idx >= 0 && idx < (double)gTotal) ? gRef[(size_t)idx] : 0;
            o[i * CH + gOut] = v;
            o[i * CH + gOut + 1] = v;
        }
        if (tout - gBase > (double)gTotal + gRate * 0.5) gDone = 1;
    }
    if (inData->mNumberBuffers) {
        const float *in = inData->mBuffers[0].mData;
        UInt32 m = inData->mBuffers[0].mDataByteSize / (CH * sizeof(float));
        if (gLastIn >= 0 && fabs(tin - (gLastIn + m)) > 0.5) gTimeJumps++;
        gLastIn = tin;
        for (UInt32 j = 0; j < m; j++) {
            double idx = tin - gBase + j;
            if (idx < 0 || idx >= (double)gTotal) continue;
            gRec[(size_t)idx] = in[j * CH + gIn];
            gRec2[(size_t)idx] = in[j * CH + gIn + 1];
            gHave[(size_t)idx] = 1;
        }
    }
    gStarted = 1;
    return noErr;
}

static OSStatus Overload(AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a, void *c)
{
    (void)o; (void)n; (void)a; (void)c;
    gOverloads++;
    return noErr;
}

static AudioObjectID FindDevice(void)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
    AudioObjectID ids[64];
    UInt32 count = size / sizeof(AudioObjectID);
    if (count > 64) count = 64;
    size = count * sizeof(AudioObjectID);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
    for (UInt32 i = 0; i < count; i++) {
        CFStringRef uid = NULL;
        UInt32 s = sizeof(uid);
        AudioObjectPropertyAddress u = { kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &u, 0, NULL, &s, &uid) || !uid) continue;
        Boolean match = CFStringCompare(uid, CFSTR("Audio8DJ:0"), 0) == kCFCompareEqualTo;
        CFRelease(uid);
        if (match) return ids[i];
    }
    return kAudioObjectUnknown;
}

static UInt32 GetU32(AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
    UInt32 v = 0, s = sizeof(v);
    AudioObjectGetPropertyData(gDev, &a, 0, NULL, &s, &v);
    return v;
}

#pragma mark - Analysis

// Lag (in frames) of rec relative to ref over [from, to), searched
// -minLag..maxLag. It can be negative: the published input timeline counts a
// frame when it reaches the host, so the input clock runs a little behind the
// moment the converter sampled it.
static long FindLag(size_t from, size_t to, long minLag, long maxLag, double *outPeak)
{
    long best = 0; double bestV = 0, energy = 0;
    for (size_t i = from; i < to; i++) energy += (double)gRef[i] * gRef[i];
    for (long lag = -minLag; lag < maxLag; lag++) {
        double acc = 0;
        for (size_t i = from; i < to && (long)i + lag < (long)gTotal; i++)
            acc += (double)gRef[i] * gRec[(long)i + lag];
        if (fabs(acc) > fabs(bestV)) { bestV = acc; best = lag; }
    }
    if (outPeak) *outPeak = energy > 0 ? bestV / energy : 0;
    return best;
}

static void Response(size_t from, size_t to, long lag)
{
    const int N = 8192, log2n = 13;
    FFTSetup fft = vDSP_create_fftsetup(log2n, kFFTRadix2);
    float *win = malloc(sizeof(float) * N), *a = malloc(sizeof(float) * N), *b = malloc(sizeof(float) * N);
    float *ar = malloc(sizeof(float) * N / 2), *ai = malloc(sizeof(float) * N / 2);
    float *br = malloc(sizeof(float) * N / 2), *bi = malloc(sizeof(float) * N / 2);
    double *pxx = calloc(N / 2, sizeof(double)), *pyy = calloc(N / 2, sizeof(double));
    double *sxr = calloc(N / 2, sizeof(double)), *sxi = calloc(N / 2, sizeof(double));
    vDSP_hann_window(win, N, vDSP_HANN_NORM);
    int segs = 0;
    for (size_t s = from; (long)(s + N) + lag < (long)to; s += N / 2, segs++) {
        for (int i = 0; i < N; i++) { a[i] = gRef[s + i] * win[i]; b[i] = gRec[(long)(s + i) + lag] * win[i]; }
        DSPSplitComplex A = { ar, ai }, B = { br, bi };
        vDSP_ctoz((DSPComplex *)a, 2, &A, 1, N / 2);
        vDSP_ctoz((DSPComplex *)b, 2, &B, 1, N / 2);
        vDSP_fft_zrip(fft, &A, 1, log2n, FFT_FORWARD);
        vDSP_fft_zrip(fft, &B, 1, log2n, FFT_FORWARD);
        for (int k = 1; k < N / 2; k++) {
            pxx[k] += (double)ar[k] * ar[k] + (double)ai[k] * ai[k];
            pyy[k] += (double)br[k] * br[k] + (double)bi[k] * bi[k];
            // Y * conj(X)
            sxr[k] += (double)br[k] * ar[k] + (double)bi[k] * ai[k];
            sxi[k] += (double)bi[k] * ar[k] - (double)br[k] * ai[k];
        }
    }
    double binHz = gRate / N;
    int k1k = (int)lround(1000.0 / binHz);
    double ref = 0;
    for (int k = k1k - 2; k <= k1k + 2; k++) ref += sqrt(sxr[k] * sxr[k] + sxi[k] * sxi[k]) / pxx[k];
    ref /= 5;
    printf("\n周波数特性 (1 kHz 基準, %d 区間平均, 帯域内平均):\n", segs);
    printf("   周波数      利得     コヒーレンス\n");
    static const double bands[] = { 20, 30, 50, 100, 200, 500, 1000, 2000, 4000, 6000, 8000, 10000,
                                    12000, 14000, 16000, 17000, 18000, 19000, 20000, 21000, 22000,
                                    23000, 30000, 40000, 44000 };
    for (size_t bi2 = 0; bi2 < sizeof(bands) / sizeof(bands[0]); bi2++) {
        double f = bands[bi2];
        if (f >= gRate / 2 * 0.98) break;
        double lo = f / pow(2, 1.0 / 12), hi = f * pow(2, 1.0 / 12);
        double g = 0, coh = 0; int nb = 0;
        for (int k = 1; k < N / 2; k++) {
            double fk = k * binHz;
            if (fk < lo || fk > hi) continue;
            double mag = sqrt(sxr[k] * sxr[k] + sxi[k] * sxi[k]);
            g += mag / pxx[k];
            coh += mag * mag / (pxx[k] * pyy[k]);
            nb++;
        }
        if (!nb) continue;
        printf("  %7.0f Hz  %+7.2f dB   %.3f\n", f, 20 * log10(g / nb / ref), coh / nb);
    }
    // -3 dB point above 1 kHz.
    for (int k = k1k; k < N / 2; k++) {
        double mag = sqrt(sxr[k] * sxr[k] + sxi[k] * sxi[k]) / pxx[k];
        if (20 * log10(mag / ref) < -3) { printf("  -3 dB 点: 約 %.0f Hz\n", k * binHz); break; }
    }
    vDSP_destroy_fftsetup(fft);
    free(win); free(a); free(b); free(ar); free(ai); free(br); free(bi);
    free(pxx); free(pyy); free(sxr); free(sxi);
}

// Demodulate the sine one millisecond at a time against the reference; a
// dropout, a repeated or skipped chunk, all show as a level or phase step.
static void Dropouts(size_t from, size_t to, long lag)
{
    size_t blk = (size_t)(gRate / 1000);
    size_t nb = (size_t)(((long)to - (long)from - (lag > 0 ? lag : 0)) / (long)blk);
    double *amp = malloc(sizeof(double) * nb), *ph = malloc(sizeof(double) * nb);
    int *missing = calloc(nb, sizeof(int));
    for (size_t b = 0; b < nb; b++) {
        double ci = 0, cq = 0, ri = 0, rq = 0;
        for (size_t i = 0; i < blk; i++) {
            size_t t = from + b * blk + i;
            double w = 2 * M_PI * F_SINE * (double)t / gRate;
            double x = gRec[(long)t + lag];
            if (!gHave[(long)t + lag]) missing[b] = 1;
            ci += x * cos(w); cq += x * sin(w);
            ri += gRef[t] * cos(w); rq += gRef[t] * sin(w);
        }
        amp[b] = sqrt(ci * ci + cq * cq) / sqrt(ri * ri + rq * rq);
        ph[b] = atan2(cq, ci) - atan2(rq, ri);
    }
    // Reference level/phase: median-ish via mean of the middle.
    double sa = 0, sc = 0, ss = 0;
    for (size_t b = 0; b < nb; b++) { sa += amp[b]; sc += cos(ph[b]); ss += sin(ph[b]); }
    double ma = sa / nb, mp = atan2(ss, sc);
    int events = 0, inEvent = 0, missingBlocks = 0;
    double worstDb = 0, worstPh = 0;
    for (size_t b = 0; b < nb; b++) {
        double db = 20 * log10(amp[b] / ma + 1e-12);
        double dp = remainder(ph[b] - mp, 2 * M_PI) * 180 / M_PI;
        int bad = fabs(db) > 1.0 || fabs(dp) > 10.0;
        if (missing[b]) missingBlocks++;
        if (bad) {
            if (fabs(db) > fabs(worstDb)) worstDb = db;
            if (fabs(dp) > fabs(worstPh)) worstPh = dp;
            if (!inEvent) {
                events++;
                if (events <= 10)
                    printf("  音切れ候補 t=%.3f s  レベル %+.1f dB  位相 %+.0f°\n",
                           (double)(from + b * blk) / gRate, db, dp);
            }
        }
        inEvent = bad;
    }
    printf("\n音切れ検査 (%.0f Hz 正弦波, %.1f 秒, 1 ms 単位):\n", F_SINE, (double)(nb * blk) / gRate);
    printf("  乱れた区間: %d 回  最大レベル変化 %+.1f dB  最大位相変化 %+.0f°  未受信ブロック %d\n",
           events, worstDb, worstPh, missingBlocks);
    printf("  正弦波の利得 %.2f dB (送出 %.1f dBFS)\n", 20 * log10(ma), 20 * log10(gLevel));
    free(amp); free(ph); free(missing);
}

int main(int argc, char **argv)
{
    UInt32 buffer = 64;
    double seconds = 30, levelDb = -12;
    const char *wav = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--out")) gOut = atoi(argv[i + 1]) - 1;
        else if (!strcmp(argv[i], "--in")) gIn = atoi(argv[i + 1]) - 1;
        else if (!strcmp(argv[i], "--buffer")) buffer = (UInt32)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--rate")) gRate = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--level")) levelDb = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--wav")) wav = argv[i + 1];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (gOut < 0 || gOut > CH - 2 || gIn < 0 || gIn > CH - 2) { fprintf(stderr, "bad channel\n"); return 2; }
    double minSeconds = T_SETTLE + T_CLICKS + T_NOISE + 5;
    if (seconds < minSeconds) seconds = minSeconds;
    gLevel = pow(10, levelDb / 20);

    gDev = FindDevice();
    if (gDev == kAudioObjectUnknown) { fprintf(stderr, "Audio 8 DJ が見つかりません\n"); return 1; }
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    Float64 cur = 0; UInt32 sz = sizeof(cur);
    AudioObjectGetPropertyData(gDev, &ra, 0, NULL, &sz, &cur);
    if (cur != gRate) {
        AudioObjectSetPropertyData(gDev, &ra, 0, NULL, sizeof(gRate), &gRate);
        for (int i = 0; i < 40 && cur != gRate; i++) {
            usleep(100000);
            sz = sizeof(cur);
            AudioObjectGetPropertyData(gDev, &ra, 0, NULL, &sz, &cur);
        }
        if (cur != gRate) { fprintf(stderr, "サンプルレートを %.0f にできません\n", gRate); return 1; }
    }
    AudioObjectPropertyAddress ba = { kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(gDev, &ba, 0, NULL, sizeof(buffer), &buffer);
    sz = sizeof(gBufferFrames);
    AudioObjectGetPropertyData(gDev, &ba, 0, NULL, &sz, &gBufferFrames);
    AudioObjectPropertyAddress oa = { kAudioDeviceProcessorOverload,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(gDev, &oa, Overload, NULL);

    gTotal = (size_t)(seconds * gRate);
    gRef = calloc(gTotal, sizeof(float));
    gRec = calloc(gTotal, sizeof(float));
    gRec2 = calloc(gTotal, sizeof(float));
    gHave = calloc(gTotal, 1);
    BuildSignal();

    UInt32 latIn = GetU32(kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput);
    UInt32 latOut = GetU32(kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput);
    UInt32 safIn = GetU32(kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput);
    UInt32 safOut = GetU32(kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
    printf("Audio 8 DJ  %.0f Hz  バッファ %u  出力 %d-%d → 入力 %d-%d  %.0f 秒\n",
           gRate, gBufferFrames, gOut + 1, gOut + 2, gIn + 1, gIn + 2, seconds);
    printf("報告値: safety in=%u out=%u  latency in=%u out=%u\n", safIn, safOut, latIn, latOut);

    AudioDeviceIOProcID proc = NULL;
    if (AudioDeviceCreateIOProcID(gDev, IOProc, NULL, &proc) || AudioDeviceStart(gDev, proc)) {
        fprintf(stderr, "IO を開始できません\n");
        return 1;
    }
    while (!gDone) usleep(100000);
    AudioDeviceStop(gDev, proc);
    AudioDeviceDestroyIOProcID(gDev, proc);
    AudioObjectRemovePropertyListener(gDev, &oa, Overload, NULL);

    if (wav) {
        FILE *f = fopen(wav, "wb");
        if (f) {
            for (size_t i = 0; i < gTotal; i++) { fwrite(&gRef[i], 4, 1, f); fwrite(&gRec[i], 4, 1, f); }
            fclose(f);
            printf("生データ (float32, ref/rec インターリーブ): %s\n", wav);
        }
    }

    float peak = 0;
    for (size_t i = 0; i < gTotal; i++) if (fabsf(gRec[i]) > peak) peak = fabsf(gRec[i]);
    printf("入力ピーク %.1f dBFS%s\n", 20 * log10(peak + 1e-12),
           peak > 0.89f ? "  ※クリップの恐れ。出力ボリュームを下げてください" :
           peak < 0.01f ? "  ※信号がほぼ無い。配線とボリュームを確認してください" : "");

    size_t c0 = (size_t)(T_SETTLE * gRate), c1 = (size_t)((T_SETTLE + T_CLICKS) * gRate);
    double corr = 0;
    long lag = FindLag(c0, c1, (long)(0.02 * gRate), (long)(0.1 * gRate), &corr);
    double cross = 0, e1 = 0;
    for (size_t i = c0; (long)i + lag < (long)c1; i++) {
        cross += (double)gRec2[(long)i + lag] * gRec2[(long)i + lag];
        e1 += (double)gRec[(long)i + lag] * gRec[(long)i + lag];
    }
    printf("\n遅延:\n");
    printf("  ハードウェア経路 (出力サンプル時刻→入力サンプル時刻): %ld フレーム = %.3f ms (相関 %.2f)\n",
           lag, lag / gRate * 1000, corr);
    printf("  IOProc の出力時刻 - 入力時刻: %ld .. %ld フレーム\n", (long)gOutInGapMin, (long)gOutInGapMax);
    long app = lag + (long)gOutInGapMax;
    printf("  アプリから見た往復遅延 ≈ %ld フレーム = %.2f ms\n", app, app / gRate * 1000);
    printf("  (報告値の合計 = safety %u + %u + latency %u + %u + バッファ 2×%u = %u フレーム)\n",
           safIn, safOut, latIn, latOut, gBufferFrames,
           safIn + safOut + latIn + latOut + 2 * gBufferFrames);
    printf("  入力 %d と %d のレベル比 %.1f dB (同じ信号を送っているので 0 dB 付近が正常)\n",
           gIn + 2, gIn + 1, 10 * log10((cross + 1e-20) / (e1 + 1e-20)));
    printf("IO サイクル %ld  時刻の飛び %ld  CoreAudio 過負荷 %ld\n",
           (long)gCycles, (long)gTimeJumps, (long)gOverloads);

    size_t n0 = c1, n1 = c1 + (size_t)(T_NOISE * gRate);
    // Analyses index rec at t + lag; shift the window so that stays in range.
    size_t shift = lag < 0 ? (size_t)(-lag) : 0;
    Response(n0 + shift, n1, lag);
    Dropouts(n1 + shift + (size_t)(0.2 * gRate), gTotal - (size_t)(0.1 * gRate), lag);
    return 0;
}
