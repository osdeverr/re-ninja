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

#include "tool_main.h"

#ifdef _WIN32
#include <ninja/getopt.h>
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

// I am way too lazy to rewrite the code to use fully-qualified names
using namespace std;

#ifdef _WIN32
// Defined in msvc_helper_main-win32.cc.
int MSVCHelperMain(int argc, char **argv);

// Defined in minidump-win32.cc.
void CreateWin32MiniDump(_EXCEPTION_POINTERS *pep);
#endif

namespace ninja
{
	bool NinjaMain::IsPathDead(StringPiece s) const
	{
		Node *n = state_.LookupNode(s);
		if (n && n->in_edge())
			return false;
		// Just checking n isn't enough: If an old output is both in the build log
		// and in the deps log, it will have a Node object in state_.  (It will also
		// have an in edge if one of its inputs is another output that's in the deps
		// log, but having a deps edge product an output that's input to another deps
		// edge is rare, and the first recompaction will delete all old outputs from
		// the deps log, and then a second recompaction will clear the build log,
		// which seems good enough for this corner case.)
		// Do keep entries around for files which still exist on disk, for
		// generators that want to use this information.
		std::string err;
		TimeStamp mtime = disk_interface_.Stat(s.AsString(), &err);
		if (mtime == -1)
			Error("%s", err.c_str()); // Log and ignore Stat() errors.
		return mtime == 0;
	}

	/// Rebuild the build manifest, if necessary.
	/// Returns true if the manifest was rebuilt.
	bool NinjaMain::RebuildManifest(const char *input_file, string *err,
									Status *status)
	{
		string path = input_file;
		if (path.empty())
		{
			*err = "empty path";
			return false;
		}
		uint64_t slash_bits; // Unused because this path is only used for lookup.
		CanonicalizePath(&path, &slash_bits);
		Node *node = state_.LookupNode(path);
		if (!node)
			return false;

		Builder builder(&state_, config_, &build_log_, &deps_log_, &disk_interface_,
						status, start_time_millis_);
		if (!builder.AddTarget(node, err))
			return false;

		if (builder.AlreadyUpToDate())
			return false; // Not an error, but we didn't rebuild.

		if (!builder.Build(err))
			return false;

		// The manifest was only rebuilt if it is now dirty (it may have been cleaned
		// by a restat).
		if (!node->dirty())
		{
			// Reset the state to prevent problems like
			// https://github.com/ninja-build/ninja/issues/874
			state_.Reset();
			return false;
		}

		return true;
	}

	Node *NinjaMain::CollectTarget(const char *cpath, string *err)
	{
		string path = cpath;
		if (path.empty())
		{
			*err = "empty path";
			return NULL;
		}
		uint64_t slash_bits;
		CanonicalizePath(&path, &slash_bits);

		// Special syntax: "foo.cc^" means "the first output of foo.cc".
		bool first_dependent = false;
		if (!path.empty() && path[path.size() - 1] == '^')
		{
			path.resize(path.size() - 1);
			first_dependent = true;
		}

		Node *node = state_.LookupNode(path);
		if (node)
		{
			if (first_dependent)
			{
				if (node->out_edges().empty())
				{
					Node *rev_deps = deps_log_.GetFirstReverseDepsNode(node);
					if (!rev_deps)
					{
						*err = "'" + path + "' has no out edge";
						return NULL;
					}
					node = rev_deps;
				}
				else
				{
					Edge *edge = node->out_edges()[0];
					if (edge->outputs_.empty())
					{
						edge->Dump();
						Fatal("edge has no outputs");
					}
					node = edge->outputs_[0];
				}
			}
			return node;
		}
		else
		{
			*err =
				"unknown target '" + Node::PathDecanonicalized(path, slash_bits) + "'";
			if (path == "clean")
			{
				*err += ", did you mean 'ninja -t clean'?";
			}
			else if (path == "help")
			{
				*err += ", did you mean 'ninja -h'?";
			}
			else
			{
				Node *suggestion = state_.SpellcheckNode(path);
				if (suggestion)
				{
					*err += ", did you mean '" + suggestion->path() + "'?";
				}
			}
			return NULL;
		}
	}

	bool NinjaMain::CollectTargetsFromArgs(int argc, char *argv[],
										   vector<Node *> *targets, string *err)
	{
		if (argc == 0)
		{
			*targets = state_.DefaultNodes(err);
			return err->empty();
		}

		for (int i = 0; i < argc; ++i)
		{
			Node *node = CollectTarget(argv[i], err);
			if (node == NULL)
				return false;
			targets->push_back(node);
		}
		return true;
	}

	int NinjaMain::ToolGraph(const Options *options, int argc, char *argv[])
	{
		vector<Node *> nodes;
		string err;
		if (!CollectTargetsFromArgs(argc, argv, &nodes, &err))
		{
			Error("%s", err.c_str());
			return 1;
		}

		GraphViz graph(&state_, &disk_interface_);
		graph.Start();
		for (vector<Node *>::const_iterator n = nodes.begin(); n != nodes.end(); ++n)
			graph.AddTarget(*n);
		graph.Finish();

		return 0;
	}

	int NinjaMain::ToolQuery(const Options *options, int argc, char *argv[])
	{
		if (argc == 0)
		{
			Error("expected a target to query");
			return 1;
		}

		DyndepLoader dyndep_loader(&state_, &disk_interface_);

		for (int i = 0; i < argc; ++i)
		{
			string err;
			Node *node = CollectTarget(argv[i], &err);
			if (!node)
			{
				Error("%s", err.c_str());
				return 1;
			}

			printf("%s:\n", node->path().c_str());
			if (Edge *edge = node->in_edge())
			{
				if (edge->dyndep_ && edge->dyndep_->dyndep_pending())
				{
					if (!dyndep_loader.LoadDyndeps(edge->dyndep_, &err))
					{
						Warning("%s\n", err.c_str());
					}
				}
				printf("  input: %s\n", edge->rule_->name().c_str());
				for (int in = 0; in < (int)edge->inputs_.size(); in++)
				{
					const char *label = "";
					if (edge->is_implicit(in))
						label = "| ";
					else if (edge->is_order_only(in))
						label = "|| ";
					printf("    %s%s\n", label, edge->inputs_[in]->path().c_str());
				}
				if (!edge->validations_.empty())
				{
					printf("  validations:\n");
					for (std::vector<Node *>::iterator validation = edge->validations_.begin();
						 validation != edge->validations_.end(); ++validation)
					{
						printf("    %s\n", (*validation)->path().c_str());
					}
				}
			}
			printf("  outputs:\n");
			for (vector<Edge *>::const_iterator edge = node->out_edges().begin();
				 edge != node->out_edges().end(); ++edge)
			{
				for (vector<Node *>::iterator out = (*edge)->outputs_.begin();
					 out != (*edge)->outputs_.end(); ++out)
				{
					printf("    %s\n", (*out)->path().c_str());
				}
			}
			const std::vector<Edge *> validation_edges = node->validation_out_edges();
			if (!validation_edges.empty())
			{
				printf("  validation for:\n");
				for (std::vector<Edge *>::const_iterator edge = validation_edges.begin();
					 edge != validation_edges.end(); ++edge)
				{
					for (vector<Node *>::iterator out = (*edge)->outputs_.begin();
						 out != (*edge)->outputs_.end(); ++out)
					{
						printf("    %s\n", (*out)->path().c_str());
					}
				}
			}
		}
		return 0;
	}

#if defined(NINJA_HAVE_BROWSE)
	int NinjaMain::ToolBrowse(const Options *options, int argc, char *argv[])
	{
		RunBrowsePython(&state_, ninja_command_, options->input_file, argc, argv);
		// If we get here, the browse failed.
		return 1;
	}
#else
	int NinjaMain::ToolBrowse(const Options *, int, char **)
	{
		Fatal("browse tool not supported on this platform");
		return 1;
	}
#endif

#if defined(_WIN32)
	int NinjaMain::ToolMSVC(const Options *options, int argc, char *argv[])
	{
		// Reset getopt: push one argument onto the front of argv, reset optind.
		argc++;
		argv--;
		optind = 0;
		return MSVCHelperMain(argc, argv);
	}
#endif

	int ToolTargetsList(const vector<Node *> &nodes, int depth, int indent)
	{
		for (vector<Node *>::const_iterator n = nodes.begin();
			 n != nodes.end();
			 ++n)
		{
			for (int i = 0; i < indent; ++i)
				printf("  ");
			const char *target = (*n)->path().c_str();
			if ((*n)->in_edge())
			{
				printf("%s: %s\n", target, (*n)->in_edge()->rule_->name().c_str());
				if (depth > 1 || depth <= 0)
					ToolTargetsList((*n)->in_edge()->inputs_, depth - 1, indent + 1);
			}
			else
			{
				printf("%s\n", target);
			}
		}
		return 0;
	}

	int ToolTargetsSourceList(State *state)
	{
		for (vector<Edge *>::iterator e = state->edges_.begin();
			 e != state->edges_.end(); ++e)
		{
			for (vector<Node *>::iterator inps = (*e)->inputs_.begin();
				 inps != (*e)->inputs_.end(); ++inps)
			{
				if (!(*inps)->in_edge())
					printf("%s\n", (*inps)->path().c_str());
			}
		}
		return 0;
	}

	int ToolTargetsList(State *state, const string &rule_name)
	{
		set<string> rules;

		// Gather the outputs.
		for (vector<Edge *>::iterator e = state->edges_.begin();
			 e != state->edges_.end(); ++e)
		{
			if ((*e)->rule_->name() == rule_name)
			{
				for (vector<Node *>::iterator out_node = (*e)->outputs_.begin();
					 out_node != (*e)->outputs_.end(); ++out_node)
				{
					rules.insert((*out_node)->path());
				}
			}
		}

		// Print them.
		for (set<string>::const_iterator i = rules.begin();
			 i != rules.end(); ++i)
		{
			printf("%s\n", (*i).c_str());
		}

		return 0;
	}

	int ToolTargetsList(State *state)
	{
		for (vector<Edge *>::iterator e = state->edges_.begin();
			 e != state->edges_.end(); ++e)
		{
			for (vector<Node *>::iterator out_node = (*e)->outputs_.begin();
				 out_node != (*e)->outputs_.end(); ++out_node)
			{
				printf("%s: %s\n",
					   (*out_node)->path().c_str(),
					   (*e)->rule_->name().c_str());
			}
		}
		return 0;
	}

	int NinjaMain::ToolDeps(const Options *options, int argc, char **argv)
	{
		vector<Node *> nodes;
		if (argc == 0)
		{
			for (vector<Node *>::const_iterator ni = deps_log_.nodes().begin();
				 ni != deps_log_.nodes().end(); ++ni)
			{
				if (DepsLog::IsDepsEntryLiveFor(*ni))
					nodes.push_back(*ni);
			}
		}
		else
		{
			string err;
			if (!CollectTargetsFromArgs(argc, argv, &nodes, &err))
			{
				Error("%s", err.c_str());
				return 1;
			}
		}

		RealDiskInterface disk_interface;
		for (vector<Node *>::iterator it = nodes.begin(), end = nodes.end();
			 it != end; ++it)
		{
			DepsLog::Deps *deps = deps_log_.GetDeps(*it);
			if (!deps)
			{
				printf("%s: deps not found\n", (*it)->path().c_str());
				continue;
			}

			string err;
			TimeStamp mtime = disk_interface.Stat((*it)->path(), &err);
			if (mtime == -1)
				Error("%s", err.c_str()); // Log and ignore Stat() errors;
			printf("%s: #deps %d, deps mtime %" PRId64 " (%s)\n",
				   (*it)->path().c_str(), deps->node_count, deps->mtime,
				   (!mtime || mtime > deps->mtime ? "STALE" : "VALID"));
			for (int i = 0; i < deps->node_count; ++i)
				printf("    %s\n", deps->nodes[i]->path().c_str());
			printf("\n");
		}

		return 0;
	}

	int NinjaMain::ToolMissingDeps(const Options *options, int argc, char **argv)
	{
		vector<Node *> nodes;
		string err;
		if (!CollectTargetsFromArgs(argc, argv, &nodes, &err))
		{
			Error("%s", err.c_str());
			return 1;
		}
		RealDiskInterface disk_interface;
		MissingDependencyPrinter printer;
		MissingDependencyScanner scanner(&printer, &deps_log_, &state_,
										 &disk_interface);
		for (vector<Node *>::iterator it = nodes.begin(); it != nodes.end(); ++it)
		{
			scanner.ProcessNode(*it);
		}
		scanner.PrintStats();
		if (scanner.HadMissingDeps())
			return 3;
		return 0;
	}

	int NinjaMain::ToolTargets(const Options *options, int argc, char *argv[])
	{
		int depth = 1;
		if (argc >= 1)
		{
			string mode = argv[0];
			if (mode == "rule")
			{
				string rule;
				if (argc > 1)
					rule = argv[1];
				if (rule.empty())
					return ToolTargetsSourceList(&state_);
				else
					return ToolTargetsList(&state_, rule);
			}
			else if (mode == "depth")
			{
				if (argc > 1)
					depth = atoi(argv[1]);
			}
			else if (mode == "all")
			{
				return ToolTargetsList(&state_);
			}
			else
			{
				const char *suggestion =
					SpellcheckString(mode.c_str(), "rule", "depth", "all", NULL);
				if (suggestion)
				{
					Error("unknown target tool mode '%s', did you mean '%s'?",
						  mode.c_str(), suggestion);
				}
				else
				{
					Error("unknown target tool mode '%s'", mode.c_str());
				}
				return 1;
			}
		}

		string err;
		vector<Node *> root_nodes = state_.RootNodes(&err);
		if (err.empty())
		{
			return ToolTargetsList(root_nodes, depth, 0);
		}
		else
		{
			Error("%s", err.c_str());
			return 1;
		}
	}

	int NinjaMain::ToolRules(const Options *options, int argc, char *argv[])
	{
		// Parse options.

		// The rules tool uses getopt, and expects argv[0] to contain the name of
		// the tool, i.e. "rules".
		argc++;
		argv--;

		bool print_description = false;

		optind = 1;
		int opt;
		while ((opt = getopt(argc, argv, const_cast<char *>("hd"))) != -1)
		{
			switch (opt)
			{
			case 'd':
				print_description = true;
				break;
			case 'h':
			default:
				printf("usage: ninja -t rules [options]\n"
					   "\n"
					   "options:\n"
					   "  -d     also print the description of the rule\n"
					   "  -h     print this message\n");
				return 1;
			}
		}
		argv += optind;
		argc -= optind;

		// Print rules

		typedef map<string, const Rule *> Rules;
		const Rules &rules = state_.bindings_.GetRules();
		for (Rules::const_iterator i = rules.begin(); i != rules.end(); ++i)
		{
			printf("%s", i->first.c_str());
			if (print_description)
			{
				const Rule *rule = i->second;
				const EvalString *description = rule->GetBinding("description");
				if (description != NULL)
				{
					printf(": %s", description->Unparse().c_str());
				}
			}
			printf("\n");
		}
		return 0;
	}

#ifdef _WIN32
	int NinjaMain::ToolWinCodePage(const Options *options, int argc, char *argv[])
	{
		if (argc != 0)
		{
			printf("usage: ninja -t wincodepage\n");
			return 1;
		}
		printf("Build file encoding: %s\n", GetACP() == CP_UTF8 ? "UTF-8" : "ANSI");
		return 0;
	}
#endif

	enum PrintCommandMode
	{
		PCM_Single,
		PCM_All
	};
	void PrintCommands(Edge *edge, EdgeSet *seen, PrintCommandMode mode)
	{
		if (!edge)
			return;
		if (!seen->insert(edge).second)
			return;

		if (mode == PCM_All)
		{
			for (vector<Node *>::iterator in = edge->inputs_.begin();
				 in != edge->inputs_.end(); ++in)
				PrintCommands((*in)->in_edge(), seen, mode);
		}

		if (!edge->is_phony())
			puts(edge->EvaluateCommand().c_str());
	}

	int NinjaMain::ToolCommands(const Options *options, int argc, char *argv[])
	{
		// The commands tool uses getopt, and expects argv[0] to contain the name of
		// the tool, i.e. "commands".
		++argc;
		--argv;

		PrintCommandMode mode = PCM_All;

		optind = 1;
		int opt;
		while ((opt = getopt(argc, argv, const_cast<char *>("hs"))) != -1)
		{
			switch (opt)
			{
			case 's':
				mode = PCM_Single;
				break;
			case 'h':
			default:
				printf("usage: ninja -t commands [options] [targets]\n"
					   "\n"
					   "options:\n"
					   "  -s     only print the final command to build [target], not the whole chain\n");
				return 1;
			}
		}
		argv += optind;
		argc -= optind;

		vector<Node *> nodes;
		string err;
		if (!CollectTargetsFromArgs(argc, argv, &nodes, &err))
		{
			Error("%s", err.c_str());
			return 1;
		}

		EdgeSet seen;
		for (vector<Node *>::iterator in = nodes.begin(); in != nodes.end(); ++in)
			PrintCommands((*in)->in_edge(), &seen, mode);

		return 0;
	}

	void CollectInputs(Edge *edge, std::set<Edge *> *seen,
					   std::vector<std::string> *result)
	{
		if (!edge)
			return;
		if (!seen->insert(edge).second)
			return;

		for (vector<Node *>::iterator in = edge->inputs_.begin();
			 in != edge->inputs_.end(); ++in)
			CollectInputs((*in)->in_edge(), seen, result);

		if (!edge->is_phony())
		{
			edge->CollectInputs(true, result);
		}
	}

	int NinjaMain::ToolInputs(const Options *options, int argc, char *argv[])
	{
		// The inputs tool uses getopt, and expects argv[0] to contain the name of
		// the tool, i.e. "inputs".
		argc++;
		argv--;
		optind = 1;
		int opt;
		const option kLongOptions[] = {{"help", no_argument, NULL, 'h'},
									   {NULL, 0, NULL, 0}};
		while ((opt = getopt_long(argc, argv, "h", kLongOptions, NULL)) != -1)
		{
			switch (opt)
			{
			case 'h':
			default:
				// clang-format off
      printf(
"Usage '-t inputs [options] [targets]\n"
"\n"
"List all inputs used for a set of targets. Note that this includes\n"
"explicit, implicit and order-only inputs, but not validation ones.\n\n"
"Options:\n"
"  -h, --help   Print this message.\n");
				// clang-format on
				return 1;
			}
		}
		argv += optind;
		argc -= optind;

		vector<Node *> nodes;
		string err;
		if (!CollectTargetsFromArgs(argc, argv, &nodes, &err))
		{
			Error("%s", err.c_str());
			return 1;
		}

		std::set<Edge *> seen;
		std::vector<std::string> result;
		for (vector<Node *>::iterator in = nodes.begin(); in != nodes.end(); ++in)
			CollectInputs((*in)->in_edge(), &seen, &result);

		// Make output deterministic by sorting then removing duplicates.
		std::sort(result.begin(), result.end());
		result.erase(std::unique(result.begin(), result.end()), result.end());

		for (size_t n = 0; n < result.size(); ++n)
			puts(result[n].c_str());

		return 0;
	}

	int NinjaMain::ToolClean(const Options *options, int argc, char *argv[])
	{
		// The clean tool uses getopt, and expects argv[0] to contain the name of
		// the tool, i.e. "clean".
		argc++;
		argv--;

		bool generator = false;
		bool clean_rules = false;

		optind = 1;
		int opt;
		while ((opt = getopt(argc, argv, const_cast<char *>("hgr"))) != -1)
		{
			switch (opt)
			{
			case 'g':
				generator = true;
				break;
			case 'r':
				clean_rules = true;
				break;
			case 'h':
			default:
				printf("usage: ninja -t clean [options] [targets]\n"
					   "\n"
					   "options:\n"
					   "  -g     also clean files marked as ninja generator output\n"
					   "  -r     interpret targets as a list of rules to clean instead\n");
				return 1;
			}
		}
		argv += optind;
		argc -= optind;

		if (clean_rules && argc == 0)
		{
			Error("expected a rule to clean");
			return 1;
		}

		Cleaner cleaner(&state_, config_, &disk_interface_);
		if (argc >= 1)
		{
			if (clean_rules)
				return cleaner.CleanRules(argc, argv);
			else
				return cleaner.CleanTargets(argc, argv);
		}
		else
		{
			return cleaner.CleanAll(generator);
		}
	}

	int NinjaMain::ToolCleanDead(const Options *options, int argc, char *argv[])
	{
		Cleaner cleaner(&state_, config_, &disk_interface_);
		return cleaner.CleanDead(build_log_.entries());
	}

	enum EvaluateCommandMode
	{
		ECM_NORMAL,
		ECM_EXPAND_RSPFILE
	};
	std::string EvaluateCommandWithRspfile(const Edge *edge,
										   const EvaluateCommandMode mode)
	{
		string command = edge->EvaluateCommand();
		if (mode == ECM_NORMAL)
			return command;

		string rspfile = edge->GetUnescapedRspfile();
		if (rspfile.empty())
			return command;

		size_t index = command.find(rspfile);
		if (index == 0 || index == string::npos || command[index - 1] != '@')
			return command;

		string rspfile_content = edge->GetBinding("rspfile_content");
		size_t newline_index = 0;
		while ((newline_index = rspfile_content.find('\n', newline_index)) !=
			   string::npos)
		{
			rspfile_content.replace(newline_index, 1, 1, ' ');
			++newline_index;
		}
		command.replace(index - 1, rspfile.length() + 1, rspfile_content);
		return command;
	}

	void printCompdb(const char *const directory, const Edge *const edge,
					 const EvaluateCommandMode eval_mode)
	{
		printf("\n  {\n    \"directory\": \"");
		PrintJSONString(directory);
		printf("\",\n    \"command\": \"");
		PrintJSONString(EvaluateCommandWithRspfile(edge, eval_mode));
		printf("\",\n    \"file\": \"");
		PrintJSONString(edge->inputs_[0]->path());
		printf("\",\n    \"output\": \"");
		PrintJSONString(edge->outputs_[0]->path());
		printf("\"\n  }");
	}

	int NinjaMain::ToolCompilationDatabase(const Options *options, int argc,
										   char *argv[])
	{
		// The compdb tool uses getopt, and expects argv[0] to contain the name of
		// the tool, i.e. "compdb".
		argc++;
		argv--;

		EvaluateCommandMode eval_mode = ECM_NORMAL;

		optind = 1;
		int opt;
		while ((opt = getopt(argc, argv, const_cast<char *>("hx"))) != -1)
		{
			switch (opt)
			{
			case 'x':
				eval_mode = ECM_EXPAND_RSPFILE;
				break;

			case 'h':
			default:
				printf(
					"usage: ninja -t compdb [options] [rules]\n"
					"\n"
					"options:\n"
					"  -x     expand @rspfile style response file invocations\n");
				return 1;
			}
		}
		argv += optind;
		argc -= optind;

		bool first = true;
		vector<char> cwd;
		char *success = NULL;

		do
		{
			cwd.resize(cwd.size() + 1024);
			errno = 0;
			success = getcwd(&cwd[0], cwd.size());
		} while (!success && errno == ERANGE);
		if (!success)
		{
			Error("cannot determine working directory: %s", strerror(errno));
			return 1;
		}

		putchar('[');
		for (vector<Edge *>::iterator e = state_.edges_.begin();
			 e != state_.edges_.end(); ++e)
		{
			if ((*e)->inputs_.empty())
				continue;
			if (argc == 0)
			{
				if (!first)
				{
					putchar(',');
				}
				printCompdb(&cwd[0], *e, eval_mode);
				first = false;
			}
			else
			{
				for (int i = 0; i != argc; ++i)
				{
					if ((*e)->rule_->name() == argv[i])
					{
						if (!first)
						{
							putchar(',');
						}
						printCompdb(&cwd[0], *e, eval_mode);
						first = false;
					}
				}
			}
		}

		puts("\n]");
		return 0;
	}

	int NinjaMain::ToolRecompact(const Options *options, int argc, char *argv[])
	{
		if (!EnsureBuildDirExists())
			return 1;

		if (!OpenBuildLog(/*recompact_only=*/true) ||
			!OpenDepsLog(/*recompact_only=*/true))
			return 1;

		return 0;
	}

	int NinjaMain::ToolRestat(const Options *options, int argc, char *argv[])
	{
		// The restat tool uses getopt, and expects argv[0] to contain the name of the
		// tool, i.e. "restat"
		argc++;
		argv--;

		optind = 1;
		int opt;
		while ((opt = getopt(argc, argv, const_cast<char *>("h"))) != -1)
		{
			switch (opt)
			{
			case 'h':
			default:
				printf("usage: ninja -t restat [outputs]\n");
				return 1;
			}
		}
		argv += optind;
		argc -= optind;

		if (!EnsureBuildDirExists())
			return 1;

		string log_path = ".ninja_log";
		if (!build_dir_.empty())
			log_path = build_dir_ + "/" + log_path;

		string err;
		const LoadStatus status = build_log_.Load(log_path, &err);
		if (status == LOAD_ERROR)
		{
			Error("loading build log %s: %s", log_path.c_str(), err.c_str());
			return EXIT_FAILURE;
		}
		if (status == LOAD_NOT_FOUND)
		{
			// Nothing to restat, ignore this
			return EXIT_SUCCESS;
		}
		if (!err.empty())
		{
			// Hack: Load() can return a warning via err by returning LOAD_SUCCESS.
			Warning("%s", err.c_str());
			err.clear();
		}

		bool success = build_log_.Restat(log_path, disk_interface_, argc, argv, &err);
		if (!success)
		{
			Error("failed recompaction: %s", err.c_str());
			return EXIT_FAILURE;
		}

		if (!config_.dry_run)
		{
			if (!build_log_.OpenForWrite(log_path, *this, &err))
			{
				Error("opening build log: %s", err.c_str());
				return EXIT_FAILURE;
			}
		}

		return EXIT_SUCCESS;
	}

	int NinjaMain::ToolUrtle(const Options *options, int argc, char **argv)
	{
		// RLE encoded.
		const char *urtle =
			" 13 ,3;2!2;\n8 ,;<11!;\n5 `'<10!(2`'2!\n11 ,6;, `\\. `\\9 .,c13$ec,.\n6 "
			",2;11!>; `. ,;!2> .e8$2\".2 \"?7$e.\n <:<8!'` 2.3,.2` ,3!' ;,(?7\";2!2'<"
			"; `?6$PF ,;,\n2 `'4!8;<!3'`2 3! ;,`'2`2'3!;4!`2.`!;2 3,2 .<!2'`).\n5 3`5"
			"'2`9 `!2 `4!><3;5! J2$b,`!>;2!:2!`,d?b`!>\n26 `'-;,(<9!> $F3 )3.:!.2 d\""
			"2 ) !>\n30 7`2'<3!- \"=-='5 .2 `2-=\",!>\n25 .ze9$er2 .,cd16$bc.'\n22 .e"
			"14$,26$.\n21 z45$c .\n20 J50$c\n20 14$P\"`?34$b\n20 14$ dbc `2\"?22$?7$c"
			"\n20 ?18$c.6 4\"8?4\" c8$P\n9 .2,.8 \"20$c.3 ._14 J9$\n .2,2c9$bec,.2 `?"
			"21$c.3`4%,3%,3 c8$P\"\n22$c2 2\"?21$bc2,.2` .2,c7$P2\",cb\n23$b bc,.2\"2"
			"?14$2F2\"5?2\",J5$P\" ,zd3$\n24$ ?$3?%3 `2\"2?12$bcucd3$P3\"2 2=7$\n23$P"
			"\" ,3;<5!>2;,. `4\"6?2\"2 ,9;, `\"?2$\n";
		int count = 0;
		for (const char *p = urtle; *p; p++)
		{
			if ('0' <= *p && *p <= '9')
			{
				count = count * 10 + *p - '0';
			}
			else
			{
				for (int i = 0; i < max(count, 1); ++i)
					printf("%c", *p);
				count = 0;
			}
		}
		return 0;
	}

	bool NinjaMain::OpenBuildLog(bool recompact_only)
	{
		string log_path = ".ninja_log";
		if (!build_dir_.empty())
			log_path = build_dir_ + "/" + log_path;

		string err;
		const LoadStatus status = build_log_.Load(log_path, &err);
		if (status == LOAD_ERROR)
		{
			Error("loading build log %s: %s", log_path.c_str(), err.c_str());
			return false;
		}
		if (!err.empty())
		{
			// Hack: Load() can return a warning via err by returning LOAD_SUCCESS.
			Warning("%s", err.c_str());
			err.clear();
		}

		if (recompact_only)
		{
			if (status == LOAD_NOT_FOUND)
			{
				return true;
			}
			bool success = build_log_.Recompact(log_path, *this, &err);
			if (!success)
				Error("failed recompaction: %s", err.c_str());
			return success;
		}

		if (!config_.dry_run)
		{
			if (!build_log_.OpenForWrite(log_path, *this, &err))
			{
				Error("opening build log: %s", err.c_str());
				return false;
			}
		}

		return true;
	}

	/// Open the deps log: load it, then open for writing.
	/// @return false on error.
	bool NinjaMain::OpenDepsLog(bool recompact_only)
	{
		string path = ".ninja_deps";
		if (!build_dir_.empty())
			path = build_dir_ + "/" + path;

		string err;
		const LoadStatus status = deps_log_.Load(path, &state_, &err);
		if (status == LOAD_ERROR)
		{
			Error("loading deps log %s: %s", path.c_str(), err.c_str());
			return false;
		}
		if (!err.empty())
		{
			// Hack: Load() can return a warning via err by returning LOAD_SUCCESS.
			Warning("%s", err.c_str());
			err.clear();
		}

		if (recompact_only)
		{
			if (status == LOAD_NOT_FOUND)
			{
				return true;
			}
			bool success = deps_log_.Recompact(path, &err);
			if (!success)
				Error("failed recompaction: %s", err.c_str());
			return success;
		}

		if (!config_.dry_run)
		{
			if (!deps_log_.OpenForWrite(path, &err))
			{
				Error("opening deps log: %s", err.c_str());
				return false;
			}
		}

		return true;
	}

	void NinjaMain::DumpMetrics()
	{
		g_metrics->Report();

		printf("\n");
		int count = (int)state_.paths_.size();
		int buckets = (int)state_.paths_.bucket_count();
		printf("path->node hash load %.2f (%d entries / %d buckets)\n",
			   count / (double)buckets, count, buckets);
	}

	bool NinjaMain::EnsureBuildDirExists()
	{
		build_dir_ = state_.bindings_.LookupVariable("builddir");
		if (!build_dir_.empty() && !config_.dry_run)
		{
			if (!disk_interface_.MakeDirs(build_dir_ + "/.") && errno != EEXIST)
			{
				Error("creating build directory %s: %s",
					  build_dir_.c_str(), strerror(errno));
				return false;
			}
		}
		return true;
	}

	int NinjaMain::RunBuild(int argc, char **argv, Status *status)
	{
		string err;
		vector<Node *> targets;
		if (!CollectTargetsFromArgs(argc, argv, &targets, &err))
		{
			status->Error("%s", err.c_str());
			return 1;
		}

		disk_interface_.AllowStatCache(g_experimental_statcache);

		Builder builder(&state_, config_, &build_log_, &deps_log_, &disk_interface_,
						status, start_time_millis_);
		for (size_t i = 0; i < targets.size(); ++i)
		{
			if (!builder.AddTarget(targets[i], &err))
			{
				if (!err.empty())
				{
					status->Error("%s", err.c_str());
					return 1;
				}
				else
				{
					// Added a target that is already up-to-date; not really
					// an error.
				}
			}
		}

		// Make sure restat rules do not see stale timestamps.
		disk_interface_.AllowStatCache(false);

		if (builder.AlreadyUpToDate())
		{
			status->Info("no work to do.");
			return 0;
		}

		if (!builder.Build(&err))
		{
			status->Info("build stopped: %s.", err.c_str());
			if (err.find("interrupted by user") != string::npos)
			{
				return 2;
			}
			return 1;
		}

		return 0;
	}
}
