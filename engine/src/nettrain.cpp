#include "nettrain.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>

#include "json.h"

namespace ygoe {

namespace {

constexpr double T = 2.5;  // the evaluator's temperature: score = sigmoid(raw / T)

double sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }

struct Net {
	std::vector<int> sizes;              // in, hidden..., 1
	std::vector<std::vector<double>> W;  // [layer][out * in]
	std::vector<std::vector<double>> b;
	double scale = 2.0;

	// The last layer's output for one input row (normalised), and the activations if asked.
	double forward(const double* x, std::vector<std::vector<double>>* acts) const {
		std::vector<double> a(x, x + sizes[0]);
		if(acts) {
			acts->resize(W.size() + 1);
			(*acts)[0] = a;
		}
		for(size_t l = 0; l < W.size(); ++l) {
			const int in = sizes[l], out = sizes[l + 1];
			std::vector<double> o(static_cast<size_t>(out));
			for(int r = 0; r < out; ++r) {
				double s = b[l][static_cast<size_t>(r)];
				const double* w = &W[l][static_cast<size_t>(r) * in];
				for(int c = 0; c < in; ++c)
					s += w[c] * a[static_cast<size_t>(c)];
				o[static_cast<size_t>(r)] = l + 1 < W.size() ? std::tanh(s) : s;
			}
			a.swap(o);
			if(acts)
				(*acts)[l + 1] = a;
		}
		return a[0];
	}
	double correction(const double* x) const {
		const double c = scale * forward(x, nullptr);
		return std::isfinite(c) ? c : 0.0;
	}
};

struct Normal {
	std::vector<double> mean, std;
};

bool load_net(const std::string& text, Net& net, Normal& norm) {
	if(text.empty())
		return false;
	auto v = json::Value::parse(text);
	if(!v.is_object())
		return false;
	norm.mean.clear();
	norm.std.clear();
	for(const auto& x : v["mean"].items())
		norm.mean.push_back(x.num(0));
	for(const auto& x : v["std"].items())
		norm.std.push_back(x.num(1));
	if(norm.mean.empty() || norm.mean.size() != norm.std.size())
		return false;
	net.sizes = {static_cast<int>(norm.mean.size())};
	net.W.clear();
	net.b.clear();
	for(const auto& l : v["layers"].items()) {
		const int in = static_cast<int>(l["in"].integer(0)), out = static_cast<int>(l["out"].integer(0));
		if(in != net.sizes.back() || out <= 0)
			return false;
		std::vector<double> w, bb;
		for(const auto& x : l["w"].items())
			w.push_back(x.num(0));
		for(const auto& x : l["b"].items())
			bb.push_back(x.num(0));
		if(w.size() != static_cast<size_t>(in) * out || bb.size() != static_cast<size_t>(out))
			return false;
		net.W.push_back(std::move(w));
		net.b.push_back(std::move(bb));
		net.sizes.push_back(out);
	}
	net.scale = v["scale"].num(2.0);
	for(auto& s : norm.std)
		if(s < 1e-6)
			s = 1.0;
	return !net.W.empty() && net.sizes.back() == 1;
}

std::string save_net(const Net& net, const Normal& norm, const std::string& id) {
	std::ostringstream o;
	o.imbue(std::locale::classic());
	o << std::setprecision(6);
	auto list = [&](const std::vector<double>& v) {
		o << "[";
		for(size_t i = 0; i < v.size(); ++i)
			o << (i ? "," : "") << (std::abs(v[i]) < 5e-7 ? 0.0 : v[i]);
		o << "]";
	};
	o << "{\"version\":1,\"id\":" << json::quote(id) << ",\"features\":" << norm.mean.size() << ",\"mean\":";
	list(norm.mean);
	o << ",\"std\":";
	list(norm.std);
	o << ",\"layers\":[";
	for(size_t l = 0; l < net.W.size(); ++l) {
		o << (l ? "," : "") << "{\"in\":" << net.sizes[l] << ",\"out\":" << net.sizes[l + 1] << ",\"w\":";
		list(net.W[l]);
		o << ",\"b\":";
		list(net.b[l]);
		o << "}";
	}
	o << "],\"scale\":" << net.scale << "}";
	return o.str();
}

double logloss_rows(const std::vector<size_t>& rows, const std::vector<double>& R, const std::vector<double>& C,
                    const std::vector<double>& Z, const std::vector<double>& Wt) {
	double l = 0, ws = 0;
	for(size_t r : rows) {
		const double p = std::clamp(sigmoid((R[r] + C[r]) / T), 1e-6, 1 - 1e-6);
		l += -(Z[r] * std::log(p) + (1 - Z[r]) * std::log(1 - p)) * Wt[r];
		ws += Wt[r];
	}
	return ws > 0 ? l / ws : 0;
}

}  // namespace

bool parse_net_sample(const std::string& line, NetSample& out) {
	if(line.find("\"f\"") == std::string::npos || line.find("\"kind\"") != std::string::npos)
		return false;
	auto v = json::Value::parse(line);
	if(!v.is_object() || !v["f"].is_array())
		return false;
	out = NetSample{};
	for(const auto& x : v["f"].items())
		out.f.push_back(static_cast<float>(x.num(0)));
	out.raw = std::clamp(v["raw"].num(0), -40.0, 40.0);
	out.z = v["z"].num(0.5);
	out.game = v["game"].str();
	out.me = static_cast<int>(v["me"].integer(0));
	out.i = static_cast<int>(v["i"].integer(0));
	out.w = v["src"].str() == "replay" ? 3.0 : 1.0;
	return !out.f.empty();
}

std::string NetTrainResult::report_json() const {
	std::ostringstream o;
	o.imbue(std::locale::classic());
	o << "{\"positions\":" << positions << ",\"games\":" << games << ",\"validation_games\":" << validation_games
	  << ",\"base_logloss\":" << base_logloss << ",\"init_logloss\":" << init_logloss << ",\"net_logloss\":" << net_logloss
	  << ",\"train_logloss\":" << train_logloss << "}";
	return o.str();
}

double net_logloss(const std::vector<NetSample>& samples, const std::string& net_json) {
	Net net;
	Normal norm;
	const bool has = load_net(net_json, net, norm);
	double l = 0, ws = 0;
	std::vector<double> x;
	for(const auto& s : samples) {
		double c = 0;
		if(has && s.f.size() == norm.mean.size()) {
			x.resize(s.f.size());
			for(size_t k = 0; k < x.size(); ++k)
				x[k] = (s.f[k] - norm.mean[k]) / norm.std[k];
			c = net.correction(x.data());
		}
		const double p = std::clamp(sigmoid((s.raw + c) / T), 1e-6, 1 - 1e-6);
		l += -(s.z * std::log(p) + (1 - s.z) * std::log(1 - p)) * s.w;
		ws += s.w;
	}
	return ws > 0 ? l / ws : 0;
}

NetTrainResult train_value_net(const std::vector<NetSample>& all, const std::string& init_json, const NetTrainConfig& cfg,
                               const std::string& id) {
	NetTrainResult res;
	if(all.empty()) {
		res.error = "no positions";
		return res;
	}
	// positions with the first one's number of features
	const size_t nf = all.front().f.size();
	std::vector<const NetSample*> S;
	for(const auto& s : all)
		if(s.f.size() == nf)
			S.push_back(&s);
	const size_t n = S.size();
	std::mt19937_64 rng(cfg.seed);

	// games split into training and validation
	std::set<std::string> game_set;
	for(auto* s : S)
		game_set.insert(s->game);
	std::vector<std::string> games(game_set.begin(), game_set.end());
	std::shuffle(games.begin(), games.end(), rng);
	const size_t nval = std::max<size_t>(1, static_cast<size_t>(games.size() * cfg.validation));
	std::set<std::string> val_games(games.begin(), games.begin() + static_cast<long>(std::min(nval, games.size())));
	std::vector<size_t> tr, va;
	for(size_t r = 0; r < n; ++r)
		(val_games.count(S[r]->game) ? va : tr).push_back(r);
	if(tr.empty() || va.empty()) {
		res.error = "too few games to learn from";
		return res;
	}

	// the network: continued from `init_json` when it has the same shape, else new
	std::vector<int> sizes{static_cast<int>(nf)};
	sizes.insert(sizes.end(), cfg.hidden.begin(), cfg.hidden.end());
	sizes.push_back(1);
	Net net;
	Normal norm;
	Net init;
	Normal init_norm;
	const bool has_init = load_net(init_json, init, init_norm);
	if(has_init && init.sizes == sizes) {
		net = init;
		norm = init_norm;
	} else {
		net.sizes = sizes;
		std::normal_distribution<double> nd(0, 1);
		for(size_t l = 0; l + 1 < sizes.size(); ++l) {
			const int in = sizes[l], out = sizes[l + 1];
			std::vector<double> w(static_cast<size_t>(in) * out);
			for(auto& x : w)
				x = l + 2 == sizes.size() ? 0.0 : nd(rng) / std::sqrt(static_cast<double>(in));  // the last layer starts at 0
			net.W.push_back(std::move(w));
			net.b.emplace_back(static_cast<size_t>(out), 0.0);
		}
		norm.mean.assign(nf, 0.0);
		norm.std.assign(nf, 0.0);
		for(size_t r : tr)
			for(size_t k = 0; k < nf; ++k)
				norm.mean[k] += S[r]->f[k];
		for(auto& m : norm.mean)
			m /= static_cast<double>(tr.size());
		for(size_t r : tr)
			for(size_t k = 0; k < nf; ++k) {
				const double d = S[r]->f[k] - norm.mean[k];
				norm.std[k] += d * d;
			}
		for(auto& s : norm.std) {
			s = std::sqrt(s / static_cast<double>(tr.size()));
			if(s < 1e-6)
				s = 1.0;
		}
	}

	// normalised inputs, raw values, results, weights
	std::vector<double> X(n * nf), R(n), Z(n), Wt(n);
	for(size_t r = 0; r < n; ++r) {
		for(size_t k = 0; k < nf; ++k)
			X[r * nf + k] = (S[r]->f[k] - norm.mean[k]) / norm.std[k];
		R[r] = S[r]->raw;
		Z[r] = S[r]->z;
		Wt[r] = S[r]->w;
	}
	auto corrections = [&](const Net& m) {
		std::vector<double> C(n);
		for(size_t r = 0; r < n; ++r)
			C[r] = m.correction(&X[r * nf]);
		return C;
	};
	const std::vector<double> zero(n, 0.0);
	res.base_logloss = logloss_rows(va, R, zero, Z, Wt);
	// the starting network's validation loss: the init network itself (in its own normalisation)
	res.init_logloss = has_init ? [&] {
		std::vector<NetSample> vs;
		for(size_t r : va)
			vs.push_back(*S[r]);
		return net_logloss(vs, init_json);
	}() : res.base_logloss;

	// TD(lambda) targets from the starting network's estimates
	std::vector<double> Y = Z;
	{
		const std::vector<double> C = corrections(net);
		if(cfg.lambda < 1) {
			std::map<std::pair<std::string, int>, std::vector<std::pair<int, size_t>>> groups;
			for(size_t r = 0; r < n; ++r)
				groups[{S[r]->game, S[r]->me}].push_back({S[r]->i, r});
			for(auto& [k, lst] : groups) {
				std::sort(lst.begin(), lst.end());
				for(size_t j = lst.size() - 1; j-- > 0;) {
					const size_t cur = lst[j].second, nxt = lst[j + 1].second;
					const double vnext = sigmoid((R[nxt] + C[nxt]) / T);
					Y[cur] = (1 - cfg.lambda) * vnext + cfg.lambda * Y[nxt];
				}
			}
		}
	}

	// Adam
	const size_t L = net.W.size();
	std::vector<std::vector<double>> mW(L), vW(L), mB(L), vB(L), gW(L), gB(L);
	for(size_t l = 0; l < L; ++l) {
		mW[l].assign(net.W[l].size(), 0);
		vW[l].assign(net.W[l].size(), 0);
		mB[l].assign(net.b[l].size(), 0);
		vB[l].assign(net.b[l].size(), 0);
	}
	long t = 0;
	double best = logloss_rows(va, R, corrections(net), Z, Wt);
	Net best_net = net;
	std::vector<size_t> idx = tr;
	std::vector<std::vector<double>> acts;
	for(int ep = 0; ep < cfg.epochs; ++ep) {
		std::shuffle(idx.begin(), idx.end(), rng);
		for(size_t s0 = 0; s0 < idx.size(); s0 += static_cast<size_t>(cfg.batch)) {
			const size_t s1 = std::min(idx.size(), s0 + static_cast<size_t>(cfg.batch));
			double wsum = 0;
			for(size_t k = s0; k < s1; ++k)
				wsum += Wt[idx[k]];
			for(size_t l = 0; l < L; ++l) {
				gW[l].assign(net.W[l].size(), 0);
				gB[l].assign(net.b[l].size(), 0);
			}
			for(size_t k = s0; k < s1; ++k) {
				const size_t r = idx[k];
				const double out = net.forward(&X[r * nf], &acts);
				const double p = sigmoid((R[r] + net.scale * out) / T);
				// d loss / d out
				std::vector<double> g{(p - Y[r]) * Wt[r] / T * net.scale / wsum};
				for(size_t l = L; l-- > 0;) {
					const int in = net.sizes[l], outn = net.sizes[l + 1];
					const std::vector<double>& a = acts[l];
					for(int o = 0; o < outn; ++o) {
						const double go = g[static_cast<size_t>(o)];
						gB[l][static_cast<size_t>(o)] += go;
						double* gw = &gW[l][static_cast<size_t>(o) * in];
						for(int c = 0; c < in; ++c)
							gw[c] += go * a[static_cast<size_t>(c)];
					}
					if(l > 0) {
						std::vector<double> gp(static_cast<size_t>(in), 0.0);
						for(int o = 0; o < outn; ++o) {
							const double go = g[static_cast<size_t>(o)];
							const double* w = &net.W[l][static_cast<size_t>(o) * in];
							for(int c = 0; c < in; ++c)
								gp[static_cast<size_t>(c)] += go * w[c];
						}
						for(int c = 0; c < in; ++c)
							gp[static_cast<size_t>(c)] *= 1 - a[static_cast<size_t>(c)] * a[static_cast<size_t>(c)];
						g.swap(gp);
					}
				}
			}
			++t;
			const double c1 = 1 - std::pow(0.9, static_cast<double>(t)), c2 = 1 - std::pow(0.999, static_cast<double>(t));
			auto step = [&](std::vector<double>& p, std::vector<double>& g, std::vector<double>& m, std::vector<double>& v, bool decay) {
				for(size_t k = 0; k < p.size(); ++k) {
					const double gk = g[k] + (decay ? cfg.l2 * p[k] : 0.0);
					m[k] = 0.9 * m[k] + 0.1 * gk;
					v[k] = 0.999 * v[k] + 0.001 * gk * gk;
					p[k] -= cfg.lr * (m[k] / c1) / (std::sqrt(v[k] / c2) + 1e-8);
				}
			};
			for(size_t l = 0; l < L; ++l) {
				step(net.W[l], gW[l], mW[l], vW[l], true);
				step(net.b[l], gB[l], mB[l], vB[l], false);
			}
		}
		const double vl = logloss_rows(va, R, corrections(net), Z, Wt);
		if(vl < best) {
			best = vl;
			best_net = net;
		}
		if(cfg.progress)
			cfg.progress(ep + 1, cfg.epochs);
		if(cfg.stop && cfg.stop())
			break;
	}
	res.train_logloss = logloss_rows(tr, R, corrections(best_net), Z, Wt);
	res.net_logloss = best;
	res.positions = static_cast<int>(n);
	res.games = static_cast<int>(games.size());
	res.validation_games = static_cast<int>(val_games.size());
	res.net_json = save_net(best_net, norm, id);
	res.ok = true;
	return res;
}

}  // namespace ygoe
