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

#pragma once
#include <ninja/build.h>
#include <ninja/build_log.h>
#include <ninja/deps_log.h>
#include <ninja/status.h>
#include <ninja/state.h>
#include <ninja/disk_interface.h>
#include <ninja/util.h>
#include <ninja/metrics.h>

namespace ninja
{
    struct Tool;

    /// Command-line options.
    struct Options
    {
        /// Build file to load.
        const char *input_file;

        /// Directory to change into before running.
        const char *working_dir;

        /// Tool to run rather than building.
        const Tool *tool;

        /// Whether duplicate rules for one target should warn or print an error.
        bool dupe_edges_should_err;

        /// Whether phony cycles should warn or print an error.
        bool phony_cycle_should_err;
    };

    /// The Ninja main() loads up a series of data structures; various tools need
    /// to poke into these, so store them as fields on an object.
    struct NinjaMain : public BuildLogUser
    {
        NinjaMain(const char *ninja_command, const BuildConfig &config) : ninja_command_(ninja_command), config_(config),
                                                                          start_time_millis_(GetTimeMillis()) {}

        /// Command line used to run Ninja.
        const char *ninja_command_;

        /// Build configuration set from flags (e.g. parallelism).
        const BuildConfig &config_;

        /// Loaded state (rules, nodes).
        State state_;

        /// Functions for accessing the disk.
        RealDiskInterface disk_interface_;

        /// The build directory, used for storing the build log etc.
        std::string build_dir_;

        BuildLog build_log_;
        DepsLog deps_log_;

        /// The type of functions that are the entry points to tools (subcommands).
        typedef int (NinjaMain::*ToolFunc)(const Options *, int, char **);

        /// Get the Node for a given command-line path, handling features like
        /// spell correction.
        Node *CollectTarget(const char *cpath, std::string *err);

        /// CollectTarget for all command-line arguments, filling in \a targets.
        bool CollectTargetsFromArgs(int argc, char *argv[],
                                    std::vector<Node *> *targets, std::string *err);

        // The various subcommands, run via "-t XXX".
        int ToolGraph(const Options *options, int argc, char *argv[]);
        int ToolQuery(const Options *options, int argc, char *argv[]);
        int ToolDeps(const Options *options, int argc, char *argv[]);
        int ToolMissingDeps(const Options *options, int argc, char *argv[]);
        int ToolBrowse(const Options *options, int argc, char *argv[]);
        int ToolMSVC(const Options *options, int argc, char *argv[]);
        int ToolTargets(const Options *options, int argc, char *argv[]);
        int ToolCommands(const Options *options, int argc, char *argv[]);
        int ToolInputs(const Options *options, int argc, char *argv[]);
        int ToolClean(const Options *options, int argc, char *argv[]);
        int ToolCleanDead(const Options *options, int argc, char *argv[]);
        int ToolCompilationDatabase(const Options *options, int argc, char *argv[]);
        int ToolRecompact(const Options *options, int argc, char *argv[]);
        int ToolRestat(const Options *options, int argc, char *argv[]);
        int ToolUrtle(const Options *options, int argc, char **argv);
        int ToolRules(const Options *options, int argc, char *argv[]);
        int ToolWinCodePage(const Options *options, int argc, char *argv[]);

        /// Open the build log.
        /// @return false on error.
        bool OpenBuildLog(bool recompact_only = false);

        /// Open the deps log: load it, then open for writing.
        /// @return false on error.
        bool OpenDepsLog(bool recompact_only = false);

        /// Ensure the build directory exists, creating it if necessary.
        /// @return false on error.
        bool EnsureBuildDirExists();

        /// Rebuild the manifest, if necessary.
        /// Fills in \a err on error.
        /// @return true if the manifest was rebuilt.
        bool RebuildManifest(const char *input_file, std::string *err, Status *status);

        /// Build the targets listed on the command line.
        /// @return an exit code.
        int RunBuild(int argc, char **argv, Status *status);

        /// Dump the output requested by '-d stats'.
        void DumpMetrics();

        virtual bool IsPathDead(StringPiece s) const;

        int64_t start_time_millis_;
    };

    /// Subtools, accessible via "-t foo".
    struct Tool
    {
        /// Short name of the tool.
        const char *name;

        /// Description (shown in "-t list").
        const char *desc;

        /// When to run the tool.
        enum
        {
            /// Run after parsing the command-line flags and potentially changing
            /// the current working directory (as early as possible).
            RUN_AFTER_FLAGS,

            /// Run after loading build.ninja.
            RUN_AFTER_LOAD,

            /// Run after loading the build/deps logs.
            RUN_AFTER_LOGS,
        } when;

        /// Implementation of the tool.
        NinjaMain::ToolFunc func;
    };

}
