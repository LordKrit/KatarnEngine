/*
JK BUILD: Cinematic Studio - JSON reader/writer (see cs_json.h).
*/
#include "cs_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cs
{
	static const Json kNull;

	const Json& Json::operator[](const size_t i) const
	{
		return (type_ == Array && i < arr_.size()) ? arr_[i] : kNull;
	}

	Json& Json::push(const Json& v)
	{
		if (type_ != Array) { *this = MakeArray(); }
		arr_.push_back(v);
		return arr_.back();
	}

	const Json& Json::get(const std::string& k) const
	{
		if (type_ != Object) return kNull;
		const auto it = obj_.find(k);
		return it == obj_.end() ? kNull : it->second;
	}

	Json& Json::set(const std::string& k, const Json& v)
	{
		if (type_ != Object) { *this = MakeObject(); }
		if (!obj_.count(k)) keys_.push_back(k);
		obj_[k] = v;
		return obj_[k];
	}

	static void EscapeTo(std::string& o, const std::string& s)
	{
		o += '"';
		for (const unsigned char c : s)
		{
			switch (c)
			{
			case '"': o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\n': o += "\\n"; break;
			case '\r': o += "\\r"; break;
			case '\t': o += "\\t"; break;
			default:
				if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
				else o += static_cast<char>(c);
			}
		}
		o += '"';
	}

	void Json::dumpTo(std::string& o, const int indent, const int depth) const
	{
		const std::string pad = indent > 0 ? std::string(static_cast<size_t>(indent) * (depth + 1), ' ') : "";
		const std::string padEnd = indent > 0 ? std::string(static_cast<size_t>(indent) * depth, ' ') : "";
		const char* nl = indent > 0 ? "\n" : "";
		switch (type_)
		{
		case Null: o += "null"; break;
		case Bool: o += b_ ? "true" : "false"; break;
		case Number:
		{
			char b[64];
			if (std::isfinite(n_) && n_ == std::floor(n_) && std::fabs(n_) < 1e15) snprintf(b, sizeof b, "%.0f", n_);
			else if (std::isfinite(n_)) snprintf(b, sizeof b, "%.9g", n_);
			else snprintf(b, sizeof b, "0");
			o += b;
			break;
		}
		case String: EscapeTo(o, s_); break;
		case Array:
			if (arr_.empty()) { o += "[]"; break; }
			// short arrays of numbers (vectors) stay on one line
			{
				bool flat = arr_.size() <= 4;
				for (const auto& e : arr_) flat = flat && e.type_ == Number;
				o += '[';
				for (size_t i = 0; i < arr_.size(); i++)
				{
					if (!flat) { o += nl; o += pad; }
					arr_[i].dumpTo(o, indent, depth + 1);
					if (i + 1 < arr_.size()) o += flat ? ", " : ",";
				}
				if (!flat) { o += nl; o += padEnd; }
				o += ']';
			}
			break;
		case Object:
			if (keys_.empty()) { o += "{}"; break; }
			o += '{';
			for (size_t i = 0; i < keys_.size(); i++)
			{
				o += nl; o += pad;
				EscapeTo(o, keys_[i]);
				o += indent > 0 ? ": " : ":";
				obj_.at(keys_[i]).dumpTo(o, indent, depth + 1);
				if (i + 1 < keys_.size()) o += ',';
			}
			o += nl; o += padEnd; o += '}';
			break;
		}
	}

	std::string Json::dump(const int indent) const
	{
		std::string o;
		dumpTo(o, indent, 0);
		if (indent > 0) o += '\n';
		return o;
	}

	namespace
	{
		struct Parser
		{
			const std::string& t;
			size_t p = 0;
			std::string err;

			explicit Parser(const std::string& text) : t(text) {}

			void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) p++; }
			bool fail(const char* m) { if (err.empty()) { err = m; err += " at offset " + std::to_string(p); } return false; }

			bool lit(const char* w)
			{
				size_t n = 0;
				while (w[n]) n++;
				if (t.compare(p, n, w) != 0) return fail("bad literal");
				p += n;
				return true;
			}

			bool str(std::string& out)
			{
				if (p >= t.size() || t[p] != '"') return fail("expected string");
				p++;
				while (p < t.size() && t[p] != '"')
				{
					char c = t[p++];
					if (c == '\\')
					{
						if (p >= t.size()) return fail("bad escape");
						const char e = t[p++];
						switch (e)
						{
						case 'n': out += '\n'; break;
						case 't': out += '\t'; break;
						case 'r': out += '\r'; break;
						case 'b': out += '\b'; break;
						case 'f': out += '\f'; break;
						case 'u':
						{
							if (p + 4 > t.size()) return fail("bad \\u escape");
							const unsigned cp = static_cast<unsigned>(strtoul(t.substr(p, 4).c_str(), nullptr, 16));
							p += 4;
							// UTF-8 encode (BMP only; enough for names and paths)
							if (cp < 0x80) out += static_cast<char>(cp);
							else if (cp < 0x800) { out += static_cast<char>(0xC0 | cp >> 6); out += static_cast<char>(0x80 | (cp & 0x3F)); }
							else { out += static_cast<char>(0xE0 | cp >> 12); out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
							break;
						}
						default: out += e;
						}
					}
					else out += c;
				}
				if (p >= t.size()) return fail("unterminated string");
				p++;
				return true;
			}

			bool value(Json& out, int depth)
			{
				if (depth > 64) return fail("nesting too deep");
				ws();
				if (p >= t.size()) return fail("unexpected end");
				const char c = t[p];
				if (c == '{')
				{
					p++;
					out = Json::MakeObject();
					ws();
					if (p < t.size() && t[p] == '}') { p++; return true; }
					for (;;)
					{
						ws();
						std::string k;
						if (!str(k)) return false;
						ws();
						if (p >= t.size() || t[p] != ':') return fail("expected ':'");
						p++;
						Json v;
						if (!value(v, depth + 1)) return false;
						out.set(k, v);
						ws();
						if (p < t.size() && t[p] == ',') { p++; continue; }
						if (p < t.size() && t[p] == '}') { p++; return true; }
						return fail("expected ',' or '}'");
					}
				}
				if (c == '[')
				{
					p++;
					out = Json::MakeArray();
					ws();
					if (p < t.size() && t[p] == ']') { p++; return true; }
					for (;;)
					{
						Json v;
						if (!value(v, depth + 1)) return false;
						out.push(v);
						ws();
						if (p < t.size() && t[p] == ',') { p++; continue; }
						if (p < t.size() && t[p] == ']') { p++; return true; }
						return fail("expected ',' or ']'");
					}
				}
				if (c == '"') { std::string s; if (!str(s)) return false; out = Json(s); return true; }
				if (c == 't') { if (!lit("true")) return false; out = Json(true); return true; }
				if (c == 'f') { if (!lit("false")) return false; out = Json(false); return true; }
				if (c == 'n') { if (!lit("null")) return false; out = Json(); return true; }
				if (c == '-' || (c >= '0' && c <= '9'))
				{
					char* end = nullptr;
					const double d = strtod(t.c_str() + p, &end);
					if (end == t.c_str() + p) return fail("bad number");
					p = static_cast<size_t>(end - t.c_str());
					out = Json(d);
					return true;
				}
				return fail("unexpected character");
			}
		};
	}

	bool Json::parse(const std::string& text, Json& out, std::string* err)
	{
		Parser ps(text);
		Json v;
		bool ok = ps.value(v, 0);
		if (ok) { ps.ws(); if (ps.p != text.size()) ok = ps.fail("trailing characters"); }
		if (!ok) { if (err) *err = ps.err; return false; }
		out = v;
		return true;
	}
}
