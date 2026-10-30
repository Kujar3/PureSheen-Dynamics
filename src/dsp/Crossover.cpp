#include "dsp/Crossover.h"

#include "core/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

// Stereo N-band crossover (dsp-common).
//
// Analog : Linkwitz-Riley 24 dB/oct (LR4 = Butterworth 2 squared) tree. band0 = LP1 AP2 AP3, band1 = HP1 LP2 AP3,
//          band2 = HP1 HP2 LP3, band3 = HP1 HP2 HP3, where APk is the all-pass that LPk + HPk sums to. The sum is
//          AP1 AP2 AP3 x (flat magnitude). Because an LR low pass and high pass are each "AP times a real, positive
//          magnitude", every band also carries the same phase AP1 AP2 AP3.
// Hybrid : the same compensated tree with Linkwitz-Riley 48 dB/oct sections (LR8 = Butterworth 4 squared): steep band
//          separation like the Digital crossover, at zero latency; sum = 4th-order all-pass per crossover, bands
//          phase-aligned. (Choice: an LR4 Hybrid would be numerically identical to Analog, since the compensated LR4
//          tree is already phase-aligned; one constant below switches it back.)
//          All sections are trapezoidal state-variable filters (= bilinear, prewarped Butterworth) in double
//          precision, so LP^2 + HP^2 = AP holds exactly, not just approximately. Frequency changes glide in the log
//          domain (20 ms time constant) with the coefficients updated every sample; the SVF structure keeps such
//          modulation free of transients. The full 4-band tree always runs, unused splits are merged back
//          (LPk + HPk = APk), so a band-count change never jumps the phase of the sum; 1 band crossfades (20 ms) to the
//          untouched input.
// Digital: complementary linear-phase FIR bank. One windowed-sinc low pass per crossover (Kaiser, 70 dB), all centred on
//          the same delay D: band0 = y1, band_k = y_(k+1) - y_k, last band = x(n - D) - y_last, so the bands sum to the
//          delayed input exactly (only float rounding remains). Constant-Q slope: the transition (stopband edge to
//          passband edge) is 1.2 x the crossover frequency wide (0.4 fc .. 1.6 fc), limited at the bottom by the FIR
//          length and at the top by Nyquist. The FIR length is fixed per sample-rate family (x1 <= 50 kHz, x2, x4,
//          x8) so the latency is the same time at every rate and never depends on the crossover frequencies:
//              window 6143 taps x family, latency D = 3327 x family samples (75.4 ms at 44.1 kHz, 69.3 ms at 48 kHz).
//          Tradeoff: the longest window sets the steepest slope at the bottom: a 20 Hz crossover is 34 Hz wide at
//          48 kHz (3 .. 37 Hz; 31 Hz at 44.1 kHz), still well defined (DC passes the low band at 0 dB). A window
//          twice as long would halve that width at twice the latency.
//          Convolution: uniformly partitioned FFT (partition P = 256 x family, FFT 2P), both channels packed into one
//          complex FFT (left = real, right = imaginary; the kernels are real). The kernels have P leading zeros, so
//          the block buffering costs no extra latency (latency = (fir length - 1) / 2 for the padded kernel).
//          Kernels are designed on the audio thread, incrementally (a few FFTs per block, no allocation) into the
//          spare of two slots per crossover; the new kernel is crossfaded in over one partition. The crossfade acts on
//          the low-pass outputs, so the bands still sum exactly to the delayed input during the fade.
//
// The input may alias any band buffer (each sample is read before any band sample at that index is written).
namespace ps
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kLrOrderAnalog = 4;
    constexpr int kLrOrderHybrid = 8;

    //==========================================================================
    // Topology-preserving (trapezoidal) state-variable filter, A. Simper's form. Its LP / HP / AP outputs are exactly the
    // bilinear-transformed analog SVF, so the Linkwitz-Riley identities hold to rounding when static, and it stays
    // smooth when g changes every sample (no coefficient-jump transients).
    struct SvfCoeffs
    {
        double k = 1.4142135623730951, a1 = 1.0, a2 = 0.0, a3 = 0.0;

        void set (double g, double q) noexcept
        {
            k = 1.0 / q;
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
    };

    struct SvfState
    {
        double ic1 = 0.0, ic2 = 0.0;
    };

    // returns band pass, writes low pass
    inline double svfTick (const SvfCoeffs& c, SvfState& s, double x, double& lp) noexcept
    {
        const double v3 = x - s.ic2;
        const double v1 = c.a1 * s.ic1 + c.a2 * v3;
        const double v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
        s.ic1 = 2.0 * v1 - s.ic1;
        s.ic2 = 2.0 * v2 - s.ic2;
        lp = v2;
        return v1;
    }

    void flush (double& v) noexcept
    {
        if (std::abs (v) < 1.0e-30)
            v = 0.0;
    }

    //==========================================================================
    // Linkwitz-Riley tree (Analog / Hybrid).
    struct IirEngine
    {
        static constexpr int kMaxQ = 2;              // Butterworth sections per LP / HP (LR8)
        static constexpr int kMaxSecs = 2 * kMaxQ;   // LR = Butterworth squared

        double sr = 48000.0;
        int numQ = 1;
        double qs[kMaxQ] { 0.7071067811865476, 0.0 };
        double curLog[kMaxCrossovers] {}, tgtLog[kMaxCrossovers] {};
        double glide = 0.0;
        SvfCoeffs co[kMaxCrossovers][kMaxQ];

        struct ChannelState
        {
            SvfState lp[kMaxCrossovers][kMaxSecs];
            SvfState hp[kMaxCrossovers][kMaxSecs];
            SvfState ap01[kMaxQ], ap02[kMaxQ], ap12[kMaxQ];   // band0 via AP2, AP3; band1 via AP3
        };
        ChannelState st[2] {};

        double passMix = 1.0, passTarget = 1.0, passStep = 0.0;

        void prepare (double sampleRate) noexcept
        {
            sr = sampleRate;
            glide = 1.0 - std::exp (-1.0 / (0.020 * sr));   // 20 ms, per sample
            passStep = 1.0 / (0.020 * sr);
            for (int k = 0; k < kMaxCrossovers; ++k)
                curLog[k] = tgtLog[k] = std::log (k == 0 ? 120.0 : (k == 1 ? 1000.0 : 6000.0));
            setOrder (kLrOrderAnalog);
            reset();
        }

        void setOrder (int lrOrder) noexcept
        {
            const int bw = lrOrder / 2;
            numQ = std::clamp (bw / 2, 1, kMaxQ);
            for (int i = 0; i < numQ; ++i)
            {
                const double theta = (2.0 * i + 1.0) * kPi / (2.0 * bw);
                qs[i] = 1.0 / (2.0 * std::cos (theta));
            }
            for (int k = 0; k < kMaxCrossovers; ++k)
                design (k);
        }

        void design (int k) noexcept
        {
            const double hz = std::min (std::exp (curLog[k]), 0.45 * sr);
            const double g = std::tan (kPi * hz / sr);
            for (int i = 0; i < numQ; ++i)
                co[k][i].set (g, qs[i]);
        }

        void reset() noexcept
        {
            for (auto& c : st)
                c = ChannelState {};
            passMix = passTarget;
        }

        void snap() noexcept
        {
            for (int k = 0; k < kMaxCrossovers; ++k)
            {
                curLog[k] = tgtLog[k];
                design (k);
            }
            passMix = passTarget;
        }

        void updateGlide() noexcept
        {
            for (int k = 0; k < kMaxCrossovers; ++k)
            {
                if (curLog[k] == tgtLog[k])
                    continue;

                curLog[k] += (tgtLog[k] - curLog[k]) * glide;
                if (std::abs (tgtLog[k] - curLog[k]) < 1.0e-7)
                    curLog[k] = tgtLog[k];
                design (k);
            }
        }

        inline double lowChain (int k, SvfState* s, double x) const noexcept
        {
            for (int i = 0; i < 2 * numQ; ++i)
                svfTick (co[k][i % numQ], s[i], x, x);
            return x;
        }

        inline double highChain (int k, SvfState* s, double x) const noexcept
        {
            for (int i = 0; i < 2 * numQ; ++i)
            {
                const auto& c = co[k][i % numQ];
                double lp;
                const double bp = svfTick (c, s[i], x, lp);
                x = x - c.k * bp - lp;
            }
            return x;
        }

        inline double allpass (int k, SvfState* s, double x) const noexcept
        {
            for (int i = 0; i < numQ; ++i)
            {
                const auto& c = co[k][i];
                double lp;
                const double bp = svfTick (c, s[i], x, lp);
                x = x - 2.0 * c.k * bp;
            }
            return x;
        }

        // Full 4-band split of one sample of one channel.
        inline void split (ChannelState& c, double x, double* b) const noexcept
        {
            const double l0 = lowChain (0, c.lp[0], x);
            const double u1 = highChain (0, c.hp[0], x);
            b[0] = allpass (2, c.ap02, allpass (1, c.ap01, l0));

            const double l1 = lowChain (1, c.lp[1], u1);
            const double u2 = highChain (1, c.hp[1], u1);
            b[1] = allpass (2, c.ap12, l1);

            b[2] = lowChain (2, c.lp[2], u2);
            b[3] = highChain (2, c.hp[2], u2);
        }

        void process (const float* const* input, float* const* const* out, int numSamples, int numBands) noexcept
        {
            for (int i = 0; i < numSamples; ++i)
            {
                updateGlide();

                if (passMix != passTarget)
                {
                    passMix = passTarget > passMix ? std::min (passTarget, passMix + passStep)
                                                   : std::max (passTarget, passMix - passStep);
                }

                const double x[2] { (double) input[0][i], (double) input[1][i] };

                for (int ch = 0; ch < 2; ++ch)
                {
                    double b[kMaxBands];
                    split (st[ch], x[ch], b);

                    // merge the unused splits back (LPk + HPk = APk)
                    for (int j = kMaxBands - 1; j >= numBands && j > 0; --j)
                        b[j - 1] += b[j];

                    if (passMix > 0.0)
                    {
                        const double dry = passMix >= 1.0 ? 1.0 : 0.5 - 0.5 * std::cos (kPi * passMix);   // smooth fade
                        const double wet = 1.0 - dry;
                        b[0] = dry * x[ch] + wet * b[0];
                        for (int j = 1; j < numBands; ++j)
                            b[j] *= wet;
                    }

                    for (int j = 0; j < numBands; ++j)
                        out[j][ch][i] = (float) b[j];
                }
            }

            for (auto& c : st)
            {
                for (auto& xo : c.lp) for (auto& s : xo) { flush (s.ic1); flush (s.ic2); }
                for (auto& xo : c.hp) for (auto& s : xo) { flush (s.ic1); flush (s.ic2); }
                for (auto& s : c.ap01) { flush (s.ic1); flush (s.ic2); }
                for (auto& s : c.ap02) { flush (s.ic1); flush (s.ic2); }
                for (auto& s : c.ap12) { flush (s.ic1); flush (s.ic2); }
            }
        }
    };

    //==========================================================================
    using cf = std::complex<float>;

    inline void cmac (cf* acc, const cf* a, const cf* b, int n) noexcept
    {
        float* r = reinterpret_cast<float*> (acc);
        const float* x = reinterpret_cast<const float*> (a);
        const float* h = reinterpret_cast<const float*> (b);
        for (int i = 0; i < n; ++i)
        {
            const float xr = x[2 * i], xi = x[2 * i + 1], hr = h[2 * i], hi = h[2 * i + 1];
            r[2 * i] += xr * hr - xi * hi;
            r[2 * i + 1] += xr * hi + xi * hr;
        }
    }

    double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double q = 0.25 * x * x;
        for (int k = 1; k < 200; ++k)
        {
            term *= q / ((double) k * (double) k);
            sum += term;
            if (term < 1.0e-17 * sum)
                break;
        }
        return sum;
    }

    // Linear-phase FIR bank (Digital).
    struct FirEngine
    {
        static constexpr double kStopbandDb = 70.0;
        static constexpr double kRelativeWidth = 1.2;   // transition width / fc
        static constexpr int kWindowTable = 4096;
        static constexpr int kUnitsPerBlock = 6;          // design budget per crossover per partition

        double sr = 48000.0;
        int family = 1, P = 256, N2 = 512, K = 24, halfMax = 3071, D = 3327;
        double beta = 0.0, transitionFactor = 0.0, minWidthHz = 0.0;
        std::unique_ptr<Fft> fft;
        std::vector<double> window;   // Kaiser w(u), u = 0..1

        std::vector<cf> fdl;          // K x N2, newest at fdlPos
        int fdlPos = 0;
        std::vector<cf> inPrev, inCur, work, acc, accOld;
        int pos = 0;

        struct Slot
        {
            std::vector<cf> spectra;  // K x N2 (partition q = 1..K stored at q - 1)
            double hz = 0.0;
            int first = 1, last = 1;
        };

        struct Job
        {
            bool running = false, done = false;
            double hz = 0.0, omega = 0.0, sumTaps = 0.0;
            int half = 0, nextT = 0, nextQ = 0, first = 1, last = 1;
            double s = 0.0, c = 1.0, s1 = 0.0, c1 = 1.0;   // sin / cos recurrence
            std::vector<double> taps;                      // t = 0..halfMax (symmetric)
        };

        struct XoverState
        {
            Slot slot[2];
            int active = 0;
            double target = 1000.0;
            Job job;
            std::vector<cf> out;      // P (packed L/R) low-pass output of the current block
        };
        XoverState xo[kMaxCrossovers];
        int activeCount = 0;          // crossovers whose output is computed

        std::vector<cf> dry;
        int dryMask = 0, dryPos = 0;

        void prepare (double sampleRate)
        {
            sr = sampleRate;
            family = sr <= 50000.0 ? 1 : (sr <= 100000.0 ? 2 : (sr <= 200000.0 ? 4 : 8));
            P = 256 * family;
            N2 = 2 * P;
            halfMax = 3071 * family;
            D = P + halfMax;
            K = (P + 2 * halfMax + 1 + P - 1) / P - 1;   // partitions 1..K hold taps P .. P + 2 halfMax

            int order = 0;
            while ((1 << order) < N2)
                ++order;
            fft = std::make_unique<Fft> (order);

            beta = 0.1102 * (kStopbandDb - 8.7);
            transitionFactor = (kStopbandDb - 7.95) / 14.357;   // (N - 1) = factor * fs / width
            minWidthHz = transitionFactor * sr / (2.0 * halfMax);

            window.resize (kWindowTable + 1);
            const double i0b = besselI0 (beta);
            for (int i = 0; i <= kWindowTable; ++i)
            {
                const double u = (double) i / kWindowTable;
                window[(size_t) i] = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - u * u))) / i0b;
            }

            fdl.assign ((size_t) (K * N2), cf {});
            inPrev.assign ((size_t) P, cf {});
            inCur.assign ((size_t) P, cf {});
            work.assign ((size_t) N2, cf {});
            acc.assign ((size_t) N2, cf {});
            accOld.assign ((size_t) N2, cf {});

            int dsize = 1;
            while (dsize < D + 1)
                dsize <<= 1;
            dry.assign ((size_t) dsize, cf {});
            dryMask = dsize - 1;

            const double defaults[kMaxCrossovers] { 120.0, 1000.0, 6000.0 };
            for (int k = 0; k < kMaxCrossovers; ++k)
            {
                auto& x = xo[k];
                for (auto& s : x.slot)
                {
                    s.spectra.assign ((size_t) (K * N2), cf {});
                    s.hz = 0.0;
                }
                x.job.taps.assign ((size_t) (halfMax + 1), 0.0);
                x.job.running = x.job.done = false;
                x.out.assign ((size_t) P, cf {});
                x.active = 0;
                x.target = defaults[k];
                designNow (k);
            }
            reset();
        }

        void reset() noexcept
        {
            std::fill (fdl.begin(), fdl.end(), cf {});
            std::fill (inPrev.begin(), inPrev.end(), cf {});
            std::fill (inCur.begin(), inCur.end(), cf {});
            std::fill (dry.begin(), dry.end(), cf {});
            for (auto& x : xo)
                std::fill (x.out.begin(), x.out.end(), cf {});
            fdlPos = 0;
            pos = 0;
            dryPos = 0;
        }

        double clampHz (double hz) const noexcept
        {
            return std::clamp (hz, 10.0, 0.45 * sr);
        }

        //----------------------------------------------------------------------
        // Incremental kernel design. One unit = up to N2 taps generated, or one partition FFT.
        void startJob (int k, double hz) noexcept
        {
            auto& j = xo[k].job;
            hz = clampHz (hz);
            double width = kRelativeWidth * hz;
            width = std::min (width, 2.0 * (0.49 * sr - hz));
            width = std::max (width, minWidthHz);
            int half = (int) std::ceil (0.5 * transitionFactor * sr / width);
            half = std::clamp (half, 1, halfMax);

            j.running = true;
            j.done = false;
            j.hz = hz;
            j.omega = 2.0 * kPi * hz / sr;
            j.half = half;
            j.nextT = 0;
            j.sumTaps = 0.0;
            j.s = 0.0;
            j.c = 1.0;
            j.s1 = std::sin (j.omega);
            j.c1 = std::cos (j.omega);
            j.first = (D - half) / P;
            j.last = (D + half) / P;
            j.nextQ = j.first;
        }

        double windowAt (double u) const noexcept
        {
            const double p = u * kWindowTable;
            const int i = std::min ((int) p, kWindowTable - 1);
            const double f = p - i;
            return window[(size_t) i] + f * (window[(size_t) i + 1] - window[(size_t) i]);
        }

        // Returns true when the job has finished.
        bool runJob (int k, int units) noexcept
        {
            auto& x = xo[k];
            auto& j = x.job;
            if (! j.running)
                return j.done;

            while (units > 0 && j.nextT <= j.half)
            {
                const int end = std::min (j.half, j.nextT + N2 - 1);
                for (int t = j.nextT; t <= end; ++t)
                {
                    double h;
                    if (t == 0)
                        h = j.omega / kPi;
                    else
                    {
                        // advance (s, c) = (sin, cos)(omega t)
                        const double s = j.s * j.c1 + j.c * j.s1;
                        const double c = j.c * j.c1 - j.s * j.s1;
                        j.s = s;
                        j.c = c;
                        h = windowAt ((double) t / (double) j.half) * s / (kPi * t);
                    }
                    j.taps[(size_t) t] = h;
                    j.sumTaps += t == 0 ? h : 2.0 * h;
                }
                j.nextT = end + 1;
                --units;
            }

            const int target = 1 - x.active;
            auto& slot = x.slot[target];
            const double scale = j.sumTaps != 0.0 ? 1.0 / j.sumTaps : 1.0;

            while (units > 0 && j.nextT > j.half && j.nextQ <= j.last)
            {
                const int q = j.nextQ;
                for (int i = 0; i < P; ++i)
                {
                    const int t = q * P + i - D;
                    const int at = t < 0 ? -t : t;
                    work[(size_t) i] = cf { at <= j.half ? (float) (j.taps[(size_t) at] * scale) : 0.0f, 0.0f };
                }
                std::fill (work.begin() + P, work.end(), cf {});
                fft->forward (work.data());
                std::copy (work.begin(), work.end(), slot.spectra.begin() + (size_t) (q - 1) * (size_t) N2);
                ++j.nextQ;
                --units;
            }

            if (j.nextT > j.half && j.nextQ > j.last)
            {
                slot.hz = j.hz;
                slot.first = j.first;
                slot.last = j.last;
                j.running = false;
                j.done = true;
            }
            return j.done;
        }

        void designNow (int k) noexcept
        {
            startJob (k, xo[k].target);
            runJob (k, 1 << 30);
            xo[k].active = 1 - xo[k].active;
            xo[k].job.done = false;
        }

        // Low-pass output of crossover k for the block that ends with FDL entry `newest`, from slot s, into acc.
        void convolve (int k, int s, int newest, cf* dst) noexcept
        {
            const auto& slot = xo[k].slot[s];
            std::fill (dst, dst + N2, cf {});
            for (int q = slot.first; q <= slot.last; ++q)
            {
                const int idx = (newest - (q - 1) + K) % K;
                cmac (dst, fdl.data() + (size_t) idx * (size_t) N2, slot.spectra.data() + (size_t) (q - 1) * (size_t) N2, N2);
            }
            fft->inverse (dst);
        }

        void computeBlock (int k, int newest, bool allowSwap) noexcept
        {
            auto& x = xo[k];
            bool fading = false;
            int old = x.active;

            if (allowSwap && x.job.done)
            {
                old = x.active;
                x.active = 1 - x.active;
                x.job.done = false;
                fading = true;
            }

            convolve (k, x.active, newest, acc.data());
            if (fading)
            {
                convolve (k, old, newest, accOld.data());
                const double inv = kPi / (double) P;
                for (int i = 0; i < P; ++i)
                {
                    const float r = (float) (0.5 - 0.5 * std::cos (((double) i + 0.5) * inv));   // raised cosine
                    const cf a = accOld[(size_t) (P + i)], b = acc[(size_t) (P + i)];
                    x.out[(size_t) i] = a + (b - a) * r;
                }
            }
            else
            {
                std::copy (acc.begin() + P, acc.end(), x.out.begin());
            }
        }

        void blockBoundary() noexcept
        {
            for (int i = 0; i < P; ++i)
            {
                work[(size_t) i] = inPrev[(size_t) i];
                work[(size_t) (P + i)] = inCur[(size_t) i];
            }
            fft->forward (work.data());
            std::copy (work.begin(), work.end(), fdl.begin() + (size_t) fdlPos * (size_t) N2);
            std::swap (inPrev, inCur);

            for (int k = 0; k < activeCount; ++k)
                computeBlock (k, fdlPos, true);

            fdlPos = (fdlPos + 1) % K;

            for (int k = 0; k < kMaxCrossovers; ++k)
            {
                auto& x = xo[k];
                if (! x.job.running && ! x.job.done && x.slot[x.active].hz != clampHz (x.target))
                    startJob (k, x.target);
                if (x.job.running)
                    runJob (k, kUnitsPerBlock);
                if (k >= activeCount && x.job.done)
                {
                    x.active = 1 - x.active;   // not audible: switch without a fade
                    x.job.done = false;
                }
            }
        }

        void setActiveCount (int count) noexcept
        {
            count = std::clamp (count, 0, kMaxCrossovers);
            for (int k = activeCount; k < count; ++k)
            {
                // late start: compute this block's output from the FDL as it was at the last boundary
                if (xo[k].job.done)
                {
                    xo[k].active = 1 - xo[k].active;
                    xo[k].job.done = false;
                }
                computeBlock (k, (fdlPos - 1 + K) % K, false);
            }
            activeCount = count;
        }

        void snapAll() noexcept
        {
            for (int k = 0; k < kMaxCrossovers; ++k)
            {
                auto& x = xo[k];
                x.job.running = x.job.done = false;
                if (x.slot[x.active].hz != clampHz (x.target))
                    designNow (k);
            }
        }

        void process (const float* const* input, float* const* const* out, int numSamples, int numBands) noexcept
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const cf xin { input[0][i], input[1][i] };
                inCur[(size_t) pos] = xin;

                dry[(size_t) dryPos] = xin;
                const cf xd = dry[(size_t) ((dryPos - D) & dryMask)];
                dryPos = (dryPos + 1) & dryMask;

                if (numBands <= 1)
                {
                    out[0][0][i] = xd.real();
                    out[0][1][i] = xd.imag();
                }
                else
                {
                    cf prev {};
                    for (int b = 0; b < numBands - 1; ++b)
                    {
                        const cf y = xo[b].out[(size_t) pos];
                        const cf band = y - prev;
                        out[b][0][i] = band.real();
                        out[b][1][i] = band.imag();
                        prev = y;
                    }
                    const cf last = xd - prev;
                    out[numBands - 1][0][i] = last.real();
                    out[numBands - 1][1][i] = last.imag();
                }

                if (++pos == P)
                {
                    pos = 0;
                    blockBoundary();
                }
            }
        }
    };
}

//==============================================================================
struct Crossover::Impl
{
    IirEngine iir;
    FirEngine fir;
    int numBands = 1;
    CrossoverType type = CrossoverType::Analog;
    float lastHz[kMaxCrossovers] { 120.0f, 1000.0f, 6000.0f };
    bool fresh = true;
    bool prepared = false;
    double sr = 48000.0;

    void applyType (CrossoverType t) noexcept
    {
        type = t;
        if (t == CrossoverType::Digital)
        {
            fir.reset();
        }
        else
        {
            iir.setOrder (t == CrossoverType::Hybrid ? kLrOrderHybrid : kLrOrderAnalog);
            iir.reset();
        }
        fresh = true;
    }
};

Crossover::Crossover() : impl (std::make_unique<Impl>()) {}
Crossover::~Crossover() = default;

void Crossover::prepare (double sampleRate, int maxBlockSize)
{
    (void) maxBlockSize;   // the engines process sample by sample / per partition, any block size works
    impl->sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    impl->iir.prepare (impl->sr);
    impl->fir.prepare (impl->sr);
    impl->prepared = true;
    impl->applyType (impl->type);
    impl->iir.passTarget = impl->numBands <= 1 ? 1.0 : 0.0;
    impl->fir.setActiveCount (impl->type == CrossoverType::Digital ? impl->numBands - 1 : 0);
    reset();
}

void Crossover::reset() noexcept
{
    impl->iir.reset();
    impl->fir.reset();
    impl->fresh = true;
}

void Crossover::setup (int numBands, const float* crossoverHz, CrossoverType type) noexcept
{
    auto& m = *impl;
    numBands = std::clamp (numBands, 1, kMaxBands);

    if (type != m.type)
        m.applyType (type);

    for (int k = 0; k < numBands - 1 && crossoverHz != nullptr; ++k)
    {
        float hz = std::isfinite (crossoverHz[k]) ? crossoverHz[k] : m.lastHz[k];
        hz = std::clamp (hz, 10.0f, (float) (0.45 * m.sr));
        if (k > 0)
            hz = std::max (hz, m.lastHz[k - 1]);
        m.lastHz[k] = hz;
    }

    for (int k = 0; k < kMaxCrossovers; ++k)
    {
        m.iir.tgtLog[k] = std::log ((double) m.lastHz[k]);
        m.fir.xo[k].target = (double) m.lastHz[k];
    }

    m.iir.passTarget = numBands <= 1 ? 1.0 : 0.0;

    if (m.fresh && m.prepared)
    {
        m.iir.snap();
        m.fir.snapAll();
    }

    m.numBands = numBands;
    if (m.prepared)
        m.fir.setActiveCount (m.type == CrossoverType::Digital ? numBands - 1 : 0);
}

void Crossover::process (const float* const* input, float* const* const* bandsOut, int numSamples) noexcept
{
    auto& m = *impl;
    if (! m.prepared || numSamples <= 0)
        return;

    m.fresh = false;
    if (m.type == CrossoverType::Digital)
        m.fir.process (input, bandsOut, numSamples, m.numBands);
    else
        m.iir.process (input, bandsOut, numSamples, m.numBands);
}

int Crossover::getLatencySamples (CrossoverType type) const noexcept
{
    return type == CrossoverType::Digital ? impl->fir.D : 0;
}

int Crossover::getNumBands() const noexcept { return impl->numBands; }
} // namespace ps
