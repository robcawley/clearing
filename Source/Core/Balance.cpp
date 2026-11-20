#include "Balance.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

namespace clr {

namespace {
double lin(float db) { return db <= kFloorDb + 1 ? 0.0 : std::pow(10.0, db / 10.0); }
float dbOf(double p) { return p <= 1e-14 ? kFloorDb : (float) (10.0 * std::log10(p)); }
std::string fmt1(float v) { char b[16]; std::snprintf(b, sizeof b, "%.1f", v); return b; }
std::string pct(float v) { char b[16]; std::snprintf(b, sizeof b, "%.0f%%", v * 100.0f); return b; }

int lastBarOf(const std::vector<TrackTimeline>& tracks)
{
    int last = 0;
    for (auto& t : tracks)
        for (int b = 0; b < kBars; ++b)
            if (barTotalDb(t.db, b) > kFloorDb + 1) last = std::max(last, b + 1);
    return last;
}

// A unit is one instrument: a track, or all tracks sharing a layer.
struct Unit { std::vector<int> members; std::string name; int role = RoleOther; float priority = 0; bool locked = true; };
std::vector<Unit> unitsOf(const std::vector<TrackTimeline>& tracks)
{
    std::vector<Unit> u;
    std::map<int, int> layerUnit;
    for (int i = 0; i < (int) tracks.size(); ++i) {
        const auto& t = tracks[(size_t) i];
        int ui;
        if (t.layer > 0 && layerUnit.count(t.layer)) ui = layerUnit[t.layer];
        else { ui = (int) u.size(); u.push_back({}); if (t.layer > 0) layerUnit[t.layer] = ui; }
        auto& x = u[(size_t) ui];
        x.members.push_back(i);
        if (x.members.size() == 1 || t.priority > x.priority) { x.role = t.role; }
        x.priority = std::max(x.priority, t.priority);
        x.locked = x.locked && t.locked;
    }
    for (auto& x : u) {
        x.name = tracks[(size_t) x.members[0]].name;
        if (x.members.size() > 1) x.name += " layer";
    }
    return u;
}
// Mean linear power per coarse band over a bar span, post offset.
std::array<double, kCoarse> spanEnergy(const std::vector<TrackTimeline>& tracks, const Unit& u, int b0, int b1)
{
    std::array<double, kCoarse> e {};
    int n = 0;
    for (int b = b0; b <= b1 && b <= kBars; ++b, ++n)
        for (int m : u.members) {
            const auto& t = tracks[(size_t) m];
            for (int k = 0; k < kCoarse; ++k) e[(size_t) k] += lin(t.db[(size_t) (b - 1)][(size_t) k] + t.offsetDb);
        }
    if (n) for (auto& v : e) v /= n;
    return e;
}
} // namespace

float barTotalDb(const Timeline& t, int barIndex)
{
    double s = 0;
    for (int k = 0; k < kCoarse; ++k) s += lin(t[(size_t) barIndex][(size_t) k]);
    return dbOf(s);
}

LevelFit estimateLevels(const std::vector<TrackTimeline>& tracks, const Timeline& master)
{
    LevelFit fit;
    const size_t n = tracks.size();
    fit.offsetDb.assign(n, kFloorDb);
    if (!n) return fit;
    std::vector<std::array<double, kCoarse>> rows;
    std::vector<std::vector<std::array<double, kCoarse>>> xs(n);
    double scale = 0;
    for (int b = 0; b < kBars; ++b) {
        if (barTotalDb(master, b) <= kFloorDb + 1) continue;
        std::array<double, kCoarse> y;
        for (int k = 0; k < kCoarse; ++k) { y[(size_t) k] = lin(master[(size_t) b][(size_t) k]); scale = std::max(scale, y[(size_t) k]); }
        rows.push_back(y);
        for (size_t t = 0; t < n; ++t) {
            std::array<double, kCoarse> x;
            for (int k = 0; k < kCoarse; ++k) x[(size_t) k] = lin(tracks[t].db[(size_t) b][(size_t) k]);
            xs[t].push_back(x);
        }
    }
    fit.bars = (int) rows.size();
    if (fit.bars < 4 || scale <= 0) return fit;
    std::vector<double> g(n, 0.0);
    // use[r][k]: cells taking part (pass 2 leaves out cells the tracks can't explain).
    std::vector<std::array<bool, kCoarse>> use(rows.size());
    std::vector<std::array<double, kCoarse>> soft(rows.size()); // down-weights cells the tracks under-explain
    for (auto& s : soft) s.fill(1.0);
    for (auto& u : use) u.fill(true);
    auto solve = [&] {
    std::vector<std::vector<double>> G(n, std::vector<double>(n, 0.0));
    std::vector<double> h(n, 0.0);
    // Relative errors: every bar and band counts alike, so the loud low end doesn't decide everything.
    // Cells more than 60 dB below the loudest are left out (noise).
    for (size_t r = 0; r < rows.size(); ++r)
        for (int k = 0; k < kCoarse; ++k) {
            const double y = rows[r][(size_t) k] / scale;
            if (y < 1e-6 || !use[r][(size_t) k]) continue;
            const double w2 = soft[r][(size_t) k] / (y * y);
            for (size_t i = 0; i < n; ++i) {
                const double xi = xs[i][r][(size_t) k] / scale;
                if (xi == 0) continue;
                h[i] += w2 * xi * y;
                for (size_t j = 0; j < n; ++j) G[i][j] += w2 * xi * xs[j][r][(size_t) k] / scale;
            }
        }
    // Non-negative least squares: solve the free variables exactly (Gaussian elimination), drop the
    // most negative and repeat. Exact solving matters: similar-sounding parts make the system
    // ill-conditioned, where iterative descent stops short of the answer.
    std::vector<bool> freeVar(n);
    for (size_t i = 0; i < n; ++i) freeVar[i] = G[i][i] > 0;
    for (size_t round = 0; round <= n; ++round) {
        std::vector<size_t> F;
        for (size_t i = 0; i < n; ++i) if (freeVar[i]) F.push_back(i);
        std::fill(g.begin(), g.end(), 0.0);
        if (F.empty()) break;
        const size_t m = F.size();
        std::vector<std::vector<double>> A(m, std::vector<double>(m + 1));
        double tr = 0; for (size_t a = 0; a < m; ++a) tr += G[F[a]][F[a]];
        const double ridge = 1e-12 * tr / (double) m; // keeps exactly collinear parts solvable
        for (size_t a = 0; a < m; ++a) {
            for (size_t b = 0; b < m; ++b) A[a][b] = G[F[a]][F[b]] + (a == b ? ridge : 0.0);
            A[a][m] = h[F[a]];
        }
        for (size_t col = 0; col < m; ++col) {
            size_t piv = col;
            for (size_t r = col + 1; r < m; ++r) if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
            std::swap(A[col], A[piv]);
            if (std::fabs(A[col][col]) < 1e-300) continue;
            for (size_t r = 0; r < m; ++r) {
                if (r == col) continue;
                const double f = A[r][col] / A[col][col];
                if (f != 0) for (size_t k = col; k <= m; ++k) A[r][k] -= f * A[col][k];
            }
        }
        size_t worst = n; double wv = 0;
        for (size_t a = 0; a < m; ++a) {
            const double v = std::fabs(A[a][a]) < 1e-300 ? 0.0 : A[a][m] / A[a][a];
            g[F[a]] = v;
            if (v < wv) { wv = v; worst = F[a]; }
        }
        if (worst == n) break;
        freeVar[worst] = false;
    }
    };
    // Low end where two or more parts overlap doesn't add up as power (their phases interfere): those
    // cells may come out lower than the parts predict. They're marked, and down-weighted below if so.
    std::vector<int> cellsOf(n, 0);
    for (size_t r = 0; r < rows.size(); ++r) for (int k = 0; k < kCoarse; ++k) for (size_t i = 0; i < n; ++i) if (xs[i][r][(size_t) k] > 0) ++cellsOf[i];
    std::vector<std::array<bool, kCoarse>> overlapLow(rows.size());
    for (auto& a : overlapLow) a.fill(false);
    for (size_t r = 0; r < rows.size(); ++r)
        for (int k = 0; k <= 1; ++k) {
            double s = 0; for (size_t i = 0; i < n; ++i) s += xs[i][r][(size_t) k];
            if (s <= 0) continue;
            int big = 0; for (size_t i = 0; i < n; ++i) if (xs[i][r][(size_t) k] / s >= 0.15) ++big;
            overlapLow[r][(size_t) k] = big >= 2;
        }
    // Each part must keep a fifth of its cells and some of its energy (8%) outside the overlap to be
    // judged without it.
    bool canExclude = true;
    {
        std::vector<int> left = cellsOf;
        std::vector<double> eAll(n, 0.0), eLeft(n, 0.0);
        for (size_t r = 0; r < rows.size(); ++r) for (int k = 0; k < kCoarse; ++k) for (size_t i = 0; i < n; ++i) {
            const double x = xs[i][r][(size_t) k];
            eAll[i] += x;
            if (overlapLow[r][(size_t) k]) { if (x > 0) --left[i]; } else eLeft[i] += x;
        }
        for (size_t i = 0; i < n; ++i) if (cellsOf[i] > 0 && (left[i] < std::max(2, cellsOf[i] / 5) || eLeft[i] < 0.08 * eAll[i])) canExclude = false;
    }
    int dropped = 0;
    auto run = [&](bool excludeOverlap) {
        for (auto& u : use) u.fill(true);
        for (auto& sw : soft) sw.fill(1.0);
        if (excludeOverlap) for (size_t r = 0; r < rows.size(); ++r) for (int k = 0; k <= 1; ++k) if (overlapLow[r][(size_t) k]) use[r][(size_t) k] = false;
        solve();
        // Energy a measured part can't remove, only add: where the stereo out has clearly more than the
        // tracks predict (+1.5 dB), something unmeasured is playing; where overlapping low parts come out
        // clearly less (-1.5 dB), they cancel. Down-weight those cells and refit, so neither drags the levels.
        for (int it = 0; it < 6; ++it) {
            int moved = 0;
            for (size_t r = 0; r < rows.size(); ++r)
                for (int k = 0; k < kCoarse; ++k) {
                    double p = 0;
                    for (size_t i = 0; i < n; ++i) p += g[i] * xs[i][r][(size_t) k];
                    const double y = rows[r][(size_t) k];
                    const double w = (y > 1.41 * p || (overlapLow[r][(size_t) k] && y < 0.71 * p)) ? 0.1 : 1.0;
                    if (w != soft[r][(size_t) k]) { soft[r][(size_t) k] = w; ++moved; }
                }
            if (!moved) break;
            solve();
        }
        // Robust passes: cells with far more at the stereo out than the tracks explain (a part without
        // CLEARING, a return, a bus effect) would drag the fit. Leave out cells 3 dB or more over and
        // solve again, a few times; every part keeps at least a fifth of its cells.
        dropped = 0;
        for (int pass = 0; pass < 4; ++pass) {
            int changed = 0;
            for (size_t r = 0; r < rows.size(); ++r)
                for (int k = 0; k < kCoarse; ++k) {
                    if (!use[r][(size_t) k]) continue;
                    double p = 0;
                    for (size_t i = 0; i < n; ++i) p += g[i] * xs[i][r][(size_t) k];
                    if (rows[r][(size_t) k] > 2.0 * p) {
                        bool keep = false;
                        for (size_t i = 0; i < n; ++i) {
                            if (xs[i][r][(size_t) k] <= 0) continue;
                            int u = 0;
                            for (size_t r2 = 0; r2 < rows.size(); ++r2) for (int k2 = 0; k2 < kCoarse; ++k2) if (use[r2][(size_t) k2] && xs[i][r2][(size_t) k2] > 0) ++u;
                            if (u - 1 < std::max(2, cellsOf[i] / 5)) keep = true;
                        }
                        if (!keep) { use[r][(size_t) k] = false; ++changed; ++dropped; }
                    }
                }
            if (!changed) break;
            solve();
        }

    };
    // How well a fit explains the cells outside the low-end overlap (mean |dB| error).
    auto score = [&] {
        double s = 0; int c = 0;
        for (size_t r = 0; r < rows.size(); ++r)
            for (int k = 0; k < kCoarse; ++k) {
                if (overlapLow[r][(size_t) k]) continue;
                const double y = rows[r][(size_t) k]; if (y < 1e-4 * scale) continue;
                double p = 0; for (size_t i = 0; i < n; ++i) p += g[i] * xs[i][r][(size_t) k];
                s += std::min(30.0, std::fabs(10.0 * std::log10((y + 1e-30) / (p + 1e-30)))); ++c;
            }
        return c ? s / c : 0.0;
    };
    // Two readings: everything counts, or the overlapping low end is left out (in case low parts
    // cancel there). Take the second only when it explains the rest of the mix clearly better.
    run(false);
    const double sA = score();
    if (canExclude) {
        const auto gA = g; const auto useA = use; const auto softA = soft; const int dA = dropped;
        run(true);
        const double sB = score();
        if (!(sB < 0.5 * sA - 0.05)) { g = gA; use = useA; soft = softA; dropped = dA; }
    }
    fit.excluded = dropped;
    double ssr = 0, sst = 0, mean = 0; int cnt = 0;
    for (size_t r = 0; r < rows.size(); ++r) for (int k = 0; k < kCoarse; ++k) if (use[r][(size_t) k]) { mean += rows[r][(size_t) k] / scale; ++cnt; }
    mean /= std::max(1, cnt);
    for (size_t r = 0; r < rows.size(); ++r)
        for (int k = 0; k < kCoarse; ++k) {
            if (!use[r][(size_t) k]) continue;
            double p = 0;
            for (size_t i = 0; i < n; ++i) p += g[i] * xs[i][r][(size_t) k] / scale;
            const double y = rows[r][(size_t) k] / scale;
            ssr += (y - p) * (y - p); sst += (y - mean) * (y - mean);
        }
    fit.r2 = sst > 0 ? (float) std::max(0.0, 1.0 - ssr / sst) : 0.0f;
    for (size_t i = 0; i < n; ++i) fit.offsetDb[i] = g[i] > 1e-9 ? (float) (10.0 * std::log10(g[i])) : kFloorDb;
    return fit;
}

std::vector<SectionSpan> detectSections(const std::vector<TrackTimeline>& tracks, int bars)
{
    std::vector<SectionSpan> out;
    if (bars <= 0) bars = lastBarOf(tracks);
    if (bars <= 0) return out;
    const size_t n = tracks.size();
    std::vector<float> peak(n, kFloorDb);
    for (size_t t = 0; t < n; ++t) for (int b = 0; b < bars; ++b) peak[t] = std::max(peak[t], barTotalDb(tracks[t].db, b));
    auto activeSet = [&](int b) {
        std::vector<int> a;
        for (size_t t = 0; t < n; ++t) if (peak[t] > kFloorDb + 1 && barTotalDb(tracks[t].db, b) > peak[t] - 25.0f) a.push_back((int) t);
        return a;
    };
    auto total = [&](int b) { double s = 0; for (auto& t : tracks) s += lin(barTotalDb(t.db, b)); return dbOf(s); };
    // Energy share of each part over a 2-bar window (bars b, b+1).
    auto shares = [&](int b) {
        std::vector<double> e(n, 0.0); double tot = 0;
        for (int x = b; x < std::min(bars, b + 2); ++x)
            for (size_t t = 0; t < n; ++t) { const double v = lin(barTotalDb(tracks[t].db, x)); e[t] += v; tot += v; }
        if (tot > 0) for (auto& v : e) v /= tot;
        return e;
    };
    auto winTotal = [&](int b) { double s = 0; int c = 0; for (int x = b; x < std::min(bars, b + 2); ++x, ++c) s += lin(total(x)); return dbOf(c ? s / c : 0); };
    // Change score at each bar (comparing the two bars before with the two bars from it).
    std::vector<double> score((size_t) bars + 1, 0.0);
    for (int b = 2; b < bars; ++b) {
        const auto A = activeSet(b - 1), B = activeSet(b);
        std::vector<int> I, U;
        std::set_intersection(A.begin(), A.end(), B.begin(), B.end(), std::back_inserter(I));
        std::set_union(A.begin(), A.end(), B.begin(), B.end(), std::back_inserter(U));
        const double jac = U.empty() ? 0.0 : 1.0 - (double) I.size() / (double) U.size();
        const auto sa = shares(b - 2), sb = shares(b);
        double dist = 0; for (size_t t = 0; t < n; ++t) dist += std::fabs(sb[t] - sa[t]);
        dist *= 0.5;
        const double jump = std::fabs(winTotal(b) - winTotal(b - 2)) / 6.0 * 0.3;
        score[(size_t) b] = std::max({ jac * 0.3 / 0.34, dist, jump });
    }
    std::vector<int> starts { 0 };
    for (int b = 2; b < bars; ++b) {
        if (b - starts.back() < 4 || score[(size_t) b] <= 0.3) continue;
        if (b + 1 < bars && score[(size_t) (b + 1)] > score[(size_t) b]) continue; // the change peaks a bar later
        starts.push_back(b);
    }
    // Merge spans shorter than 4 bars into the previous span.
    std::vector<int> merged;
    for (size_t i = 0; i < starts.size(); ++i) {
        const int end = i + 1 < starts.size() ? starts[i + 1] : bars;
        if (!merged.empty() && end - starts[i] < 4) continue;
        merged.push_back(starts[i]);
    }
    for (size_t i = 0; i < merged.size(); ++i) {
        SectionSpan s;
        s.start = merged[i] + 1;
        s.end = i + 1 < merged.size() ? merged[i + 1] : bars;
        s.name = "Part " + std::to_string(i + 1);
        // parts playing in most of the span
        for (size_t t = 0; t < n; ++t) {
            int on = 0;
            for (int b = s.start - 1; b < s.end; ++b) { auto a = activeSet(b); if (std::find(a.begin(), a.end(), (int) t) != a.end()) ++on; }
            if (on * 2 >= s.end - s.start + 1) s.active.push_back(tracks[t].name);
        }
        out.push_back(s);
    }
    return out;
}

namespace {
struct Lead { int unit = -1; int band = 0; float share = 0; std::array<double, kCoarse> e {}; bool active = false; };
Lead leaderOf(const std::vector<TrackTimeline>& tracks, const std::vector<Unit>& units, const SectionSpan& s)
{
    Lead L;
    if (s.leadId >= 0) {
        for (int i = 0; i < (int) units.size(); ++i)
            for (int m : units[(size_t) i].members) if (tracks[(size_t) m].id == s.leadId) L.unit = i;
    }
    if (L.unit < 0) { // the overall focus: highest priority, then loudest
        float best = -1;
        for (int i = 0; i < (int) units.size(); ++i) if (units[(size_t) i].priority > best + 1e-4f) { best = units[(size_t) i].priority; L.unit = i; }
    }
    if (L.unit < 0) return L;
    const auto& u = units[(size_t) L.unit];
    L.e = spanEnergy(tracks, u, s.start, s.end);
    // playing here? compare with its loudest bar anywhere
    float peak = kFloorDb, here = dbOf(std::accumulate(L.e.begin(), L.e.end(), 0.0));
    for (int b = 0; b < kBars; ++b) { double sum = 0; for (int m : u.members) sum += lin(barTotalDb(tracks[(size_t) m].db, b) + tracks[(size_t) m].offsetDb); peak = std::max(peak, dbOf(sum)); }
    L.active = here > peak - 25.0f && here > kFloorDb + 1;
    const bool low = u.role == RoleBass || u.role == RoleKick || u.role == RoleDoubleBasses || u.role == RoleLowBrass;
    int k0 = low ? 0 : 1;
    L.band = k0;
    for (int k = k0; k < kCoarse; ++k) if (L.e[(size_t) k] > L.e[(size_t) L.band]) L.band = k;
    double tot = 0;
    for (size_t i = 0; i < units.size(); ++i) tot += spanEnergy(tracks, units[i], s.start, s.end)[(size_t) L.band];
    L.share = tot > 0 ? (float) (L.e[(size_t) L.band] / tot) : 0.0f;
    return L;
}
} // namespace

std::vector<SectionShare> describeSections(const std::vector<TrackTimeline>& tracks, const std::vector<SectionSpan>& sections)
{
    std::vector<SectionShare> out;
    const auto units = unitsOf(tracks);
    for (auto& s : sections) {
        SectionShare d;
        const auto L = leaderOf(tracks, units, s);
        if (L.unit < 0) { out.push_back(d); continue; }
        d.leader = units[(size_t) L.unit].name; d.band = coarseName(L.band); d.share = L.share; d.active = L.active;
        double tot = 0;
        std::vector<std::pair<std::string, double>> o;
        for (size_t i = 0; i < units.size(); ++i) { const double e = spanEnergy(tracks, units[i], s.start, s.end)[(size_t) L.band]; tot += e; if ((int) i != L.unit) o.push_back({ units[i].name, e }); }
        std::sort(o.begin(), o.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < o.size() && i < 3; ++i) if (tot > 0 && o[i].second / tot > 0.05) d.others.push_back({ o[i].first, (float) (o[i].second / tot) });
        out.push_back(d);
    }
    return out;
}

std::vector<BalanceMove> planBalance(const std::vector<TrackTimeline>& tracks, const std::vector<SectionSpan>& sectionsIn,
                                     int style, std::vector<std::string>* notes)
{
    std::vector<BalanceMove> moves;
    const auto units = unitsOf(tracks);
    std::vector<SectionSpan> sections = sectionsIn;
    const bool whole = sections.empty();
    if (whole) { SectionSpan s; s.name = "the whole song"; s.start = 1; s.end = std::max(1, lastBarOf(tracks)); sections.push_back(s); }
    const float orchAdj = style == StyleOrchestral ? 0.05f : 0.0f;

    for (int si = 0; si < (int) sections.size(); ++si) {
        const auto& s = sections[(size_t) si];
        const auto L = leaderOf(tracks, units, s);
        if (L.unit < 0 || !L.active) continue;
        const auto& lu = units[(size_t) L.unit];
        const bool low = L.band <= 1;
        float target = low ? 0.40f : (lu.role == RoleSolo || lu.role == RoleLeadVocal) ? 0.35f : 0.30f;
        target -= orchAdj;
        if (L.share <= 0) continue;
        const float need = 10.0f * std::log10(target / L.share);
        if (need < 0.75f) continue;
        const std::string where = whole ? "" : " in " + s.name;
        const float raise = std::min(need, 2.0f);
        if (!lu.locked)
            for (int m : lu.members) {
                if (tracks[(size_t) m].locked) continue;
                BalanceMove mv; mv.track = tracks[(size_t) m].id; mv.section = whole ? -1 : si; mv.gainDb = raise;
                mv.confidence = std::min(0.9f, 0.4f + need / 8.0f);
                mv.reason = lu.name + " leads" + where + " but holds only " + pct(L.share) + " of the " + std::string(coarseName(L.band))
                            + " band, where its sound lives. +" + fmt1(raise) + " dB there brings it toward " + pct(target) + ".";
                moves.push_back(mv);
            }
        if (need > 2.0f) {
            int mi = -1; double me = 0;
            for (int i = 0; i < (int) units.size(); ++i) {
                if (i == L.unit || units[(size_t) i].locked) continue;
                const double e = spanEnergy(tracks, units[(size_t) i], s.start, s.end)[(size_t) L.band];
                if (e > me) { me = e; mi = i; }
            }
            if (mi >= 0) {
                const float cut = -std::min(need - 2.0f, 2.0f);
                for (int m : units[(size_t) mi].members) {
                    if (tracks[(size_t) m].locked) continue;
                    BalanceMove mv; mv.track = tracks[(size_t) m].id; mv.section = whole ? -1 : si; mv.gainDb = cut;
                    mv.confidence = 0.5f;
                    mv.reason = units[(size_t) mi].name + " is the biggest part in " + lu.name + "'s " + coarseName(L.band) + " band" + where
                                + ". " + fmt1(cut) + " dB lets " + lu.name + " through without pushing it further.";
                    moves.push_back(mv);
                }
            }
        }
        if (need > 4.0f && notes)
            notes->push_back(lu.name + where + " needs about " + fmt1(need) + " dB more presence than small level moves can give. Consider the arrangement (fewer parts in its register) or a bigger fader move.");
    }
    // A track moved the same way in every section with moves becomes one whole-song move.
    if (!whole) {
        std::map<int, std::vector<BalanceMove*>> byTrack;
        for (auto& m : moves) byTrack[m.track].push_back(&m);
        int playing = 0;
        for (auto& s : sections) { auto L = leaderOf(tracks, units, s); if (L.unit >= 0 && L.active) ++playing; }
        std::vector<BalanceMove> merged;
        for (auto& [id, list] : byTrack) {
            float lo = 1e9f, hi = -1e9f, sum = 0;
            for (auto* m : list) { lo = std::min(lo, m->gainDb); hi = std::max(hi, m->gainDb); sum += m->gainDb; }
            if ((int) list.size() == playing && playing > 1 && hi - lo <= 0.5f) {
                BalanceMove m = *list[0]; m.section = -1; m.gainDb = sum / (float) list.size();
                m.reason = "In every section: " + m.reason;
                merged.push_back(m);
            } else for (auto* m : list) merged.push_back(*m);
        }
        moves = merged;
    }
    return moves;
}

std::vector<MixFinding> findResiduals(const std::vector<TrackTimeline>& tracks, const Timeline& master)
{
    std::vector<MixFinding> out;
    int bars = 0;
    for (int b = 0; b < kBars; ++b) if (barTotalDb(master, b) > kFloorDb + 1) bars = b + 1;
    if (bars < 4 || tracks.empty()) return out;
    // prediction per bar/band, and each part's share of it
    auto pred = [&](int b, int k, std::vector<double>* parts) {
        double s = 0;
        if (parts) parts->assign(tracks.size(), 0.0);
        for (size_t t = 0; t < tracks.size(); ++t) {
            const double v = lin(tracks[t].db[(size_t) b][(size_t) k]) * std::pow(10.0, tracks[t].offsetDb / 10.0);
            s += v; if (parts) (*parts)[t] = v;
        }
        return s;
    };
    char buf[160];
    for (int k = 0; k < kCoarse; ++k) {
        float peak = kFloorDb;
        for (int b = 0; b < bars; ++b) peak = std::max(peak, master[(size_t) b][(size_t) k]);
        if (peak <= kFloorDb + 1) continue;
        // 1. Energy the tracks don't account for (> 4 dB more at the stereo out), in runs of 2+ bars.
        {
            int runStart = -1; double acc = 0; int cnt = 0, gap = 0;
            auto flush = [&](int endBar) {
                if (runStart >= 0 && cnt >= 2) {
                    MixFinding f; f.kind = "unexplained"; f.band = k; f.start = runStart + 1; f.end = endBar + 1; f.db = (float) (acc / cnt);
                    std::snprintf(buf, sizeof buf, "%s (%d-%d Hz)", coarseName(k), (int) coarseEdge(k), (int) coarseEdge(k + 1));
                    f.text = "Something without CLEARING on it is loud in the " + std::string(buf) + " in bars " + std::to_string(f.start) + "-"
                             + std::to_string(f.end) + ": the stereo out has about " + fmt1(f.db) + " dB more there than the tracks CLEARING sees. A part, a return or a bus effect.";
                    out.push_back(f);
                }
                runStart = -1; acc = 0; cnt = 0; gap = 0;
            };
            int lastHit = -10;
            for (int b = 0; b < bars; ++b) {
                const float m = master[(size_t) b][(size_t) k];
                const double p = pred(b, k, nullptr);
                const float r = m - dbOf(p);
                const bool hit = m > peak - 30.0f && r > 2.5f;
                if (hit) {
                    if (runStart < 0) runStart = b;
                    acc += std::min(r, 30.0f); ++cnt; lastHit = b; gap = 0;
                } else if (runStart >= 0 && ++gap > 1) flush(lastHit);
            }
            flush(lastHit);
        }
        // 2. Low end lost where two or more low parts overlap (likely phase between them).
        if (k <= 1) {
            double accO = 0; int nO = 0, first = -1, last = -1;
            std::map<size_t, int> who;
            for (int b = 0; b < bars; ++b) {
                std::vector<double> parts;
                const double p = pred(b, k, &parts);
                if (p <= 0 || master[(size_t) b][(size_t) k] < peak - 30.0f) continue;
                std::vector<size_t> big;
                for (size_t t = 0; t < parts.size(); ++t) if (parts[t] / p >= 0.15) big.push_back(t);
                if (big.size() < 2) continue;
                accO += master[(size_t) b][(size_t) k] - dbOf(p); ++nO;
                if (first < 0) first = b;
                last = b;
                for (auto t : big) ++who[t];
            }
            if (nO >= 2 && accO / nO < -2.0) {
                MixFinding f; f.kind = "low-end cancellation"; f.band = k; f.start = first + 1; f.end = last + 1; f.db = (float) (accO / nO);
                std::vector<std::pair<int, size_t>> ranked;
                for (auto& [t, c] : who) ranked.push_back({ c, t });
                std::sort(ranked.rbegin(), ranked.rend());
                for (size_t i = 0; i < ranked.size() && i < 2; ++i) f.parts.push_back(tracks[ranked[i].second].name);
                std::snprintf(buf, sizeof buf, "%s (%d-%d Hz)", coarseName(k), (int) coarseEdge(k), (int) coarseEdge(k + 1));
                f.text = "The " + std::string(buf) + " loses about " + fmt1(-f.db) + " dB at the stereo out where "
                         + (f.parts.size() > 1 ? f.parts[0] + " and " + f.parts[1] : std::string("the low parts")) + " overlap (bars "
                         + std::to_string(f.start) + "-" + std::to_string(f.end) + "): they partly cancel. Try flipping the polarity of one, "
                         + "nudging its timing by a millisecond or two, or letting one own that range.";
                out.push_back(f);
            }
        }
    }
    return out;
}

} // namespace clr
