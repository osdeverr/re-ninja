// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//
// NOTICE: Modified by osdever (Nikita Ivanov) at 12/04/2022 - porting to the Re build system
//

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <ninja/windows/getopt.h>
#include <direct.h>
#include <windows.h>
#elif defined(_AIX)
#include <ninja/getopt.h>
#include <unistd.h>
#else
#include <getopt.h>
#include <unistd.h>
#endif

#ifdef NINJA_HAVE_BROWSE
#include <ninja/browse.h>
#endif

#include <ninja/build.h>
#include <ninja/build_log.h>
#include <ninja/deps_log.h>
#include <ninja/clean.h>
#include <ninja/debug_flags.h>
#include <ninja/depfile_parser.h>
#include <ninja/disk_interface.h>
#include <ninja/graph.h>
#include <ninja/graphviz.h>
#include <ninja/json.h>
#include <ninja/manifest_parser.h>
#include <ninja/metrics.h>
#include <ninja/missing_deps.h>
#include <ninja/state.h>
#include <ninja/status.h>
#include <ninja/util.h>
#include <ninja/version.h>

#include <ninja/tool_main.h>

using namespace std;
using namespace ninja;

#ifdef _WIN32
// Defined in msvc_helper_main-win32.cc.
int MSVCHelperMain(int argc, char **argv);

// Defined in minidump-win32.cc.
void CreateWin32MiniDump(_EXCEPTION_POINTERS *pep);
#endif

namespace
{

#ifdef _MSC_VER

	/// This handler processes fatal crashes that you can't catch
	/// Test example: C++ exception in a stack-unwind-block
	/// Real-world example: ninja launched a compiler to process a tricky
	/// C++ input file. The compiler got itself into a state where it
	/// generated 3 GB of output and caused ninja to crash.
	void TerminateHandler()
	{
		CreateWin32MiniDump(NULL);
		Fatal("terminate handler called");
	}

	/// On Windows, we want to prevent error dialogs in case of exceptions.
	/// This function handles the exception, and writes a minidump.
	int ExceptionFilter(unsigned int code, struct _EXCEPTION_POINTERS *ep)
	{
		Error("exception: 0x%X", code); // e.g. EXCEPTION_ACCESS_VIOLATION
		fflush(stderr);
		CreateWin32MiniDump(ep);
		return EXCEPTION_EXECUTE_HANDLER;
	}

#endif // _MSC_VER

	/// Print usage information.
	void Usage(const BuildConfig &config)
	{
		fprintf(stderr,
				"usage: ninja [options] [targets...]\n"
				"\n"
				"if targets are unspecified, builds the 'default' target (see manual).\n"
				"\n"
				"options:\n"
				"  --version      print ninja version (\"%s\")\n"
				"  -v, --verbose  show all command lines while building\n"
				"  --quiet        don't show progress status, just command output\n"
				"\n"
				"  -C DIR   change to DIR before doing anything else\n"
				"  -f FILE  specify input build file [default=build.ninja]\n"
				"\n"
				"  -j N     run N jobs in parallel (0 means infinity) [default=%d on this system]\n"
				"  -k N     keep going until N jobs fail (0 means infinity) [default=1]\n"
				"  -l N     do not start new jobs if the load average is greater than N\n"
				"  -n       dry run (don't run commands but act like they succeeded)\n"
				"\n"
				"  -d MODE  enable debugging (use '-d list' to list modes)\n"
				"  -t TOOL  run a subtool (use '-t list' to list subtools)\n"
				"    terminates toplevel options; further flags are passed to the tool\n"
				"  -w FLAG  adjust warnings (use '-w list' to list warnings)\n",
				kNinjaVersion, config.parallelism);
	}

	/// Choose a default value for the -j (parallelism) flag.
	int GuessParallelism()
	{
		switch (int processors = GetProcessorCount())
		{
		case 0:
		case 1:
			return 2;
		case 2:
			return 3;
		default:
			return processors + 2;
		}
	}

	class DeferGuessParallelism
	{
	public:
		bool needGuess;
		BuildConfig *config;

		DeferGuessParallelism(BuildConfig *config)
			: needGuess(true), config(config) {}

		void Refresh()
		{
			if (needGuess)
			{
				needGuess = false;
				config->parallelism = GuessParallelism();
			}
		}
		~DeferGuessParallelism() { Refresh(); }
	};

	/// Enable a debugging mode.  Returns false if Ninja should exit instead
	/// of continuing.
	bool DebugEnable(const string &name)
	{
		if (name == "list")
		{
			printf("debugging modes:\n"
				   "  stats        print operation counts/timing info\n"
				   "  explain      explain what caused a command to execute\n"
				   "  keepdepfile  don't delete depfiles after they're read by ninja\n"
				   "  keeprsp      don't delete @response files on success\n"
#ifdef _WIN32
				   "  nostatcache  don't batch stat() calls per directory and cache them\n"
#endif
				   "multiple modes can be enabled via -d FOO -d BAR\n");
			return false;
		}
		else if (name == "stats")
		{
			g_metrics = new Metrics;
			return true;
		}
		else if (name == "explain")
		{
			g_explaining = true;
			return true;
		}
		else if (name == "keepdepfile")
		{
			g_keep_depfile = true;
			return true;
		}
		else if (name == "keeprsp")
		{
			g_keep_rsp = true;
			return true;
		}
		else if (name == "nostatcache")
		{
			g_experimental_statcache = false;
			return true;
		}
		else
		{
			const char *suggestion =
				SpellcheckString(name.c_str(),
								 "stats", "explain", "keepdepfile", "keeprsp",
								 "nostatcache", NULL);
			if (suggestion)
			{
				Error("unknown debug setting '%s', did you mean '%s'?",
					  name.c_str(), suggestion);
			}
			else
			{
				Error("unknown debug setting '%s'", name.c_str());
			}
			return false;
		}
	}

	/// Set a warning flag.  Returns false if Ninja should exit instead of
	/// continuing.
	bool WarningEnable(const string &name, Options *options)
	{
		if (name == "list")
		{
			printf("warning flags:\n"
				   "  phonycycle={err,warn}  phony build statement references itself\n");
			return false;
		}
		else if (name == "dupbuild=err")
		{
			options->dupe_edges_should_err = true;
			return true;
		}
		else if (name == "dupbuild=warn")
		{
			options->dupe_edges_should_err = false;
			return true;
		}
		else if (name == "phonycycle=err")
		{
			options->phony_cycle_should_err = true;
			return true;
		}
		else if (name == "phonycycle=warn")
		{
			options->phony_cycle_should_err = false;
			return true;
		}
		else if (name == "depfilemulti=err" ||
				 name == "depfilemulti=warn")
		{
			Warning("deprecated warning 'depfilemulti'");
			return true;
		}
		else
		{
			const char *suggestion =
				SpellcheckString(name.c_str(), "dupbuild=err", "dupbuild=warn",
								 "phonycycle=err", "phonycycle=warn", NULL);
			if (suggestion)
			{
				Error("unknown warning flag '%s', did you mean '%s'?",
					  name.c_str(), suggestion);
			}
			else
			{
				Error("unknown warning flag '%s'", name.c_str());
			}
			return false;
		}
	}

	/// Find the function to execute for \a tool_name and return it via \a func.
	/// Returns a Tool, or NULL if Ninja should exit.
	const Tool *ChooseTool(const string &tool_name)
	{
		static const Tool kTools[] = {
			{"browse", "browse dependency graph in a web browser",
			 Tool::RUN_AFTER_LOAD, &NinjaMain::ToolBrowse},
#ifdef _WIN32
			{"msvc", "build helper for MSVC cl.exe (DEPRECATED)",
			 Tool::RUN_AFTER_FLAGS, &NinjaMain::ToolMSVC},
#endif
			{"clean", "clean built files",
			 Tool::RUN_AFTER_LOAD, &NinjaMain::ToolClean},
			{"commands", "list all commands required to rebuild given targets",
			 Tool::RUN_AFTER_LOAD, &NinjaMain::ToolCommands},
			{"inputs", "list all inputs required to rebuild given targets",
			 Tool::RUN_AFTER_LOAD, &NinjaMain::ToolInputs},
			{"deps", "show dependencies stored in the deps log",
			 Tool::RUN_AFTER_LOGS, &NinjaMain::ToolDeps},
			{"missingdeps", "check deps log dependencies on generated files",
			 Tool::RUN_AFTER_LOGS, &NinjaMain::ToolMissingDeps},
			{"graph", "output graphviz dot file for targets",
			 Tool::RUN_AFTER_LOAD, &NinjaMain::ToolGraph},
			{"query", "show inputs/outputs for a path",
			 Tool::RUN_AFTER_LOGS, &NinjaMain::ToolQuery},
			{"targets", "list targets by their rule or depth in the DAG",
			 Tool::RUN_AFTER_LOAD, &NinjaMain::ToolTargets},
			{"compdb", "dump JSON compilation database to stdout",
			 Tool::RUN_AFTER_LOAD, &NinjaMain::ToolCompilationDatabase},
			{"recompact", "recompacts ninja-internal data structures",
			 Tool::RUN_AFTER_LOAD, &NinjaMain::ToolRecompact},
			{"restat", "restats all outputs in the build log",
			 Tool::RUN_AFTER_FLAGS, &NinjaMain::ToolRestat},
			{"rules", "list all rules",
			 Tool::RUN_AFTER_LOAD, &NinjaMain::ToolRules},
			{"cleandead", "clean built files that are no longer produced by the manifest",
			 Tool::RUN_AFTER_LOGS, &NinjaMain::ToolCleanDead},
			{"urtle", NULL,
			 Tool::RUN_AFTER_FLAGS, &NinjaMain::ToolUrtle},
#ifdef _WIN32
			{"wincodepage", "print the Windows code page used by ninja",
			 Tool::RUN_AFTER_FLAGS, &NinjaMain::ToolWinCodePage},
#endif
			{NULL, NULL, Tool::RUN_AFTER_FLAGS, NULL}};

		if (tool_name == "list")
		{
			printf("ninja subtools:\n");
			for (const Tool *tool = &kTools[0]; tool->name; ++tool)
			{
				if (tool->desc)
					printf("%11s  %s\n", tool->name, tool->desc);
			}
			return NULL;
		}

		for (const Tool *tool = &kTools[0]; tool->name; ++tool)
		{
			if (tool->name == tool_name)
				return tool;
		}

		vector<const char *> words;
		for (const Tool *tool = &kTools[0]; tool->name; ++tool)
			words.push_back(tool->name);
		const char *suggestion = SpellcheckStringV(tool_name, words);
		if (suggestion)
		{
			Fatal("unknown tool '%s', did you mean '%s'?",
				  tool_name.c_str(), suggestion);
		}
		else
		{
			Fatal("unknown tool '%s'", tool_name.c_str());
		}
		return NULL; // Not reached.
	}

	/// Parse argv for command-line options.
	/// Returns an exit code, or -1 if Ninja should continue.
	int ReadFlags(int *argc, char ***argv,
				  Options *options, BuildConfig *config)
	{
		DeferGuessParallelism deferGuessParallelism(config);

		enum
		{
			OPT_VERSION = 1,
			OPT_QUIET = 2
		};
		const option kLongOptions[] = {
			{"help", no_argument, NULL, 'h'},
			{"version", no_argument, NULL, OPT_VERSION},
			{"verbose", no_argument, NULL, 'v'},
			{"quiet", no_argument, NULL, OPT_QUIET},
			{NULL, 0, NULL, 0}};

		int opt;
		while (!options->tool &&
			   (opt = getopt_long(*argc, *argv, "d:f:j:k:l:nt:vw:C:h", kLongOptions,
								  NULL)) != -1)
		{
			switch (opt)
			{
			case 'd':
				if (!DebugEnable(optarg))
					return 1;
				break;
			case 'f':
				options->input_file = optarg;
				break;
			case 'j':
			{
				char *end;
				int value = strtol(optarg, &end, 10);
				if (*end != 0 || value < 0)
					Fatal("invalid -j parameter");

				// We want to run N jobs in parallel. For N = 0, INT_MAX
				// is close enough to infinite for most sane builds.
				config->parallelism = value > 0 ? value : INT_MAX;
				deferGuessParallelism.needGuess = false;
				break;
			}
			case 'k':
			{
				char *end;
				int value = strtol(optarg, &end, 10);
				if (*end != 0)
					Fatal("-k parameter not numeric; did you mean -k 0?");

				// We want to go until N jobs fail, which means we should allow
				// N failures and then stop.  For N <= 0, INT_MAX is close enough
				// to infinite for most sane builds.
				config->failures_allowed = value > 0 ? value : INT_MAX;
				break;
			}
			case 'l':
			{
				char *end;
				double value = strtod(optarg, &end);
				if (end == optarg)
					Fatal("-l parameter not numeric: did you mean -l 0.0?");
				config->max_load_average = value;
				break;
			}
			case 'n':
				config->dry_run = true;
				break;
			case 't':
				options->tool = ChooseTool(optarg);
				if (!options->tool)
					return 0;
				break;
			case 'v':
				config->verbosity = BuildConfig::VERBOSE;
				break;
			case OPT_QUIET:
				config->verbosity = BuildConfig::NO_STATUS_UPDATE;
				break;
			case 'w':
				if (!WarningEnable(optarg, options))
					return 1;
				break;
			case 'C':
				options->working_dir = optarg;
				break;
			case OPT_VERSION:
				printf("%s\n", kNinjaVersion);
				return 0;
			case 'h':
			default:
				deferGuessParallelism.Refresh();
				Usage(*config);
				return 1;
			}
		}
		*argv += optind;
		*argc -= optind;

		return -1;
	}

	NORETURN void real_main(int argc, char **argv)
	{
		// Use exit() instead of return in this function to avoid potentially
		// expensive cleanup when destructing NinjaMain.
		BuildConfig config;
		Options options = {};
		options.input_file = "build.ninja";
		options.dupe_edges_should_err = true;

		setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
		const char *ninja_command = argv[0];

		int exit_code = ReadFlags(&argc, &argv, &options, &config);
		if (exit_code >= 0)
			exit(exit_code);

		Status *status = new StatusPrinter(config);

		if (options.working_dir)
		{
			// The formatting of this string, complete with funny quotes, is
			// so Emacs can properly identify that the cwd has changed for
			// subsequent commands.
			// Don't print this if a tool is being used, so that tool output
			// can be piped into a file without this string showing up.
			if (!options.tool && config.verbosity != BuildConfig::NO_STATUS_UPDATE)
				status->Info("Entering directory `%s'", options.working_dir);
			if (chdir(options.working_dir) < 0)
			{
				Fatal("chdir to '%s' - %s", options.working_dir, strerror(errno));
			}
		}

		if (options.tool && options.tool->when == Tool::RUN_AFTER_FLAGS)
		{
			// None of the RUN_AFTER_FLAGS actually use a NinjaMain, but it's needed
			// by other tools.
			NinjaMain ninja(ninja_command, config);
			exit((ninja.*options.tool->func)(&options, argc, argv));
		}

		// Limit number of rebuilds, to prevent infinite loops.
		const int kCycleLimit = 100;
		for (int cycle = 1; cycle <= kCycleLimit; ++cycle)
		{
			NinjaMain ninja(ninja_command, config);

			ManifestParserOptions parser_opts;
			if (options.dupe_edges_should_err)
			{
				parser_opts.dupe_edge_action_ = kDupeEdgeActionError;
			}
			if (options.phony_cycle_should_err)
			{
				parser_opts.phony_cycle_action_ = kPhonyCycleActionError;
			}
			ManifestParser parser(&ninja.state_, &ninja.disk_interface_, parser_opts);
			string err;
			if (!parser.Load(options.input_file, &err))
			{
				status->Error("%s", err.c_str());
				exit(1);
			}

			if (options.tool && options.tool->when == Tool::RUN_AFTER_LOAD)
				exit((ninja.*options.tool->func)(&options, argc, argv));

			if (!ninja.EnsureBuildDirExists())
				exit(1);

			if (!ninja.OpenBuildLog() || !ninja.OpenDepsLog())
				exit(1);

			if (options.tool && options.tool->when == Tool::RUN_AFTER_LOGS)
				exit((ninja.*options.tool->func)(&options, argc, argv));

			// Attempt to rebuild the manifest before building anything else
			if (ninja.RebuildManifest(options.input_file, &err, status))
			{
				// In dry_run mode the regeneration will succeed without changing the
				// manifest forever. Better to return immediately.
				if (config.dry_run)
					exit(0);
				// Start the build over with the new manifest.
				continue;
			}
			else if (!err.empty())
			{
				status->Error("rebuilding '%s': %s", options.input_file, err.c_str());
				exit(1);
			}

			int result = ninja.RunBuild(argc, argv, status);
			if (g_metrics)
				ninja.DumpMetrics();
			exit(result);
		}

		status->Error("manifest '%s' still dirty after %d tries, perhaps system time is not set",
					  options.input_file, kCycleLimit);
		exit(1);
	}

} // anonymous namespace

int main(int argc, char **argv)
{
#if defined(_MSC_VER)
	// Set a handler to catch crashes not caught by the __try..__except
	// block (e.g. an exception in a stack-unwind-block).
	std::set_terminate(TerminateHandler);
	__try
	{
		// Running inside __try ... __except suppresses any Windows error
		// dialogs for errors such as bad_alloc.
		real_main(argc, argv);
	}
	__except (ExceptionFilter(GetExceptionCode(), GetExceptionInformation()))
	{
		// Common error situations return exitCode=1. 2 was chosen to
		// indicate a more serious problem.
		return 2;
	}
#else
	real_main(argc, argv);
#endif
}
