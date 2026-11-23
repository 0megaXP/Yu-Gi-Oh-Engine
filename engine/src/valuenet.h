// A small learned value network: a correction to the hand-written evaluator.
//
// The evaluator turns a position into a raw value (yours minus the opponent's) and a score
// sigmoid(raw / 2.5). The network reads ~50 numbers describing the position (Evaluator::features)
// and adds a learned amount to raw, trained so that the corrected score predicts who wins
// (tools/ml/train.py, from self-play games). With no network loaded nothing changes.
//
// File format (JSON): {"version":1, "features":N, "mean":[N], "std":[N],
//   "layers":[{"in":N,"out":H,"w":[H*N row-major],"b":[H]}, ...], "scale":s}
// Hidden layers use tanh, the last one is linear with one output; the result times `scale` is
// the correction to raw.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace ygoe {

class ValueNet {
public:
	bool parse(const std::string& json, std::string* err = nullptr);
	bool load_file(const std::string& path, std::string* err = nullptr);
	// Correction to the evaluator's raw value for this position (features as Evaluator::features).
	double correction(const std::vector<float>& features) const;
	size_t inputs() const { return mean_.size(); }
	const std::string& id() const { return id_; }

private:
	struct Layer {
		int in = 0, out = 0;
		std::vector<float> w, b;
	};
	std::vector<float> mean_, std_;
	std::vector<Layer> layers_;
	double scale_ = 1.0;
	std::string id_;
};

}  // namespace ygoe
