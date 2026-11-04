#include "i18n.h"
#include "json.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

namespace banana
{
	namespace
	{
		std::map<std::string, std::string> strings;
		std::string code = "en", dir = "/usr/share/banana/lang", dataRoot;
		std::string trim(const std::string& s)
		{
			size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
			return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
		}
	}

	bool i18nSet(const std::string& lang)
	{
		strings.clear();
		code = lang.empty() ? "en" : lang;
		if (code == "en") return true;
		if (const char* d = getenv("BANANA_LANG_DIR")) dir = d;	// the tests' catalogs
		std::string file = dataRoot + "/lang/" + code + ".json";	// a copy on the card wins, like a theme
		JPtr doc = readJsonFile(file);
		if (!doc) { file = dir + "/" + code + ".json"; doc = readJsonFile(file); }
		const JVal* s = doc ? doc->get("strings") : nullptr;
		if (s && s->isObj())
			for (auto& kv : s->obj)
				if (kv.second && kv.second->isStr() && !kv.second->s.empty()) strings[kv.first] = kv.second->s;
		fprintf(stderr, "banana-ui: i18n: %s (%zu strings, %s)\n", code.c_str(), strings.size(), file.c_str());
		return !strings.empty();
	}

	void i18nInit(const std::string& dataDir)
	{
		std::string lang = "en", ln;
		dataRoot = dataDir;
		std::ifstream f(dataDir + "/settings.conf");
		while (std::getline(f, ln))
		{
			size_t eq = ln.find('=');
			if (eq == std::string::npos || ln[0] == '#') continue;
			if (trim(ln.substr(0, eq)) == "lang") lang = trim(ln.substr(eq + 1));
		}
		i18nSet(lang);
	}

	std::string tr(const std::string& english)
	{
		auto it = strings.find(english);
		return it == strings.end() ? english : it->second;
	}

	const std::string& i18nCode() { return code; }
}
