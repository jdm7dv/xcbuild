/**
 Copyright (c) 2015-present, Facebook, Inc.
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree. An additional grant
 of patent rights can be found in the PATENTS file in the same directory.
 */

#include <pbxbuild/Action/NinjaExecutor.h>
#include <pbxbuild/Phase/Environment.h>
#include <pbxbuild/Phase/PhaseInvocations.h>
#include <ninja/Writer.h>
#include <ninja/Value.h>
#include <libutil/md5.h>

#include <sstream>
#include <iomanip>
#include <fstream>

#include <sys/types.h>
#include <sys/stat.h>

using pbxbuild::Action::NinjaExecutor;
namespace Build = pbxbuild::Build;
namespace Target = pbxbuild::Target;
namespace Tool = pbxbuild::Tool;
using libutil::FSUtil;

NinjaExecutor::
NinjaExecutor(std::shared_ptr<Formatter> const &formatter, bool dryRun) :
    Executor(formatter, dryRun)
{
}

NinjaExecutor::
~NinjaExecutor()
{
}

static std::string
TargetNinjaBegin(pbxproj::PBX::Target::shared_ptr const &target)
{
    return "begin-target-" + target->name();
}

static std::string
TargetNinjaFinish(pbxproj::PBX::Target::shared_ptr const &target)
{
    return "finish-target-" + target->name();
}

static std::string
TargetNinjaPath(pbxproj::PBX::Target::shared_ptr const &target, Target::Environment const &targetEnvironment)
{
    /*
     * Determine where the Ninja file should go. We use the target's temp dir
     * as, being target-specific, it will allow the Ninja files to not conflict.
     */
    pbxsetting::Environment const &environment = targetEnvironment.environment();
    std::string temporaryDirectory = environment.resolve("TARGET_TEMP_DIR");
    // TODO(grp): How to handle varying configurations / actions / other build context options?

    return temporaryDirectory + "/" + "build.ninja";
}

static std::string
NinjaRuleName()
{
    return "invoke";
}

static std::string
NinjaDescription(std::string const &description)
{
    /* Limit to the first line: Ninja can only handle a single line status. */
    std::string::size_type newline = description.find('\n');
    if (newline != std::string::npos) {
        return description.substr(0, description.find('\n'));
    } else {
        return description;
    }
}

static std::string
NinjaPhonyOutputTarget(std::string const &phonyOutput)
{
    /*
     * This is a hack to support multiple rules generating the same output,
     * for when a later invocation wants to modify the output of a previous
     * invocation in-place.
     *
     * Ninja does not support multiple targets with the same output, even
     * when the latter target depends on the former. However, since there is
     * already a target to generate the output, it doesn't particularly
     * matter *what* the latter command outputs: it just has to be something.
     * As long as the "target finish" depends on that fake output, it will
     * be run at the right time.
     *
     * To simulate this, we just need to pick a unique target name here,
     * preferably through a stable algorithm. Unfortunately, since the
     * post-processing invocations could be exactly identical, there's no
     * good value to use as a stable key for the fake output.
     */

    // TODO(grp): Handle identical phony output targets in a build.
    md5_state_t state;
    md5_init(&state);
    md5_append(&state, reinterpret_cast<const md5_byte_t *>(phonyOutput.data()), phonyOutput.size());
    uint8_t digest[16];
    md5_finish(&state, reinterpret_cast<md5_byte_t *>(&digest));

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t c : digest) {
        ss << std::setw(2) << static_cast<int>(c);
    }

    return ".ninja-phony-output-" + ss.str();
}

static bool
WriteNinja(ninja::Writer const &writer, std::string const &path)
{
    std::string contents = writer.serialize();

    if (!FSUtil::CreateDirectory(FSUtil::GetDirectoryName(path))) {
        return false;
    }

    std::ofstream out;
    out.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (out.fail()) {
        return false;
    }

    out.write(contents.data(), contents.size() * sizeof(char));
    out.close();

    return true;
}

bool NinjaExecutor::
build(
    Build::Environment const &buildEnvironment,
    Build::Context const &buildContext,
    DirectedGraph<pbxproj::PBX::Target::shared_ptr> const &targetGraph)
{
    /*
     * This environment contains only settings shared for the entire build.
     */
    pbxsetting::Environment environment = buildEnvironment.baseEnvironment();
    environment.insertFront(buildContext.baseSettings(), false);

    /*
     * Determine where build-level outputs will go. Note we can't use CONFIGURATION_BUILD_DIR
     * at this point because that includes the EFFECTIVE_PLATFORM_NAME, but we don't have a platform.
     */
    std::string intermediatesDirectory = environment.resolve("OBJROOT");
    // TODO(grp): How to handle varying configurations / actions / other build context options?

    /*
     * Write out a Ninja file for the build as a whole. Note each target will have a separate
     * file, this is to coordinate the build between targets.
     */
    ninja::Writer writer;
    writer.comment("xcbuild ninja");
    writer.comment("Action: " + buildContext.action());
    if (buildContext.workspaceContext()->workspace() != nullptr) {
        writer.comment("Workspace: " + buildContext.workspaceContext()->workspace()->projectFile());
    } else if (buildContext.workspaceContext()->project() != nullptr) {
        writer.comment("Project: " + buildContext.workspaceContext()->project()->projectFile());
    }
    if (buildContext.scheme() != nullptr) {
        writer.comment("Scheme: " + buildContext.scheme()->name());
    }
    writer.comment("Configuation: " + buildContext.configuration());
    writer.newline();

    /*
     * Ninja's intermediate outputs should also go in the temp dir.
     */
    writer.binding({ "builddir", { ninja::Value::String(intermediatesDirectory) } });
    writer.newline();

    /*
     * Since invocations are already resolved at this point, we can't use more specific
     * rules at the Ninja level. Instead, add a single rule that just passes through from
     * the build command that calls it.
     */
    writer.rule(NinjaRuleName(), ninja::Value::Expression("cd $dir && $exec"));

    /* Stores seen output directories, since each can only have one target to build them. */
    std::unordered_set<std::string> seenDirectories;

    /*
     * Go over each target and write out Ninja targets for the start and end of each.
     * Don't bother topologically sorting the targets now, since Ninja will do that for us.
     */
    for (pbxproj::PBX::Target::shared_ptr const &target : targetGraph.nodes()) {

        /*
         * Beginning target depends on finishing the targets before that. This is implemented
         * in three parts:
         *
         *  1. Each target has a "target begin" Ninja target depending on completing the build
         *     of any dependent targets.
         *  2. Each invocation's Ninja target depends on the "target begin" target to order
         *     them necessarily after the target started building.
         *  3. Each target also has a "target finish" Ninja target, which depends on all of
         *     the invocations created for the target.
         *
         * The end result is that targets build in the right order. Note this does not preclude
         * cross-target parallelization; if the target dependency graph doesn't have an edge,
         * then they will be parallelized. Linear builds have edges from each target to all
         * previous targets.
         */

        /*
         * As described above, the target's begin depends on all of the target dependencies.
         */
        std::vector<ninja::Value> dependenciesFinished;
        for (pbxproj::PBX::Target::shared_ptr const &dependency : targetGraph.adjacent(target)) {
            std::string targetFinished = TargetNinjaFinish(dependency);
            dependenciesFinished.push_back(ninja::Value::String(targetFinished));
        }

        /*
         * Add the phony target for beginning this target's build.
         */
        std::string targetBegin = TargetNinjaBegin(target);
        writer.build({ ninja::Value::String(targetBegin) }, "phony", dependenciesFinished);

        /*
         * Resolve this target and generate its Ninja file.
         */
        std::unique_ptr<Target::Environment> targetEnvironment = buildContext.targetEnvironment(buildEnvironment, target);
        if (targetEnvironment == nullptr) {
            fprintf(stderr, "error: couldn't create target environment for %s\n", target->name().c_str());
            continue;
        }

        Phase::Environment phaseEnvironment = Phase::Environment(buildEnvironment, buildContext, target, *targetEnvironment);
        Phase::PhaseInvocations phaseInvocations = Phase::PhaseInvocations::Create(phaseEnvironment, target);

        /*
         * Each output directory can only have one rule to build it, so as directories are shared
         * between targets, the rules to build them also need to go into the shared Ninja file.
         */
        if (!buildTargetOutputDirectories(&writer, target, *targetEnvironment, phaseInvocations.invocations(), &seenDirectories)) {
            return false;
        }

        /*
         * Write out the Ninja file to build this target.
         */
        if (!buildTargetInvocations(target, *targetEnvironment, phaseInvocations.invocations())) {
            return false;
        }

        /*
         * Load the Ninja file generated for this target.
         */
        std::string targetPath = TargetNinjaPath(target, *targetEnvironment);
        writer.subninja(targetPath);

        /*
         * As described above, the target's finish depends on all of the invocation outputs.
         */
        std::vector<ninja::Value> invocationOutputs;
        std::vector<ninja::Value> invocationOrderOnlyOutputs;
        for (Tool::Invocation const &invocation : phaseInvocations.invocations()) {
            for (std::string const &output : invocation.outputs()) {
                invocationOutputs.push_back(ninja::Value::String(output));
            }
            for (std::string const &phonyOutput : invocation.phonyOutputs()) {
                std::string phonyOutputTarget = NinjaPhonyOutputTarget(phonyOutput);
                invocationOrderOnlyOutputs.push_back(ninja::Value::String(phonyOutputTarget));
            }
        }

        /*
         * Add the phony target for ending this target's build.
         */
        std::string targetFinish = TargetNinjaFinish(target);
        writer.build({ ninja::Value::String(targetFinish) }, "phony", { }, { }, invocationOutputs, invocationOrderOnlyOutputs);
    }

    /*
     * Serialize the Ninja file into the build root.
     */
    std::string path = intermediatesDirectory + "/" + "build.ninja";
    if (!WriteNinja(writer, path)) {
        return false;
    }

    /*
     * Note where the Ninja file is written.
     */
    fprintf(stderr, "Wrote meta-ninja: %s\n", path.c_str());

    return true;
}

static std::string
ShellEscape(std::string const &value)
{
    const std::string alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const std::string digits = "0123456789";

    if (value.find_first_not_of(alphabet + digits + "@%_-+=:,./") == std::string::npos) {
        return value;
    } else {
        std::string result = value;
        std::string::size_type offset = 0;
        while ((offset = result.find("'", offset)) != std::string::npos) {
            result.replace(offset, 1, "'\"'\"'");
            offset += 5;
        }
        return "'" + result + "'";
    }
}

static std::string
ResolveExecutable(std::string const &executable, std::vector<std::string> const &searchPaths)
{
    std::string builtinPrefix = "builtin-";
    bool builtin = executable.compare(0, builtinPrefix.size(), builtinPrefix) == 0;

    if (builtin) {
        // TODO(grp): Find the path to the builtin tool.
        return std::string();
    } else if (!FSUtil::IsAbsolutePath(executable)) {
        return FSUtil::FindExecutable(executable, searchPaths);
    } else {
        return executable;
    }
}

bool NinjaExecutor::
buildTargetOutputDirectories(
    ninja::Writer *writer,
    pbxproj::PBX::Target::shared_ptr const &target,
    Target::Environment const &targetEnvironment,
    std::vector<Tool::Invocation const> const &invocations,
    std::unordered_set<std::string> *seenDirectories)
{
    std::string targetBegin = TargetNinjaBegin(target);

    /*
     * Add a build command to create each output directory. These are depended on by
     * the build commands for invocations that have outputs inside each directory.
     */
    for (Tool::Invocation const &invocation : invocations) {
        for (std::string const &output : invocation.outputs()) {
            std::string outputDirectory = FSUtil::GetDirectoryName(output);

            /*
             * Only create each directory once. If this directory already has a build
             * target to create it, skip adding another one.
             */
            auto it = seenDirectories->find(outputDirectory);
            if (it != seenDirectories->end()) {
                continue;
            }
            seenDirectories->insert(outputDirectory);

            /*
             * Create the bindings for creating the directory.
             */
            std::string description = NinjaDescription(_formatter->createAuxiliaryDirectory(outputDirectory));
            std::string command = "/bin/mkdir -p " + ShellEscape(outputDirectory);
            std::vector<ninja::Binding> bindings = {
                { "description", ninja::Value::String(description) },
                { "dir", ninja::Value::String(ShellEscape(invocation.workingDirectory())) },
                { "exec", ninja::Value::String(command) },
            };

            /*
             * Add the rule to create the directory. It depends on the target build starting
             * and outputs the directory being created. Note there are no inputs.
             */
            std::vector<ninja::Value> outputs = { ninja::Value::String(outputDirectory) };
            std::vector<ninja::Value> orderDependencies = { ninja::Value::String(targetBegin) };
            writer->build(outputs, NinjaRuleName(), { }, bindings, { }, orderDependencies);
        }
    }

    return true;
}

bool NinjaExecutor::
buildTargetAuxiliaryFiles(
    ninja::Writer *writer,
    pbxproj::PBX::Target::shared_ptr const &target,
    Target::Environment const &targetEnvironment,
    std::vector<Tool::Invocation const> const &invocations)
{
    // TODO(grp): In a dry run, Ninja will still need these files to exist, but the whole
    // point of a dry run is to avoid the filesystem. What's the best way to resolve this?
    if (_dryRun) {
        return true;
    }

    // TODO(grp): Could this defer writing auxiliary files and let Ninja do it?
    for (Tool::Invocation const &invocation : invocations) {
        for (Tool::Invocation::AuxiliaryFile const &auxiliaryFile : invocation.auxiliaryFiles()) {
            if (!FSUtil::CreateDirectory(FSUtil::GetDirectoryName(auxiliaryFile.path()))) {
                return false;
            }

            std::ofstream out;
            out.open(auxiliaryFile.path(), std::ios::out | std::ios::trunc | std::ios::binary);
            if (out.fail()) {
                return false;
            }

            out.write(auxiliaryFile.contents().data(), auxiliaryFile.contents().size() * sizeof(char));
            out.close();

            if (auxiliaryFile.executable() && !FSUtil::TestForExecute(auxiliaryFile.path())) {
                if (::chmod(auxiliaryFile.path().c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0) {
                    return false;
                }
            }
        }
    }

    return true;
}

bool NinjaExecutor::
buildTargetInvocations(
    pbxproj::PBX::Target::shared_ptr const &target,
    Target::Environment const &targetEnvironment,
    std::vector<Tool::Invocation const> const &invocations)
{
    std::string targetBegin = TargetNinjaBegin(target);

    /*
     * Start building the Ninja file for this target.
     */
    ninja::Writer writer;
    writer.comment("xcbuild ninja");
    writer.comment("Target: " + target->name());
    writer.newline();

    /*
     * Write out auxiliary files used by the invocations.
     */
    if (!buildTargetAuxiliaryFiles(&writer, target, targetEnvironment, invocations)) {
        return false;
    }

    /*
     * Add the build command for each invocation.
     */
    for (Tool::Invocation const &invocation : invocations) {
        // TODO(grp): This should perhaps be a separate flag for a 'phony' invocation.
        if (invocation.executable().empty()) {
            continue;
        }

        /*
         * Find the executable to use for the invocation. For builtin tools, this will return
         * a path to a real executable that Ninja can execute, rather than doing it in-process.
         */
        std::string executable = ResolveExecutable(invocation.executable(), targetEnvironment.sdk()->executablePaths());
        if (executable.empty()) {
            fprintf(stderr, "error: unable to find executable %s\n", invocation.executable().c_str());
            continue;
        }

        /*
         * Build the invocation arguments. Must escape for shell arguments as Ninja passes
         * the command string directly to the shell, which would interpret spaces, etc as meaningful.
         */
        std::string exec = ShellEscape(executable);
        for (std::string const &arg : invocation.arguments()) {
            exec += " " + ShellEscape(arg);
        }

        /*
         * Determine the status message for Ninja to print for this invocation.
         */
        std::string description = NinjaDescription(_formatter->beginInvocation(invocation, executable));

        /*
         * Build up the bindings for the invocation.
         */
        std::vector<ninja::Binding> bindings = {
            { "description", ninja::Value::String(description) },
            { "dir", ninja::Value::String(ShellEscape(invocation.workingDirectory())) },
            { "exec", ninja::Value::String(exec) },
        };

#if 0
        // TODO(grp): Two issues here.
        //  1. "-MT dependencies" doesn't work with Ninja.
        //  2. ld64 dependency files are some other format, not a Makefile.
        if (!invocation.dependencyInfo().empty()) {
            bindings.push_back({ "depfile", ninja::Value::String(invocation.dependencyInfo()) });
        }
#endif

        /*
         * Build up outputs as literal Ninja values.
         */
        std::vector<ninja::Value> outputs;
        for (std::string const &output : invocation.outputs()) {
            outputs.push_back(ninja::Value::String(output));
        }

        /*
         * Add fake output paths for outputs that may already exist. Ninja can't have
         * multiple build commands generating an output, so use fake paths for these.
         */
        for (std::string const &phonyOutput : invocation.phonyOutputs()) {
            std::string phonyOutputTarget = NinjaPhonyOutputTarget(phonyOutput);
            outputs.push_back(ninja::Value::String(phonyOutputTarget));
        }

        /*
         * Add phony rules for input dependencies that we don't know if they exist.
         * This can come up, for example, for user-specified custom script inputs.
         */
        for (std::string const &phonyInput : invocation.phonyInputs()) {
            writer.build({ ninja::Value::String(phonyInput) }, "phony", { });
        }

        /*
         * Build up inputs as literal Ninja values.
         */
        std::vector<ninja::Value> inputs;
        for (std::string const &input : invocation.inputs()) {
            inputs.push_back(ninja::Value::String(input));
        }

        /*
         * Build up input dependencies as literal Ninja values.
         */
        std::vector<ninja::Value> inputDependencies;
        for (std::string const &inputDependency : invocation.inputDependencies()) {
            inputDependencies.push_back(ninja::Value::String(inputDependency));
        }

        /*
         * Build up order dependencies as literal Ninja values.
         */
        std::vector<ninja::Value> orderDependencies;
        for (std::string const &orderDependency : invocation.orderDependencies()) {
            orderDependencies.push_back(ninja::Value::String(orderDependency));
        }

        /*
         * Depend on creating the directories to hold the outputs. Note the target
         * to create the directory will have been added above, before the invocations.
         *
         * These are order-only dependencies as the timestamp of the directory is not
         * important, it just has to exist.
         */
        std::unordered_set<std::string> outputDirectories;
        for (std::string const &output : invocation.outputs()) {
            std::string outputDirectory = FSUtil::GetDirectoryName(output);
            outputDirectories.insert(outputDirectory);
        }
        for (std::string const &outputDirectory : outputDirectories) {
            orderDependencies.push_back(ninja::Value::String(outputDirectory));
        }

        /*
         * All invocations depend on the target containing them beginning.
         */
        orderDependencies.push_back(ninja::Value::String(targetBegin));

        /*
         * Add the rule to build this invocation.
         */
        writer.build(outputs, NinjaRuleName(), inputs, bindings, inputDependencies, orderDependencies);
    }

    /*
     * Serialize the Ninja file into the build root.
     */
    std::string path = TargetNinjaPath(target, targetEnvironment);
    if (!WriteNinja(writer, path)) {
        return false;
    }

    /*
     * Note where the Ninja file is written.
     */
    fprintf(stderr, "Wrote %s ninja: %s\n", target->name().c_str(), path.c_str());

    return true;
}

std::unique_ptr<NinjaExecutor> NinjaExecutor::
Create(std::shared_ptr<Formatter> const &formatter, bool dryRun)
{
    return std::unique_ptr<NinjaExecutor>(new NinjaExecutor(
        formatter,
        dryRun
    ));
}