/*
 * Copyright (C) 2003-2011 The Music Player Daemon Project
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
#include "SimpleDatabasePlugin.hxx"
#include "DatabaseSelection.hxx"

extern "C" {
#include "db_error.h"
#include "db_save.h"
#include "db_lock.h"
#include "conf.h"
#include "locate.h"
}

#include "directory.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

G_GNUC_CONST
static inline GQuark
simple_db_quark(void)
{
	return g_quark_from_static_string("simple_db");
}

Database *
SimpleDatabase::Create(const struct config_param *param, GError **error_r)
{
	SimpleDatabase *db = new SimpleDatabase();
	if (!db->Configure(param, error_r)) {
		delete db;
		db = NULL;
	}

	return db;
}

bool
SimpleDatabase::Configure(const struct config_param *param, GError **error_r)
{
	GError *error = NULL;

	char *_path = config_dup_block_path(param, "path", &error);
	if (_path == NULL) {
		if (error != NULL)
			g_propagate_error(error_r, error);
		else
			g_set_error(error_r, simple_db_quark(), 0,
				    "No \"path\" parameter specified");
		return false;
	}

	path = _path;
	free(_path);

	return true;
}

bool
SimpleDatabase::Check(GError **error_r) const
{
	assert(!path.empty());

	/* Check if the file exists */
	if (access(path.c_str(), F_OK)) {
		/* If the file doesn't exist, we can't check if we can write
		 * it, so we are going to try to get the directory path, and
		 * see if we can write a file in that */
		char *dirPath = g_path_get_dirname(path.c_str());

		/* Check that the parent part of the path is a directory */
		struct stat st;
		if (stat(dirPath, &st) < 0) {
			g_free(dirPath);
			g_set_error(error_r, simple_db_quark(), errno,
				    "Couldn't stat parent directory of db file "
				    "\"%s\": %s",
				    path.c_str(), g_strerror(errno));
			return false;
		}

		if (!S_ISDIR(st.st_mode)) {
			g_free(dirPath);
			g_set_error(error_r, simple_db_quark(), 0,
				    "Couldn't create db file \"%s\" because the "
				    "parent path is not a directory",
				    path.c_str());
			return false;
		}

		/* Check if we can write to the directory */
		if (access(dirPath, X_OK | W_OK)) {
			g_set_error(error_r, simple_db_quark(), errno,
				    "Can't create db file in \"%s\": %s",
				    dirPath, g_strerror(errno));
			g_free(dirPath);
			return false;
		}

		g_free(dirPath);

		return true;
	}

	/* Path exists, now check if it's a regular file */
	struct stat st;
	if (stat(path.c_str(), &st) < 0) {
		g_set_error(error_r, simple_db_quark(), errno,
			    "Couldn't stat db file \"%s\": %s",
			    path.c_str(), g_strerror(errno));
		return false;
	}

	if (!S_ISREG(st.st_mode)) {
		g_set_error(error_r, simple_db_quark(), 0,
			    "db file \"%s\" is not a regular file",
			    path.c_str());
		return false;
	}

	/* And check that we can write to it */
	if (access(path.c_str(), R_OK | W_OK)) {
		g_set_error(error_r, simple_db_quark(), errno,
			    "Can't open db file \"%s\" for reading/writing: %s",
			    path.c_str(), g_strerror(errno));
		return false;
	}

	return true;
}

bool
SimpleDatabase::Load(GError **error_r)
{
	assert(!path.empty());
	assert(root != NULL);

	FILE *fp = fopen(path.c_str(), "r");
	if (fp == NULL) {
		g_set_error(error_r, simple_db_quark(), errno,
			    "Failed to open database file \"%s\": %s",
			    path.c_str(), g_strerror(errno));
		return false;
	}

	if (!db_load_internal(fp, root, error_r)) {
		fclose(fp);
		return false;
	}

	fclose(fp);

	struct stat st;
	if (stat(path.c_str(), &st) == 0)
		mtime = st.st_mtime;

	return true;
}

bool
SimpleDatabase::Open(GError **error_r)
{
	root = directory_new_root();
	mtime = 0;

	GError *error = NULL;
	if (!Load(&error)) {
		directory_free(root);

		g_warning("Failed to load database: %s", error->message);
		g_error_free(error);

		if (!Check(error_r))
			return false;

		root = directory_new_root();
	}

	return true;
}

void
SimpleDatabase::Close()
{
	assert(root != NULL);

	directory_free(root);
}

struct song *
SimpleDatabase::GetSong(const char *uri, GError **error_r) const
{
	assert(root != NULL);

	db_lock();
	struct song *song = directory_lookup_song(root, uri);
	db_unlock();
	if (song == NULL)
		g_set_error(error_r, db_quark(), DB_NOT_FOUND,
			    "No such song: %s", uri);

	return song;
}

G_GNUC_PURE
const struct directory *
SimpleDatabase::LookupDirectory(const char *uri) const
{
	assert(root != NULL);
	assert(uri != NULL);

	db_lock();
	struct directory *directory =
		directory_lookup_directory(root, uri);
	db_unlock();
	return directory;
}

bool
SimpleDatabase::Visit(const DatabaseSelection &selection,
		      VisitDirectory visit_directory,
		      VisitSong visit_song,
		      VisitPlaylist visit_playlist,
		      GError **error_r) const
{
	const struct directory *directory = LookupDirectory(selection.uri);
	if (directory == NULL) {
		struct song *song;
		if (visit_song &&
		    (song = GetSong(selection.uri, NULL)) != NULL &&
		    (selection.match == NULL ||
		     locate_list_song_match(song, selection.match)))
			return visit_song(*song, error_r);

		g_set_error(error_r, db_quark(), DB_NOT_FOUND,
			    "No such directory");
		return false;
	}

	if (selection.recursive && visit_directory &&
	    !visit_directory(*directory, error_r))
		return false;

	db_lock();
	bool ret = directory->Walk(selection.recursive, selection.match,
				   visit_directory, visit_song, visit_playlist,
				   error_r);
	db_unlock();
	return ret;
}

bool
SimpleDatabase::Save(GError **error_r)
{
	db_lock();

	g_debug("removing empty directories from DB");
	directory_prune_empty(root);

	g_debug("sorting DB");
	directory_sort(root);

	db_unlock();

	g_debug("writing DB");

	FILE *fp = fopen(path.c_str(), "w");
	if (!fp) {
		g_set_error(error_r, simple_db_quark(), errno,
			    "unable to write to db file \"%s\": %s",
			    path.c_str(), g_strerror(errno));
		return false;
	}

	db_save_internal(fp, root);

	if (ferror(fp)) {
		g_set_error(error_r, simple_db_quark(), errno,
			    "Failed to write to database file: %s",
			    g_strerror(errno));
		fclose(fp);
		return false;
	}

	fclose(fp);

	struct stat st;
	if (stat(path.c_str(), &st) == 0)
		mtime = st.st_mtime;

	return true;
}

const DatabasePlugin simple_db_plugin = {
	"simple",
	SimpleDatabase::Create,
};
