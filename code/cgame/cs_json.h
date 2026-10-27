/*
JK BUILD: Cinematic Studio - a small JSON value with a reader and a writer (no dependencies).
Used for project files. Numbers are doubles; object keys keep their insertion order.
*/
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cs
{
	class Json
	{
	public:
		enum Type { Null, Bool, Number, String, Array, Object };

		Json() = default;
		Json(bool b) : type_(Bool), b_(b) {}
		Json(int n) : type_(Number), n_(n) {}
		Json(double n) : type_(Number), n_(n) {}
		Json(const char* s) : type_(String), s_(s ? s : "") {}
		Json(const std::string& s) : type_(String), s_(s) {}

		static Json MakeArray() { Json j; j.type_ = Array; return j; }
		static Json MakeObject() { Json j; j.type_ = Object; return j; }

		Type type() const { return type_; }
		bool isNull() const { return type_ == Null; }
		bool isObject() const { return type_ == Object; }
		bool isArray() const { return type_ == Array; }

		// typed reads with defaults (never throw)
		bool asBool(bool def = false) const { return type_ == Bool ? b_ : def; }
		double asNumber(double def = 0.0) const { return type_ == Number ? n_ : def; }
		int asInt(int def = 0) const { return type_ == Number ? static_cast<int>(n_) : def; }
		std::string asString(const std::string& def = "") const { return type_ == String ? s_ : def; }

		// arrays
		size_t size() const { return type_ == Array ? arr_.size() : (type_ == Object ? keys_.size() : 0); }
		const Json& operator[](size_t i) const;
		Json& push(const Json& v);

		// objects
		bool has(const std::string& k) const { return type_ == Object && obj_.count(k) != 0; }
		const Json& get(const std::string& k) const;
		Json& set(const std::string& k, const Json& v);
		const std::vector<std::string>& keys() const { return keys_; }

		std::string dump(int indent = 2) const;
		// returns false and fills err on malformed input
		static bool parse(const std::string& text, Json& out, std::string* err = nullptr);

	private:
		void dumpTo(std::string& o, int indent, int depth) const;

		Type type_ = Null;
		bool b_ = false;
		double n_ = 0.0;
		std::string s_;
		std::vector<Json> arr_;
		std::map<std::string, Json> obj_;
		std::vector<std::string> keys_;
	};
}
