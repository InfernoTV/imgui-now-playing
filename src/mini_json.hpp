#pragma once
// Tiny JSON reader, just enough for lrclib.net responses (objects, arrays, strings with
// escapes incl. \u surrogate pairs, numbers, bools, null). Keeps the library dependency-free.
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace now_playing::json
{
	struct Value
	{
		enum class Type { Null, Bool, Number, String, Array, Object } type{ Type::Null };
		bool boolean{};
		double number{};
		std::string string{};
		std::vector<Value> array{};
		std::map<std::string, Value> object{};

		bool is_object() const { return type == Type::Object; }
		bool is_array() const { return type == Type::Array; }
		bool is_string() const { return type == Type::String; }

		const Value* find(const std::string& key) const
		{
			if (type != Type::Object) return nullptr;
			auto it = object.find(key);
			return it == object.end() ? nullptr : &it->second;
		}
		std::string str(const std::string& key) const
		{
			const Value* v = find(key);
			return v && v->is_string() ? v->string : std::string{};
		}
		double num(const std::string& key, double fallback = 0.0) const
		{
			const Value* v = find(key);
			return v && v->type == Type::Number ? v->number : fallback;
		}
	};

	namespace detail
	{
		struct Parser
		{
			std::string_view s;
			std::size_t i = 0;
			bool ok = true;

			void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
			bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }

			static void utf8(std::string& out, unsigned cp)
			{
				if (cp < 0x80) out += static_cast<char>(cp);
				else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
				else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
				else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
			}

			unsigned hex4()
			{
				if (i + 4 > s.size()) { ok = false; return 0; }
				unsigned v = 0;
				for (int k = 0; k < 4; k++)
				{
					const char c = s[i++];
					v <<= 4;
					if (c >= '0' && c <= '9') v |= c - '0';
					else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
					else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
					else ok = false;
				}
				return v;
			}

			std::string string()
			{
				std::string out;
				if (!eat('"')) { ok = false; return out; }
				while (i < s.size() && s[i] != '"')
				{
					char c = s[i++];
					if (c != '\\') { out += c; continue; }
					if (i >= s.size()) break;
					switch (s[i++])
					{
					case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
					case 'b': out += '\b'; break; case 'f': out += '\f'; break;
					case 'u':
					{
						unsigned cp = hex4();
						if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u')
						{
							i += 2;
							const unsigned lo = hex4();
							cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						}
						utf8(out, cp);
						break;
					}
					default: out += s[i - 1]; break;   // \" \\ \/
					}
				}
				if (i < s.size()) i++; else ok = false;
				return out;
			}

			Value value(int depth = 0)
			{
				Value v;
				if (depth > 64) { ok = false; return v; }
				ws();
				if (i >= s.size()) { ok = false; return v; }
				const char c = s[i];
				if (c == '{')
				{
					i++;
					v.type = Value::Type::Object;
					if (eat('}')) return v;
					do
					{
						std::string key = string();
						if (!eat(':')) { ok = false; return v; }
						v.object[key] = value(depth + 1);
					} while (ok && eat(','));
					if (!eat('}')) ok = false;
				}
				else if (c == '[')
				{
					i++;
					v.type = Value::Type::Array;
					if (eat(']')) return v;
					do { v.array.push_back(value(depth + 1)); } while (ok && eat(','));
					if (!eat(']')) ok = false;
				}
				else if (c == '"') { v.type = Value::Type::String; v.string = string(); }
				else if (s.substr(i, 4) == "true") { v.type = Value::Type::Bool; v.boolean = true; i += 4; }
				else if (s.substr(i, 5) == "false") { v.type = Value::Type::Bool; i += 5; }
				else if (s.substr(i, 4) == "null") { i += 4; }
				else
				{
					const std::string num(s.substr(i, std::min<std::size_t>(32, s.size() - i)));
					char* end = nullptr;
					v.number = std::strtod(num.c_str(), &end);
					if (end == num.c_str()) { ok = false; return v; }
					v.type = Value::Type::Number;
					i += static_cast<std::size_t>(end - num.c_str());
				}
				return v;
			}
		};
	}

	// returns a Null value on malformed input
	inline Value parse(std::string_view text)
	{
		detail::Parser p{ text };
		Value v = p.value();
		return p.ok ? v : Value{};
	}
}
