/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "Configured.hxx"
#include "Registry.hxx"
#include "plugins/LocalStorage.hxx"
#include "config/ConfigGlobal.hxx"
#include "config/ConfigError.hxx"
#include "fs/StandardDirectory.hxx"
#include "fs/CheckFile.hxx"
#include "util/UriUtil.hxx"
#include "util/Error.hxx"

#include <assert.h>

static Storage *
CreateConfiguredStorageUri(const char *uri, Error &error)
{
	Storage *storage = CreateStorageURI(uri, error);
	if (storage == nullptr && !error.IsDefined())
		error.Format(config_domain,
			     "Unrecognized storage URI: %s", uri);
	return storage;
}

static AllocatedPath
GetConfiguredMusicDirectory(Error &error)
{
	AllocatedPath path = config_get_path(CONF_MUSIC_DIR, error);
	if (path.IsNull() && !error.IsDefined())
		path = GetUserMusicDir();

	return path;
}

static Storage *
CreateConfiguredStorageLocal(Error &error)
{
	AllocatedPath path = GetConfiguredMusicDirectory(error);
	if (path.IsNull())
		return nullptr;

	path.ChopSeparators();
	CheckDirectoryReadable(path);
	return CreateLocalStorage(path);
}

Storage *
CreateConfiguredStorage(Error &error)
{
	assert(!error.IsDefined());

	auto uri = config_get_string(CONF_MUSIC_DIR, nullptr);
	if (uri == nullptr)
		return nullptr;

	if (uri_has_scheme(uri))
		return CreateConfiguredStorageUri(uri, error);

	return CreateConfiguredStorageLocal(error);
}

bool
IsStorageConfigured()
{
	return config_get_string(CONF_MUSIC_DIR, nullptr) != nullptr;
}
