#include "valuenet.h"

#include <cmath>
#include <fstream>
#include <sstream>

#include "json.h"

namespace ygoe {

namespace {
std::vector<float> floats(const json::Value& v) {
	std::vector<float> out;
	out.reserve(v.size());
	for(size_t i = 0; i < v.size(); ++i)
		out.push_back(static_cast<float>(v[i].num(0)));
	return out;
}
}  // namespace

bool ValueNet::parse(const std::string& text, std::string* err) {
	std::string e;
	auto v = json::Value::parse(text, &e);
	auto fail = [&](const std::string& m) {
		if(err)
			*err = m;
		layers_.clear();
		return false;
	};
	if(!v.is_object())
		return fail("value net: " + (e.empty() ? std::string("not a JSON object") : e));
	mean_ = floats(v["mean"]);
	std_ = floats(v["std"]);
	scale_ = v["scale"].num(1.0);
	id_ = v["id"].str();
	if(mean_.empty() || std_.size() != mean_.size())
		return fail("value net: mean/std missing or of different sizes");
	layers_.clear();
	int in = static_cast<int>(mean_.size());
	for(const auto& l : v["layers"].items()) {
		Layer L;
		L.in = static_cast<int>(l["in"].integer(0));
		L.out = static_cast<int>(l["out"].integer(0));
		L.w = floats(l["w"]);
		L.b = floats(l["b"]);
		if(L.in != in || L.out <= 0 || L.w.size() != static_cast<size_t>(L.in) * L.out || L.b.size() != static_cast<size_t>(L.out))
			return fail("value net: layer sizes don't match");
		in = L.out;
		layers_.push_back(std::move(L));
	}
	if(layers_.empty() || in != 1)
		return fail("value net: the last layer must have one output");
	return true;
}

bool ValueNet::load_file(const std::string& path, std::string* err) {
	std::ifstream f(path);
	if(!f) {
		if(err)
			*err = "can't read " + path;
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	return parse(ss.str(), err);
}

double ValueNet::correction(const std::vector<float>& x) const {
	if(layers_.empty() || x.size() != mean_.size())
		return 0.0;
	std::vector<float> a(x.size());
	for(size_t i = 0; i < x.size(); ++i)
		a[i] = (x[i] - mean_[i]) / (std_[i] > 1e-6f ? std_[i] : 1.0f);
	for(size_t li = 0; li < layers_.size(); ++li) {
		const Layer& L = layers_[li];
		std::vector<float> o(static_cast<size_t>(L.out));
		for(int r = 0; r < L.out; ++r) {
			float s = L.b[static_cast<size_t>(r)];
			const float* w = &L.w[static_cast<size_t>(r) * L.in];
			for(int c = 0; c < L.in; ++c)
				s += w[c] * a[static_cast<size_t>(c)];
			o[static_cast<size_t>(r)] = li + 1 < layers_.size() ? std::tanh(s) : s;
		}
		a.swap(o);
	}
	const double out = scale_ * a[0];
	return std::isfinite(out) ? out : 0.0;
}

}  // namespace ygoe
