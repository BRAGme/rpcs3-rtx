#pragma once

#include <string>

namespace remix_rsx
{
	// Loads "<executable dir>/<TITLEID>.conf" into the process environment.
	//
	// MUST run before any RPCS3_REMIX_* knob is read for the first time. Nearly every accessor in
	// this backend is of the form
	//
	//     static const u32 value = env_u32(L"RPCS3_REMIX_X", 0);
	//
	// which latches on first call and never looks again. That is fine -- it is why this is a
	// load-once step at init rather than a per-frame poll -- but it means a value written to the
	// environment after the first read is silently ignored, with no error and no log line. The
	// sibling PCSX2 fork lost real time to exactly that, with three knobs that "silently ignored
	// every value ever set". Call this at the top of on_init_thread(), before the run-start banner
	// (which reads dozens of knobs) and before the runtime DLL is loaded (which reads
	// RPCS3_REMIX_DLL).
	//
	// A variable that is ALREADY SET in the environment is never overwritten. A launcher script
	// therefore still outranks the config file, which keeps an A/B harness authoritative over
	// whatever a shipped profile happens to say -- the same precedence rule the PCSX2 fork uses
	// for its per-game .conf files.
	//
	// Returns a one-line summary for dump_line(). It always returns a line, including when there
	// is no config file, because "no per-game config was loaded" is the answer to the first
	// question anyone asks when a setting appears to do nothing.
	std::string load_game_config();
}
