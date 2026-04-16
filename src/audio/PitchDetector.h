#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PitchDetector — YIN algorithm, runs offline after recording.
// Returns dominant pitch in Hz and maps it to a musical key string.
// ─────────────────────────────────────────────────────────────────────────────
#include <vector>
#include <string>
#include <map>
#include <cmath>
#include <algorithm>

class PitchDetector
{
public:
    // Detect fundamental frequency from mono float samples.
    // Returns Hz, or 0 if no clear pitch.
    static float detectHz(const float* samples, int numSamples, double sampleRate)
    {
        if (numSamples < 2048) return 0.f;
        const int W = std::min(numSamples / 2, 4096);
        const float threshold = 0.15f;

        // YIN difference function
        std::vector<float> d(W, 0.f);
        for (int tau = 1; tau < W; ++tau)
            for (int i = 0; i < W; ++i) {
                float diff = samples[i] - samples[i + tau];
                d[tau] += diff * diff;
            }

        // Cumulative mean normalised difference
        std::vector<float> cmnd(W, 0.f);
        cmnd[0] = 1.f;
        float runSum = 0.f;
        for (int tau = 1; tau < W; ++tau) {
            runSum += d[tau];
            cmnd[tau] = runSum > 0.f ? d[tau] * (float)tau / runSum : 1.f;
        }

        // First dip below threshold
        int tauEst = -1;
        for (int tau = 2; tau < W; ++tau) {
            if (cmnd[tau] < threshold) {
                while (tau + 1 < W && cmnd[tau + 1] < cmnd[tau]) ++tau;
                tauEst = tau; break;
            }
        }
        if (tauEst < 2) return 0.f;

        // Parabolic interpolation
        float better = (float)tauEst;
        if (tauEst > 0 && tauEst < W - 1) {
            float s0 = cmnd[tauEst-1], s1 = cmnd[tauEst], s2 = cmnd[tauEst+1];
            float den = s0 - 2.f*s1 + s2;
            if (std::abs(den) > 1e-6f) better += (s0 - s2) / (2.f * den);
        }

        float freq = (float)(sampleRate / (double)better);
        return (freq >= 50.f && freq <= 1500.f) ? freq : 0.f;
    }

    // Hz → note name e.g. "A4", "C#3"
    static std::string hzToNote(float hz)
    {
        if (hz <= 0.f) return "";
        int midi = (int)std::round(12.0 * std::log2((double)hz / 440.0) + 69.0);
        if (midi < 0 || midi > 127) return "";
        static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        return std::string(names[midi % 12]) + std::to_string(midi / 12 - 1);
    }

    // Analyse entire take: majority-vote on 50ms windows → most common note
    static std::string analyseFile(const std::vector<float>& mono, double sampleRate)
    {
        if ((int)mono.size() < 4096) return "";
        const int step = std::max(1, (int)(sampleRate * 0.05));
        const int win  = std::min((int)mono.size() / 2, 4096);
        std::map<int,int> votes;
        int total = 0;

        for (int pos = 0; pos + win * 2 <= (int)mono.size(); pos += step) {
            float hz = detectHz(mono.data() + pos, win * 2, sampleRate);
            if (hz > 0.f) {
                int midi = (int)std::round(12.0 * std::log2((double)hz / 440.0) + 69.0);
                if (midi >= 21 && midi <= 108) { votes[midi]++; total++; }
            }
        }
        if (total < 4) return "";

        auto best = std::max_element(votes.begin(), votes.end(),
            [](const auto& a, const auto& b){ return a.second < b.second; });
        if (best->second < total / 6) return "";  // needs 16% majority

        float hz = (float)(440.0 * std::pow(2.0, (best->first - 69) / 12.0));
        return hzToNote(hz);
    }
};
