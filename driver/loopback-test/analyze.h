// 共通解析: 二階差分で継ぎ目を検出し、既知 1kHz 正弦の位相から継ぎ目前後の
// 位相ジャンプをフレーム換算で出す（1 周期 = 48 frames なので ±24 で折り返す）。
// 先頭 1 バッファは起動過渡として除外。
static double PhaseAt(const float* cap, unsigned n, double A, double w) {
    double s = cap[2*n] / A, sp = cap[2*(n-1)] / A;
    if (s > 1) s = 1; if (s < -1) s = -1;
    double c = (s * cos(w) - sp) / sin(w);
    return atan2(s, c);
}
static void AnalyzeCommon(const float* cap, unsigned frames, unsigned bufFrames, double A, double w) {
    unsigned gl = 0, prev = 0; double maxD2 = 0; unsigned offs[16]; unsigned no = 0;
    printf("analyze: %u frames, skip first %u\n", frames, bufFrames);
    for (unsigned n = bufFrames + 3; n + 1 < frames; n++) {
        double d2 = fabs(cap[2*n] - 2*cap[2*(n-1)] + cap[2*(n-2)]);
        if (d2 > maxD2) maxD2 = d2;
        if (d2 > 0.06) {
            gl++;
            double pb = PhaseAt(cap, n-2, A, w), pa = PhaseAt(cap, n+1, A, w);
            double jump = pa - (pb + 3*w); while (jump > M_PI) jump -= 2*M_PI; while (jump < -M_PI) jump += 2*M_PI;
            if (gl <= 14) printf("  seam @%u (buf#%u +%u) d2=%.3f dt=%u phaseJump=%+.1f frames (mod 48)\n", n, n / bufFrames, n % bufFrames, d2, n - prev, jump / w);
            if (no < 16 && (n - prev) > 2) offs[no++] = n % bufFrames;
            prev = n; n += 2; // 同一継ぎ目の連続検出をまとめる
        }
    }
    double sq = 0; for (unsigned n = 0; n < frames; n++) sq += cap[2*n]*cap[2*n];
    printf("summary: seams=%u maxD2=%.4f rms=%.4f  offsets:", gl, maxD2, sqrt(sq / frames));
    for (unsigned i = 0; i < no; i++) printf(" %u", offs[i]); printf("\n");
}
// ランプ解析: 書き手がフレーム番号 (ctr & 0xFFFFF) をそのまま書く。経路は bit-exact
// なので delta = x[n]-x[n-1] が 1 以外の点は全て継ぎ目で、(delta-1) が符号付きの
// 正確なフレームオフセット。ゼロ区間（無音）は別集計。
static void AnalyzeRamp(const float* cap, unsigned frames, unsigned bufFrames) {
    unsigned seams = 0, zero = 0; int hist[8] = {0}; double vals[8] = {0}; unsigned nv = 0;
    printf("ramp analyze: %u frames, skip first %u\n", frames, bufFrames);
    for (unsigned n = bufFrames + 1; n < frames; n++) {
        double a = cap[2*(n-1)], b = cap[2*n];
        if (a == 0 && b == 0) { zero++; continue; }
        if (a == 0 || b == 0) continue; // 無音境界（書き手の開始/停止）は除外
        double d = b - a;
        if (d == 1) continue;
        double off = d - 1;
        if (off < -0x80000) off += 0x100000; // ラップ補正
        seams++;
        if (seams <= 12) printf("  seam @%u (buf#%u +%u) offset=%+.0f frames\n", n, n / bufFrames, n % bufFrames, off);
        unsigned k; for (k = 0; k < nv; k++) if (vals[k] == off) { hist[k]++; break; }
        if (k == nv && nv < 8) { vals[nv] = off; hist[nv] = 1; nv++; }
    }
    printf("ramp summary: seams=%u zeroFrames=%u  offset histogram:", seams, zero);
    for (unsigned k = 0; k < nv; k++) printf(" [%+.0f x%d]", vals[k], hist[k]); printf("\n");
}
