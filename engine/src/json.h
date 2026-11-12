// Minimal JSON reader/writer for the engine's request/response API (no dependencies).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ygoe::json {

class Value {
public:
	enum Type { Null, Bool, Number, String, Array, Object };
	Value() = default;
	Type type() const { return type_; }
	bool is_null() const { return type_ == Null; }
	bool is_array() const { return type_ == Array; }
	bool is_object() const { return type_ == Object; }

	double num(double def = 0) const { return type_ == Number ? num_ : def; }
	int64_t integer(int64_t def = 0) const { return type_ == Number ? static_cast<int64_t>(num_) : def; }
	bool boolean(bool def = false) const { return type_ == Bool ? b_ : def; }
	const std::string& str() const { return s_; }
	const std::vector<Value>& items() const { return arr_; }
	// Missing keys return a shared null value.
	const Value& operator[](const std::string& key) const;
	const Value& operator[](size_t i) const;
	bool has(const std::string& key) const { return obj_.count(key) > 0; }
	size_t size() const { return type_ == Array ? arr_.size() : obj_.size(); }

	static Value parse(const std::string& text, std::string* error = nullptr);

private:
	friend class Parser;
	Type type_ = Null;
	bool b_ = false;
	double num_ = 0;
	std::string s_;
	std::vector<Value> arr_;
	std::map<std::string, Value> obj_;
};

std::string quote(const std::string& s);

}  // namespace ygoe::json
