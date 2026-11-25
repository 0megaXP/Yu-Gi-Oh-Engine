// Training the value network (valuenet.h) in the engine itself: the same method as
// tools/ml/train.py (the nightly pipeline's), so a deck can learn on the person's computer
// (learn.h) without Python.
//
// Each position has the evaluator's features, its raw value (yours minus the opponent's) and
// the game's result z. The network learns a correction c so that sigmoid((raw + c) / 2.5)
// predicts the result. Targets are TD(lambda) (an early position that was fine isn't marked lost
// because of a later misplay); games are split 85/15 into training and validation; the epoch with
// the lowest validation log-loss is kept. Continuing from a network of the same shape keeps its
// normalisation.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ygoe {

struct NetSample {
	std::vector<float> f;
	double raw = 0;
	double z = 0.5;
	double w = 1;        // weight (real games count 3)
	std::string game;
	int me = 0;          // side, and order within the game from that side
	int i = 0;
};

// One position line as self-play writes it (`"f"`, `"raw"`, `"z"`, `"game"`, `"me"`, `"i"`,
// `"src"`); false for other lines (start positions, cut lines).
bool parse_net_sample(const std::string& line, NetSample& out);

struct NetTrainConfig {
	std::vector<int> hidden{32, 16};
	int epochs = 60;
	uint64_t seed = 1;
	double lambda = 0.95;
	double l2 = 1e-4;
	double lr = 3e-3;
	int batch = 256;
	double validation = 0.15;
	std::function<bool()> stop;                   // true: stop after this epoch
	std::function<void(int done, int total)> progress;
};

struct NetTrainResult {
	bool ok = false;
	std::string error;
	std::string net_json;        // the trained network (valuenet.h format)
	int positions = 0, games = 0, validation_games = 0;
	double base_logloss = 0;     // validation, the plain evaluator
	double init_logloss = 0;     // validation, the network it started from (= base when new)
	double net_logloss = 0;      // validation, the trained one
	double train_logloss = 0;
	std::string report_json() const;
};

// `init_json`: the network to continue from ("" for a new one; a different shape starts anew).
NetTrainResult train_value_net(const std::vector<NetSample>& samples, const std::string& init_json, const NetTrainConfig& cfg,
                               const std::string& id);

// Validation-style log-loss of a network (JSON, "" for none) on samples, against their results.
double net_logloss(const std::vector<NetSample>& samples, const std::string& net_json);

}  // namespace ygoe
