/*
 * Copyright 2005 - 2016 Zarafa and its licensors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <kopano/zcdefs.h>
#include <kopano/platform.h>
#include <memory>
#include <mutex>
#include <utility>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <sys/stat.h>
#include <kopano/lockhelper.hpp>
#include <kopano/memory.hpp>
#include <kopano/stringutil.h>
#include "ECConfigImpl.h"

#include <kopano/charset/convert.h>

using namespace std;
using namespace KCHL;

namespace KC {

const directive_t ECConfigImpl::s_sDirectives[] = {
	{ "include",	&ECConfigImpl::HandleInclude },
	{ "propmap",	&ECConfigImpl::HandlePropMap },
	{ NULL }
};

// Configuration file parser

ECConfigImpl::ECConfigImpl(const configsetting_t *lpDefaults,
    const char *const *lpszDirectives) :
	m_lpDefaults(lpDefaults)
{
	// allowed directives in this config object
	for (int i = 0; lpszDirectives != NULL && lpszDirectives[i] != NULL; ++i)
		m_lDirectives.push_back(lpszDirectives[i]);

	InitDefaults(LOADSETTING_INITIALIZING | LOADSETTING_UNKNOWN | LOADSETTING_OVERWRITE);
}

bool ECConfigImpl::LoadSettings(const char *szFilename)
{
	m_szConfigFile = szFilename;	
	return InitConfigFile(LOADSETTING_OVERWRITE);
}

static int tounderscore(int c)
{
	return c == '-' ? '_' : c;
}

/**
 * Parse commandline parameters to override the values loaded from the
 * config files.
 * 
 * This function accepts only long options in the form
 * --option-name=value. All dashes in the option-name will be converted
 * to underscores. The option-name should then match a valid config option.
 * This config option will be set to value. No processing is done on value
 * except for removing leading and trailing whitespaces.
 * 
 * The aray in argv will be reordered so all non-long-option values will
 * be located after the long-options. On return *lpargidx will be the
 * index of the first non-long-option in the array.
 * 
 * @param[in]	argc		The number of arguments to parse.
 * @param[in]	argv		The parameters to parse. The size of the
 * 							array must be at least argc.
 * @param[out]	lpargidx	Pointer to an integer that will be set to
 * 							the index in argv where parsing stopped.
 * 							This parameter may be NULL.
 * @retval	true
 */
int ECConfigImpl::ParseParams(int argc, char **argv)
{
	for (int i = 0; i < argc; ++i) {
		char *arg = argv[i];
		if (arg == nullptr)
			continue;
		if (arg[0] != '-' || arg[1] != '-') {
			// Move non-long-option to end of list
			--argc;
			for (int j = i; j < argc; ++j)
				argv[j] = argv[j+1];
			argv[argc] = arg;
			--i;
			continue;
		}
		const char *eq = strchr(arg, '=');
		if (eq == nullptr) {
			errors.push_back("Commandline option '" + string(arg+2) + "' cannot be empty!");
			continue;
		}
		string strName(arg+2, eq-arg-2);
		string strValue(eq+1);
		strName = trim(strName, " \t\r\n");
		strValue = trim(strValue, " \t\r\n");
		std::transform(strName.begin(), strName.end(), strName.begin(), tounderscore);
		configsetting_t setting = {strName.c_str(), strValue.c_str(), 0, 0};
		// Overwrite an existing setting, and make sure it is not reloadable during HUP
		AddSetting(&setting, LOADSETTING_OVERWRITE | LOADSETTING_CMDLINE_PARAM);
	}
	return argc;
}

bool ECConfigImpl::ReloadSettings()
{
	// unsetting this value isn't possible
	if (!m_szConfigFile)
		return false;

	// Check if we can still open the main config file. Do not reset to Defaults
	FILE *fp = fopen(m_szConfigFile, "rt");
	if (fp == nullptr)
		return false;
	fclose(fp);

	// reset to defaults because unset items in config file should return to default values.
	InitDefaults(LOADSETTING_OVERWRITE_RELOAD);

	return InitConfigFile(LOADSETTING_OVERWRITE_RELOAD);
}

bool ECConfigImpl::AddSetting(const char *szName, const char *szValue, const unsigned int ulGroup)
{
	configsetting_t sSetting;

	sSetting.szName = szName;
	sSetting.szValue = szValue;
	sSetting.ulFlags = 0;
	sSetting.ulGroup = ulGroup;

	return AddSetting(&sSetting, ulGroup ? LOADSETTING_OVERWRITE_GROUP : LOADSETTING_OVERWRITE);
}

static void freeSettings(settingmap_t::value_type &entry)
{
	// see InsertOrReplace
	delete [] entry.second;
}

void ECConfigImpl::CleanupMap(settingmap_t *lpMap)
{
	if (!lpMap->empty())
		for_each(lpMap->begin(), lpMap->end(), freeSettings);
}

ECConfigImpl::~ECConfigImpl()
{
	std::lock_guard<KC::shared_mutex> lset(m_settingsRWLock);
	CleanupMap(&m_mapSettings);
	CleanupMap(&m_mapAliases);
}

/** 
 * Returns the size in bytes for a size marked config value
 * 
 * @param szValue input value from config file
 * 
 * @return size in bytes
 */
size_t ECConfigImpl::GetSize(const char *szValue)
{
	if (szValue == nullptr)
		return 0;
	char *end = NULL;
	unsigned long long rv = strtoull(szValue, &end, 10);
	if (rv == 0 || end <= szValue || *end == '\0')
		return rv;
	while (*end != '\0' && (*end == ' ' || *end == '\t'))
		++end;
	switch (tolower(*end)) {
	case 'k': return rv << 10; break;
	case 'm': return rv << 20; break;
	case 'g': return rv << 30; break;
	}
	return rv;
}

/** 
 * Adds a new setting to the map, or replaces the current data.
 * Only the first 1024 bytes of the value are saved, longer values are truncated.
 * The map must be locked by the m_settingsRWLock.
 * 
 * @param lpMap settings map to set value in
 * @param s key to access map point
 * @param szValue new value to set in map
 */
void ECConfigImpl::InsertOrReplace(settingmap_t *lpMap, const settingkey_t &s, const char* szValue, bool bIsSize)
{
	char* data = NULL;
	size_t len = std::min((size_t)1023, strlen(szValue));

	auto i = lpMap->find(s);
	if (i == lpMap->cend()) {
		// Insert new value
		data = new char[1024];
		lpMap->insert(make_pair(s, data));
	} else {
		// Actually remove and re-insert the map entry since we may be modifying
		// ulFlags in the key (this is a bit of a hack, since you shouldn't be modifying
		// stuff in the key, but this is the easiest)
		data = i->second;
		lpMap->erase(i);
		lpMap->insert(make_pair(s, data));
	}
	
	if (bIsSize)
		len = snprintf(data, 1024, "%zu", GetSize(szValue));
	else
		strncpy(data, szValue, len);
	data[len] = '\0';
}

const char *ECConfigImpl::GetMapEntry(const settingmap_t *lpMap,
    const char *szName)
{
	const char *retval = NULL;
	if (szName == NULL)
		return NULL;

	settingkey_t key = {""};
	if (strlen(szName) >= sizeof(key.s))
		return NULL;

	strcpy(key.s, szName);

	KC::shared_lock<KC::shared_mutex> lset(m_settingsRWLock);
	auto itor = lpMap->find(key);
	if (itor != lpMap->cend())
		retval = itor->second;
	return retval;
}

const char *ECConfigImpl::GetSetting(const char *szName)
{
	return GetMapEntry(&m_mapSettings, szName);
}

const char *ECConfigImpl::GetAlias(const char *szName)
{
	return GetMapEntry(&m_mapAliases, szName);
}

const char *ECConfigImpl::GetSetting(const char *szName, const char *equal,
    const char *other)
{
	const char *value = this->GetSetting(szName);
	if (value == equal || (value && equal && !strcmp(value, equal)))
		return other;
	else
		return value;
}

const wchar_t *ECConfigImpl::GetSettingW(const char *szName)
{
	const char *value = GetSetting(szName);
	auto result = m_convertCache.insert({value, L""});
	auto iter = result.first;
	if (result.second)
		iter->second = convert_to<wstring>(value);

	return iter->second.c_str();
}

const wchar_t *ECConfigImpl::GetSettingW(const char *szName,
    const wchar_t *equal, const wchar_t *other)
{
	const wchar_t *value = this->GetSettingW(szName);
	if (value == equal || (value && equal && !wcscmp(value, equal)))
		return other;
	else
		return value;
}

list<configsetting_t> ECConfigImpl::GetSettingGroup(unsigned int ulGroup)
{
	list<configsetting_t> lGroup;
	configsetting_t sSetting;

	for (const auto &s : m_mapSettings)
		if ((s.first.ulGroup & ulGroup) == ulGroup &&
		    CopyConfigSetting(&s.first, s.second, &sSetting))
			lGroup.push_back(std::move(sSetting));
	return lGroup;
}

std::list<configsetting_t> ECConfigImpl::GetAllSettings()
{
	list<configsetting_t> lSettings;
	configsetting_t sSetting;

	for (const auto &s : m_mapSettings)
		if (CopyConfigSetting(&s.first, s.second, &sSetting))
			lSettings.push_back(std::move(sSetting));
	return lSettings;
}

bool ECConfigImpl::InitDefaults(unsigned int ulFlags)
{
	unsigned int i = 0;

	/* throw error? this is unacceptable! useless object, since it won't set new settings */
	if (!m_lpDefaults)
		return false;

	while (m_lpDefaults[i].szName != NULL) {
		if (m_lpDefaults[i].ulFlags & CONFIGSETTING_ALIAS) {
			/* Aliases are only initialized once */
			if (ulFlags & LOADSETTING_INITIALIZING)
				AddAlias(&m_lpDefaults[i]);
		} else
			AddSetting(&m_lpDefaults[i], ulFlags);
		++i;
	}

	return true;
}

bool ECConfigImpl::InitConfigFile(unsigned int ulFlags)
{
	bool bResult = false;

	assert(m_readFiles.empty());

	if (!m_szConfigFile)
		return false;

	bResult = ReadConfigFile(m_szConfigFile, ulFlags);

	m_readFiles.clear();

	return bResult;
}

bool ECConfigImpl::ReadConfigFile(const std::string &file,
    unsigned int ulFlags, unsigned int ulGroup)
{
	FILE *fp = NULL;
	bool bReturn = false;
	char cBuffer[MAXLINELEN] = {0};
	string strFilename;
	string strLine;
	string strName;
	string strValue;
	size_t pos;

	std::unique_ptr<char, cstdlib_deleter> normalized_file(realpath(file.c_str(), nullptr));
	if (normalized_file == nullptr) {
		errors.push_back("Cannot normalize path \"" + file + "\": " + strerror(errno));
		return false;
	}
	struct stat sb;
	if (stat(normalized_file.get(), &sb) < 0) {
		errors.push_back("Config file \"" + file + "\" cannot be read: " + strerror(errno));
		return false;
	}
	if (!S_ISREG(sb.st_mode)) {
		errors.push_back("Config file \"" + file + "\" is not a file");
		return false;
	}

	// Store the path of the previous file in case we're recursively processing files.
	// We need to keep track of the current path so we can handle relative includes in HandleInclude
	std::string prevFile = m_currentFile;
	m_currentFile = normalized_file.get();

	/* Check if we read this file before. */
	if (std::find(m_readFiles.cbegin(), m_readFiles.cend(), m_currentFile) != m_readFiles.cend()) {
		bReturn = true;
		goto exit;
	}

	m_readFiles.insert(m_currentFile);
	fp = fopen(file.c_str(), "rt");
	if (fp == nullptr) {
		errors.push_back("Unable to open config file \"" + file + "\"");
		goto exit;
	}

	while (!feof(fp)) {
		memset(&cBuffer, 0, sizeof(cBuffer));

		if (!fgets(cBuffer, sizeof(cBuffer), fp))
			continue;

		strLine = string(cBuffer);

		/* Skip empty lines any lines which start with # */
		if (strLine.empty() || strLine[0] == '#')
 			continue;

		/* Handle special directives which start with '!' */
		if (strLine[0] == '!') {
			if (!HandleDirective(strLine, ulFlags))
				goto exit;
			continue;
		}

		/* Get setting name */
		pos = strLine.find('=');
		if (pos != string::npos) {
			strName = strLine.substr(0, pos);
			strValue = strLine.substr(pos + 1);
		} else
			continue;

		/*
		 * The line is build up like this:
		 * config_name = bla bla
		 *
		 * Whe should clean it in such a way that it resolves to:
		 * config_name=bla bla
		 *
		 * Be careful _not_ to remove any whitespace characters
		 * within the configuration value itself.
		 */
		strName = trim(strName, " \t\r\n");
		strValue = trim(strValue, " \t\r\n");

		if(!strName.empty()) {
			// Save it
			configsetting_t setting = { strName.c_str(), strValue.c_str(), 0, static_cast<unsigned short int>(ulGroup) };
			AddSetting(&setting, ulFlags);
		}
	}

	bReturn = true;

exit:
	if(fp) 
		fclose(fp);

	// Restore the path of the previous file.
	m_currentFile = std::move(prevFile);
	return bReturn;
}

bool ECConfigImpl::HandleDirective(const string &strLine, unsigned int ulFlags)
{
	size_t pos = strLine.find_first_of(" \t", 1);
	string strName = strLine.substr(1, pos - 1);

	/* Check if this directive is known */
	for (int i = 0; s_sDirectives[i].lpszDirective != NULL; ++i) {
		if (strName.compare(s_sDirectives[i].lpszDirective) != 0)
			continue;
		/* Check if this directive is supported */
		auto f = find(m_lDirectives.cbegin(), m_lDirectives.cend(), strName);
		if (f != m_lDirectives.cend())
			return (this->*s_sDirectives[i].fExecute)(strLine.substr(pos).c_str(), ulFlags);

		warnings.push_back("Unsupported directive '" + strName + "' found!");
		return true;
	}

	warnings.push_back("Unknown directive '" + strName + "' found!");
	return true;
}

bool ECConfigImpl::HandleInclude(const char *lpszArgs, unsigned int ulFlags)
{
	string strValue;
	std::string file;
	
	file = (strValue = trim(lpszArgs, " \t\r\n"));
	if (file[0] != PATH_SEPARATOR) {
		// Rebuild the path. m_currentFile is always a normalized path.
		auto pos = m_currentFile.find_last_of(PATH_SEPARATOR);
		file = (pos != std::string::npos) ? m_currentFile.substr(0, pos) : ".";
		file += PATH_SEPARATOR;
		file += strValue;
	}
	
	return ReadConfigFile(file, ulFlags);
}

bool ECConfigImpl::HandlePropMap(const char *lpszArgs, unsigned int ulFlags)
{
	string	strValue;
	bool	bResult;

	strValue = trim(lpszArgs, " \t\r\n");
	bResult = ReadConfigFile(strValue.c_str(), LOADSETTING_UNKNOWN | LOADSETTING_OVERWRITE_GROUP, CONFIGGROUP_PROPMAP);

	return bResult;
}

bool ECConfigImpl::CopyConfigSetting(const configsetting_t *lpsSetting, settingkey_t *lpsKey)
{
	if (lpsSetting->szName == NULL || lpsSetting->szValue == NULL)
		return false;

	memset(lpsKey, 0, sizeof(*lpsKey));
	kc_strlcpy(lpsKey->s, lpsSetting->szName, sizeof(lpsKey->s));
	lpsKey->ulFlags = lpsSetting->ulFlags;
	lpsKey->ulGroup = lpsSetting->ulGroup;

	return true;
}

bool ECConfigImpl::CopyConfigSetting(const settingkey_t *lpsKey, const char *szValue, configsetting_t *lpsSetting)
{
	if (strlen(lpsKey->s) == 0 || szValue == NULL)
		return false;

	lpsSetting->szName = lpsKey->s;
	lpsSetting->szValue = szValue;
	lpsSetting->ulFlags = lpsKey->ulFlags;
	lpsSetting->ulGroup = lpsKey->ulGroup;

	return true;
}

bool ECConfigImpl::AddSetting(const configsetting_t *lpsConfig, unsigned int ulFlags)
{
	settingmap_t::const_iterator iterSettings;
	settingkey_t s;
	char *valid = NULL;
	const char *szAlias = NULL;

	if (!CopyConfigSetting(lpsConfig, &s))
		return false;

	// Lookup name as alias
	szAlias = GetAlias(lpsConfig->szName);
	if (szAlias) {
		if (!(ulFlags & LOADSETTING_INITIALIZING))
			warnings.push_back("Option '" + string(lpsConfig->szName) + "' is deprecated! New name for option is '" + szAlias + "'.");
		kc_strlcpy(s.s, szAlias, sizeof(s.s));
	}

	std::lock_guard<KC::shared_mutex> lset(m_settingsRWLock);
	iterSettings = m_mapSettings.find(s);

	if (iterSettings == m_mapSettings.cend()) {
		// new items from file are illegal, add error
		if (!(ulFlags & LOADSETTING_UNKNOWN)) {
			errors.push_back("Unknown option '" + string(lpsConfig->szName) + "' found!");
			return true;
		}
	} else {
		// Check for permissions before overwriting
		if (ulFlags & LOADSETTING_OVERWRITE_GROUP) {
			if (iterSettings->first.ulGroup != lpsConfig->ulGroup) {
				errors.push_back("option '" + string(lpsConfig->szName) + "' cannot be overridden (different group)!");
				return false;
			}
		} else if (ulFlags & LOADSETTING_OVERWRITE_RELOAD) {
			if (!(iterSettings->first.ulFlags & CONFIGSETTING_RELOADABLE))
				return false;
		} else if (!(ulFlags & LOADSETTING_OVERWRITE)) {
			errors.push_back("option '" + string(lpsConfig->szName) + "' cannot be overridden!");
			return false;
		}

		if (!(ulFlags & LOADSETTING_INITIALIZING) &&
		    (iterSettings->first.ulFlags & CONFIGSETTING_UNUSED))
			warnings.push_back("Option '" + string(lpsConfig->szName) + "' is not used anymore.");

		s.ulFlags = iterSettings->first.ulFlags;

		// If this is a commandline parameter, mark the setting as non-reloadable since you do not want to
		// change the value after a HUP
		if (ulFlags & LOADSETTING_CMDLINE_PARAM)
			s.ulFlags &= ~ CONFIGSETTING_RELOADABLE;

	}

	if (lpsConfig->szValue[0] == '$' && (s.ulFlags & CONFIGSETTING_EXACT) == 0) {
		const char *szValue = getenv(lpsConfig->szValue + 1);
		if (szValue == NULL) {
			warnings.push_back("'" + string(lpsConfig->szValue + 1) + "' not found in environment, using '" + lpsConfig->szValue + "' for options '" + lpsConfig->szName + "'.");
			szValue = lpsConfig->szValue;
		}

		if (s.ulFlags & CONFIGSETTING_SIZE) {
			strtoul(szValue, &valid, 10);
			if (valid == szValue) {
				errors.push_back("Option '" + string(lpsConfig->szName) + "' must be a size value (number + optional k/m/g multiplier).");
				return false;
			}
		}

		InsertOrReplace(&m_mapSettings, s, szValue, lpsConfig->ulFlags & CONFIGSETTING_SIZE);
		return true;
	}
	if (s.ulFlags & CONFIGSETTING_SIZE) {
		strtoul(lpsConfig->szValue, &valid, 10);
		if (valid == lpsConfig->szValue) {
			errors.push_back("Option '" + string(lpsConfig->szName) + "' must be a size value (number + optional k/m/g multiplier).");
			return false;
		}
	}
	InsertOrReplace(&m_mapSettings, s, lpsConfig->szValue, s.ulFlags & CONFIGSETTING_SIZE);
	return true;
}

void ECConfigImpl::AddAlias(const configsetting_t *lpsAlias)
{
	settingkey_t s;

	if (!CopyConfigSetting(lpsAlias, &s))
		return;

	std::lock_guard<KC::shared_mutex> lset(m_settingsRWLock);
	InsertOrReplace(&m_mapAliases, s, lpsAlias->szValue, false);
}

bool ECConfigImpl::HasWarnings() {
	return !warnings.empty();
}

bool ECConfigImpl::HasErrors() {
	/* First validate the configuration settings */
	KC::shared_lock<KC::shared_mutex> lset(m_settingsRWLock);

	for (const auto &s : m_mapSettings)
		if (s.first.ulFlags & CONFIGSETTING_NONEMPTY)
			if (!s.second || strlen(s.second) == 0)
				errors.push_back("Option '" + string(s.first.s) + "' cannot be empty!");
	return !errors.empty();
}

} /* namespace */
