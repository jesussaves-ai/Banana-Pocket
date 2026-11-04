# pragma once
// The menu's text in other languages. The English string is the key: tr("Theme") returns the active language's
// "Tema"/"Design", or the English itself when there is no catalog or no entry. Catalogs are JSON files in
// /usr/share/banana/lang (BANANA_LANG_DIR points elsewhere, the tests do), one per language:
//   { "name": "Español", "strings": { "Theme": "Tema", ... } }
// A catalog is user-editable on the card like a theme; a missing one falls back to English.
#include <string>

namespace banana
{
	// read settings.conf's `lang` under dataDir and load that catalog (the menu calls this once at start)
	void i18nInit(const std::string& dataDir);
	// switch at once (Settings > Display & sound > Language); false when that catalog could not be read
	bool i18nSet(const std::string& code);
	std::string tr(const std::string& english);
	const std::string& i18nCode();
}
