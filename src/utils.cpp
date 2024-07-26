#include "utils.h"

#include <sys/stat.h>

#include <algorithm>
#include <cassert>
#include <cctype> // ::isspace
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <giomm.h>
#include <glibmm/fileutils.h>
#include <glibmm/uriutils.h>

#include "debug.h"
#include "gettext.h"
#include "ui.h"

using namespace std;
using namespace Glib;

const DB_playItem_t *last;

static const char *home_cache = getenv("XDG_CACHE_HOME");
static const string lyrics_dir = (home_cache ? string(home_cache) : string(getenv("HOME")) + "/.cache")
                               + "/deadbeef/lyrics/";

static experimental::optional<ustring>(*const providers[])(DB_playItem_t *) = {&get_lyrics_from_script};

inline string cached_filename(string artist, string title) {
	replace(artist.begin(), artist.end(), '/', '_');
	replace(title.begin(), title.end(), '/', '_');

	return lyrics_dir + artist + '-' + title;
}

extern "C"
bool is_cached(const char *artist, const char *title) {
	return artist && title && access(cached_filename(artist, title).c_str(), 0) == 0;
}

extern "C"
void ensure_lyrics_path_exists() {
	mkpath(lyrics_dir, 0755);
}

/**
 * Loads the cached lyrics
 * @param artist The artist name
 * @param title  The song title
 * @note         Have no idea about the encodings, so a bug possible here
 */
experimental::optional<ustring> load_cached_lyrics(const char *artist, const char *title) {
	string filename = cached_filename(artist, title);
	debug_out << "filename = '" << filename << "'\n";
	try {
		return {file_get_contents(filename)};
	} catch (const FileError& error) {
		debug_out << error.what();
		return {};
	}
}

bool save_cached_lyrics(const string &artist, const string &title, const string &lyrics) {
	string filename = cached_filename(artist, title);
	ofstream t(filename);
	if (!t) {
		cerr << "lyricbar: could not open file for writing: " << filename << endl;
		return false;
	}
	t << lyrics;
	return true;
}

bool is_playing(DB_playItem_t *track) {
	DB_playItem_t *pl_track = deadbeef->streamer_get_playing_track();
	if (!pl_track)
		return false;

	deadbeef->pl_item_unref(pl_track);
	return pl_track == track;
}

static
experimental::optional<ustring> get_lyrics_from_metadata(DB_playItem_t *track) {
	pl_lock_guard guard;
	const char *lyrics = deadbeef->pl_find_meta(track, "unsynced lyrics")
	                  ?: deadbeef->pl_find_meta(track, "UNSYNCEDLYRICS")
	                  ?: deadbeef->pl_find_meta(track, "lyrics");
	if (lyrics)
		return ustring{lyrics};
	else return {};
}

experimental::optional<ustring> get_lyrics_from_script(DB_playItem_t *track) {
	std::string buf = std::string(4096, '\0');
	deadbeef->conf_get_str("lyricbar.customcmd", nullptr, &buf[0], buf.size());
	if (!buf[0]) {
		return {};
	}
	auto tf_code = deadbeef->tf_compile(buf.data());
	if (!tf_code) {
		std::cerr << "lyricbar: Invalid script command!\n";
		return {};
	}
	ddb_tf_context_t ctx{};
	ctx._size = sizeof(ctx);
	ctx.it = track;

	int command_len = deadbeef->tf_eval(&ctx, tf_code, &buf[0], buf.size());
	deadbeef->tf_free(tf_code);
	if (command_len < 0) {
		std::cerr << "lyricbar: Invalid script command!\n";
		return {};
	}

	buf.resize(command_len);

	std::string script_output;
	int exit_status = 0;
	try {
		spawn_command_line_sync(buf, &script_output, nullptr, &exit_status);
	} catch (const Glib::Error &e) {
		std::cerr << "lyricbar: " << e.what() << "\n";
		return {};
	}

	if (script_output.empty() || exit_status != 0) {
		return {};
	}

	auto res = ustring{std::move(script_output)};
	if (!res.validate()) {
		cerr << "lyricbar: script output is not a valid UTF8 string!\n";
		return {};
	}
	return {std::move(res)};
}

void update_lyrics(void *tr) {
	DB_playItem_t *track = static_cast<DB_playItem_t*>(tr);

	if (auto lyrics = get_lyrics_from_metadata(track)) {
		set_lyrics(track, *lyrics);
		return;
	}

	const char *artist;
	const char *title;
	{
		pl_lock_guard guard;
		artist = deadbeef->pl_find_meta(track, "artist");
		title  = deadbeef->pl_find_meta(track, "title");
	}

	if (artist && title) {
		if (auto lyrics = load_cached_lyrics(artist, title)) {
			set_lyrics(track, *lyrics);
			return;
		}

		set_lyrics(track, _("Loading..."));

		// No lyrics in the tag or cache; try to get some and cache if succeeded
		for (auto f : providers) {
			if (auto lyrics = f(track)) {
				set_lyrics(track, *lyrics);
				save_cached_lyrics(artist, title, *lyrics);
				return;
			}
		}
	}
	set_lyrics(track, _("Lyrics not found"));
}

/**
 * Creates the directory tree.
 * @param name the directory path, including trailing slash
 * @return 0 on success; errno after mkdir call if something went wrong
 */
int mkpath(const string &name, mode_t mode) {
	string dir;
	size_t pos = 0;
	while ((pos = name.find_first_of('/', pos)) != string::npos){
		dir = name.substr(0, pos++);
		if (dir.empty())
			continue; // ignore the leading slash
		if (mkdir(dir.c_str(), mode) && errno != EEXIST)
			return errno;
	}
	return 0;
}

int remove_from_cache_action(DB_plugin_action_t *, ddb_action_context_t ctx) {
	if (ctx == DDB_ACTION_CTX_SELECTION) {
		pl_lock_guard guard;

		ddb_playlist_t *playlist = deadbeef->plt_get_curr();
		if (playlist) {
			DB_playItem_t *current = deadbeef->plt_get_first(playlist, PL_MAIN);
			while (current) {
				if (deadbeef->pl_is_selected (current)) {
					const char *artist = deadbeef->pl_find_meta(current, "artist");
					const char *title  = deadbeef->pl_find_meta(current, "title");
					if (is_cached(artist, title))
						remove(cached_filename(artist, title).c_str());
				}
				DB_playItem_t *next = deadbeef->pl_get_next(current, PL_MAIN);
				deadbeef->pl_item_unref(current);
				current = next;
			}
			deadbeef->plt_unref(playlist);
		}
	}
	return 0;
}
