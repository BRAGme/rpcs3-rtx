#include "stdafx.h"
#include "RemixGameConfig.h"

#include "Emu/System.h"
#include "Utilities/File.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace remix_rsx
{
	namespace
	{
		// Only this prefix is accepted. The file sits next to the executable where a user can edit
		// it, and pushing arbitrary names into the process environment from a text file found on
		// disk is a wider door than this needs. Anything else is counted and named in the summary
		// rather than silently dropped, so a typo in a key is visible instead of just inert.
		constexpr const char* k_prefix = "RPCS3_REMIX_";

		std::string trim(std::string_view s)
		{
			usz b = 0;
			usz e = s.size();

			while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
			while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;

			return std::string(s.substr(b, e - b));
		}

#ifdef _WIN32
		std::wstring widen(std::string_view s)
		{
			if (s.empty())
			{
				return {};
			}

			const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);

			if (need <= 0)
			{
				return {};
			}

			std::wstring out(static_cast<usz>(need), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need);
			return out;
		}

		bool env_already_set(const std::wstring& name)
		{
			// A zero return with ERROR_ENVVAR_NOT_FOUND is "absent"; a zero return with a buffer
			// that was too small cannot happen here because the size query form is used. An
			// EXPLICITLY EMPTY variable reads as absent on Win32, which is correct for us: the
			// launcher blanks a handful of keys with set "X=" precisely to mean "unset".
			return ::GetEnvironmentVariableW(name.c_str(), nullptr, 0) != 0;
		}
#endif
	}

	std::string load_game_config()
	{
#ifndef _WIN32
		return "Remix game-config: not supported off Win32";
#else
		const std::string title = Emu.GetTitleID();

		if (title.empty())
		{
			return "Remix game-config: no title id at init -- no per-game config loaded";
		}

		// Sanitised only against path separators. A title id is already [A-Z0-9]-shaped, but this
		// value reaches a filesystem path and the cost of being sure is one pass.
		std::string safe = title;
		std::replace(safe.begin(), safe.end(), '/', '_');
		std::replace(safe.begin(), safe.end(), '\\', '_');
		safe.erase(std::remove(safe.begin(), safe.end(), ':'), safe.end());

		const std::string path = fs::get_executable_dir() + safe + ".conf";

		fs::file f{path};

		if (!f)
		{
			return fmt::format("Remix game-config: none for %s (looked for %s)", title, path);
		}

		const std::string text = f.to_string();

		u32 applied = 0;
		u32 skipped_set = 0;
		u32 ignored = 0;
		std::vector<std::string> ignored_names;

		usz pos = 0;

		while (pos <= text.size())
		{
			const usz nl = text.find('\n', pos);
			const std::string_view raw = std::string_view(text).substr(
				pos, (nl == std::string::npos ? text.size() : nl) - pos);
			pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

			const std::string line = trim(raw);

			if (line.empty() || line[0] == '#' || line[0] == ';')
			{
				continue;
			}

			const usz eq = line.find('=');

			if (eq == std::string::npos)
			{
				continue;
			}

			const std::string key = trim(std::string_view(line).substr(0, eq));
			const std::string value = trim(std::string_view(line).substr(eq + 1));

			if (key.rfind(k_prefix, 0) != 0)
			{
				++ignored;

				if (ignored_names.size() < 4)
				{
					ignored_names.push_back(key);
				}

				continue;
			}

			const std::wstring wkey = widen(key);

			if (wkey.empty())
			{
				++ignored;
				continue;
			}

			// The precedence rule. An environment variable that is already set always wins, so a
			// launcher script -- or an A/B harness -- outranks whatever the shipped profile says,
			// and a sweep cannot be silently overridden by a file the operator forgot about.
			if (env_already_set(wkey))
			{
				++skipped_set;
				continue;
			}

			if (::SetEnvironmentVariableW(wkey.c_str(), widen(value).c_str()))
			{
				++applied;
			}
		}

		std::string summary = fmt::format(
			"Remix game-config: %s applied=%u skipped-already-set=%u ignored=%u (%s)",
			title, applied, skipped_set, ignored, path);

		if (!ignored_names.empty())
		{
			summary += " ignored-keys=";

			for (usz i = 0; i < ignored_names.size(); ++i)
			{
				summary += (i ? "," : "") + ignored_names[i];
			}

			if (ignored > ignored_names.size())
			{
				summary += ",...";
			}
		}

		return summary;
#endif
	}
}
