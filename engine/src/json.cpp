#include "json.h"

#include <cstdio>
#include <cstdlib>

namespace ygoe::json {

namespace {
const Value& null_value() {
	static const Value v;
	return v;
}
}  // namespace

const Value& Value::operator[](const std::string& key) const {
	if(type_ != Object)
		return null_value();
	auto it = obj_.find(key);
	return it == obj_.end() ? null_value() : it->second;
}

const Value& Value::operator[](size_t i) const {
	if(type_ != Array || i >= arr_.size())
		return null_value();
	return arr_[i];
}

class Parser {
public:
	explicit Parser(const std::string& t) : t_(t) {}

	bool parse(Value& out, std::string* err) {
		bool ok = value(out) && (ws(), p_ == t_.size());
		if(!ok && err)
			*err = "invalid JSON near offset " + std::to_string(p_);
		return ok;
	}

private:
	void ws() {
		while(p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\n' || t_[p_] == '\r' || t_[p_] == '\t'))
			++p_;
	}
	bool lit(const char* s) {
		size_t n = std::char_traits<char>::length(s);
		if(t_.compare(p_, n, s) != 0)
			return false;
		p_ += n;
		return true;
	}
	bool value(Value& v) {
		ws();
		if(p_ >= t_.size())
			return false;
		char c = t_[p_];
		if(c == '{')
			return object(v);
		if(c == '[')
			return array(v);
		if(c == '"') {
			v.type_ = Value::String;
			return string(v.s_);
		}
		if(lit("true")) {
			v.type_ = Value::Bool;
			v.b_ = true;
			return true;
		}
		if(lit("false")) {
			v.type_ = Value::Bool;
			return true;
		}
		if(lit("null"))
			return true;
		const char* start = t_.c_str() + p_;
		char* end = nullptr;
		double d = std::strtod(start, &end);
		if(end == start)
			return false;
		p_ += static_cast<size_t>(end - start);
		v.type_ = Value::Number;
		v.num_ = d;
		return true;
	}
	bool string(std::string& out) {
		++p_;
		while(p_ < t_.size()) {
			char c = t_[p_++];
			if(c == '"')
				return true;
			if(c != '\\') {
				out += c;
				continue;
			}
			if(p_ >= t_.size())
				return false;
			char e = t_[p_++];
			switch(e) {
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'u': {
				if(p_ + 4 > t_.size())
					return false;
				unsigned cp = static_cast<unsigned>(std::strtoul(t_.substr(p_, 4).c_str(), nullptr, 16));
				p_ += 4;
				if(cp >= 0xD800 && cp <= 0xDBFF && t_.compare(p_, 2, "\\u") == 0) {
					unsigned lo = static_cast<unsigned>(std::strtoul(t_.substr(p_ + 2, 4).c_str(), nullptr, 16));
					p_ += 6;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				if(cp < 0x80) {
					out += static_cast<char>(cp);
				} else if(cp < 0x800) {
					out += static_cast<char>(0xC0 | (cp >> 6));
					out += static_cast<char>(0x80 | (cp & 0x3F));
				} else if(cp < 0x10000) {
					out += static_cast<char>(0xE0 | (cp >> 12));
					out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (cp & 0x3F));
				} else {
					out += static_cast<char>(0xF0 | (cp >> 18));
					out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
					out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (cp & 0x3F));
				}
				break;
			}
			default: out += e; break;
			}
		}
		return false;
	}
	bool array(Value& v) {
		v.type_ = Value::Array;
		++p_;
		ws();
		if(p_ < t_.size() && t_[p_] == ']') {
			++p_;
			return true;
		}
		while(true) {
			Value item;
			if(!value(item))
				return false;
			v.arr_.push_back(std::move(item));
			ws();
			if(p_ < t_.size() && t_[p_] == ',') {
				++p_;
				continue;
			}
			if(p_ < t_.size() && t_[p_] == ']') {
				++p_;
				return true;
			}
			return false;
		}
	}
	bool object(Value& v) {
		v.type_ = Value::Object;
		++p_;
		ws();
		if(p_ < t_.size() && t_[p_] == '}') {
			++p_;
			return true;
		}
		while(true) {
			ws();
			std::string key;
			if(p_ >= t_.size() || t_[p_] != '"' || !string(key))
				return false;
			ws();
			if(p_ >= t_.size() || t_[p_] != ':')
				return false;
			++p_;
			Value item;
			if(!value(item))
				return false;
			v.obj_[key] = std::move(item);
			ws();
			if(p_ < t_.size() && t_[p_] == ',') {
				++p_;
				continue;
			}
			if(p_ < t_.size() && t_[p_] == '}') {
				++p_;
				return true;
			}
			return false;
		}
	}

	const std::string& t_;
	size_t p_ = 0;
};

Value Value::parse(const std::string& text, std::string* error) {
	Value v;
	Parser p(text);
	if(!p.parse(v, error))
		return Value();
	return v;
}

std::string quote(const std::string& s) {
	std::string o = "\"";
	for(char c : s) {
		switch(c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if(static_cast<unsigned char>(c) < 0x20) {
				char b[8];
				std::snprintf(b, sizeof b, "\\u%04x", c);
				o += b;
			} else {
				o += c;
			}
		}
	}
	return o + "\"";
}

}  // namespace ygoe::json
