#pragma once

// Tolerant readers for model-written JSON. nlohmann's j.value("key", def) throws when the key
// exists with another type ("request": null, "qty": "two"); thrown on a SkyrimNet worker thread
// that would take the game down. These never throw.
namespace MundaneMatters::JsonUtil
{
	inline std::string Str(const json& j, const char* key, std::string fallback = {})
	{
		if (!j.is_object()) {
			return fallback;
		}
		auto it = j.find(key);
		if (it == j.end()) {
			return fallback;
		}
		if (it->is_string()) {
			return it->get<std::string>();
		}
		if (it->is_number() || it->is_boolean()) {
			return it->dump();
		}
		return fallback;
	}

	inline std::string Lower(std::string s)
	{
		std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	// "12", "12.5", "twelve", "a hundred", "two hundred and fifty", "a couple", "a few", "a dozen".
	inline std::optional<double> ParseNumber(std::string text)
	{
		text = Lower(std::move(text));
		std::smatch m;
		static const std::regex digits(R"((-?\d+(?:\.\d+)?))");
		if (std::regex_search(text, m, digits)) {
			try {
				return std::stod(m[1].str());
			} catch (...) {
			}
		}
		static const std::unordered_map<std::string, double> kWords{
			{ "a", 1 }, { "an", 1 }, { "one", 1 }, { "two", 2 }, { "three", 3 }, { "four", 4 }, { "five", 5 }, { "six", 6 },
			{ "seven", 7 }, { "eight", 8 }, { "nine", 9 }, { "ten", 10 }, { "eleven", 11 }, { "twelve", 12 },
			{ "thirteen", 13 }, { "fourteen", 14 }, { "fifteen", 15 }, { "sixteen", 16 }, { "seventeen", 17 },
			{ "eighteen", 18 }, { "nineteen", 19 }, { "twenty", 20 }, { "thirty", 30 }, { "forty", 40 },
			{ "fifty", 50 }, { "sixty", 60 }, { "seventy", 70 }, { "eighty", 80 }, { "ninety", 90 },
			{ "couple", 2 }, { "pair", 2 }, { "few", 3 }, { "several", 3 }, { "handful", 5 }, { "dozen", 12 }
		};
		double total = 0, current = 0;
		bool any = false;
		static const std::regex word(R"([a-z]+)");
		for (auto it = std::sregex_iterator(text.begin(), text.end(), word); it != std::sregex_iterator(); ++it) {
			const auto w = it->str();
			if (w == "hundred") {
				current = (current == 0 ? 1 : current) * 100;
				any = true;
			} else if (w == "thousand") {
				total += (current == 0 ? 1 : current) * 1000;
				current = 0;
				any = true;
			} else if (auto k = kWords.find(w); k != kWords.end()) {
				// "a dozen" / "a couple": the article multiplies, it doesn't add.
				if ((w == "dozen" || w == "couple" || w == "few" || w == "handful") && current == 1) {
					current = k->second;
				} else {
					current += k->second;
				}
				any = true;
			}
		}
		if (!any) {
			return std::nullopt;
		}
		return total + current;
	}

	// Lower-case alphanumeric words of a text.
	inline std::vector<std::string> Words(std::string_view text)
	{
		std::vector<std::string> out;
		std::string cur;
		for (char c : text) {
			if (std::isalnum(static_cast<unsigned char>(c))) {
				cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			} else if (!cur.empty()) {
				out.push_back(std::move(cur));
				cur.clear();
			}
		}
		if (!cur.empty()) {
			out.push_back(std::move(cur));
		}
		return out;
	}

	// Levenshtein distance: typo tolerance for names and key words ("Hlda" ~ "Hulda").
	inline std::size_t EditDistance(std::string_view a, std::string_view b)
	{
		std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
		for (std::size_t j = 0; j <= b.size(); ++j) {
			prev[j] = j;
		}
		for (std::size_t i = 1; i <= a.size(); ++i) {
			cur[0] = i;
			for (std::size_t j = 1; j <= b.size(); ++j) {
				cur[j] = (std::min)({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1) });
			}
			std::swap(prev, cur);
		}
		return prev[b.size()];
	}

	// A number written as a number, a numeric string or words; nullopt when absent or junk.
	inline std::optional<double> Num(const json& j, const char* key)
	{
		if (!j.is_object()) {
			return std::nullopt;
		}
		auto it = j.find(key);
		if (it == j.end() || it->is_null() || it->is_boolean()) {
			return std::nullopt;
		}
		if (it->is_number()) {
			return it->get<double>();
		}
		if (it->is_string()) {
			return ParseNumber(it->get<std::string>());
		}
		return std::nullopt;
	}
}
