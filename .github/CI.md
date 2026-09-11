# GitHub CI Infrastructure

This directory contains the GitHub Actions workflows and reusable composite actions that implement the project's CI pipeline.

## Architecture

The CI infrastructure follows a **Recipe Pattern**:

- **Workflows** are recipes that define high-level, platform-agnostic orchestration.
- **Composite Actions** are ingredients that encapsulate reusable implementation details.
- **Configuration** is independent from the workflow and controls the analysis matrix and optional features.
- **Project-specific extensions** provide repository-specific environment setup without modifying the generic CI infrastructure.

This separation keeps workflows focused on orchestration while allowing implementation details and analysis parameters to evolve independently.

### Overall Flow

```text
Configuration
     |
     v
Process & Validate Configuration
     |
     v
Create Analysis Matrix
     |
     v
Analyze Each Matrix Entry
     |
     +-- Checkout
     +-- Setup environment
     |     +-- MSVC / Clang
     |     +-- LLVM tools
     |     +-- Ninja
     |     +-- [optional] project-specific setup
     |           e.g. Boost, CUDA, SDKs, environment variables
     +-- Check formatting
     |     +-- Python (Ruff)
     |     +-- C++ (clang-format)
     +-- CMake configure
     |     +-- Generate compile_commands.json
     |     +-- [optional] post-process compile commands
     +-- CodeQL Init [optional]
     +-- Build
     +-- Run clang-tidy
     +-- Perform CodeQL Analysis [optional]
     +-- Upload artifacts
```

The generic CI infrastructure owns only the standard build and analysis environment.

Repository-specific requirements are implemented through the optional project extension rather than being added to the shared composite action.

---

## Repository Structure

```text
.github/
├── actions/
│   ├── process-analysis-config/
│   │   └── action.yml
│   ├── setup-environment/
│   │   └── action.yml
│   ├── check-formatting/
│   │   └── action.yml
│   ├── configure-cmake/
│   │   └── action.yml
│   ├── build-project/
│   │   └── action.yml
│   └── run-clang-tidy/
│       └── action.yml
├── scripts/
│   ├── run-clang-tidy.py
│   └── RUN_CLANG_TIDY_README.md
├── workflows/
│   └── static-code-analysis.yml
├── static-code-analysis.json
└── CI.md
```

A repository may additionally provide:

```text
.github-ext/
├── static-code-analysis.json
└── setup_environment.py
```

The `.github-ext/` directory is intentionally outside `.github/`.

The generic `.github/` infrastructure is reusable across repositories and should not contain repository-specific dependencies, tool installation logic, or per-project analysis parameters.

The optional extension provides a controlled place for two kinds of repository-specific customization:

- **`static-code-analysis.json`** - a per-project override of the analysis matrix and optional features. When present, this file is used instead of `.github/static-code-analysis.json` (see [Configuration Override](#configuration-override) below).
- **`setup_environment.py`** - repository-specific environment setup, such as:
  - Boost
  - CUDA
  - spdlog
  - SDKs
  - additional command-line tools
  - project-specific environment variables
  - PATH modifications
  - other repository-specific preparation

Both files are optional and independent of each other. If neither exists, the generic CI behaves entirely from `.github/`.

The extension's contents are intentionally **free-form**. There is no artificial distinction between "dependencies" and "environment" because repository-specific setup may legitimately involve both.

Because all per-project variation lives in `.github-ext/`, the entire `.github/` directory can be replaced wholesale (e.g. to roll out a CI template update) without touching any repository-specific configuration or setup.

---

## Configuration

The analysis pipeline is controlled by:

```text
.github/static-code-analysis.json
```

The configuration defines the analysis matrix and optional analysis features without requiring changes to the workflow itself.

### Configuration Override

A repository may override the generic configuration by providing:

```text
.github-ext/static-code-analysis.json
```

Resolution order:

1. If `.github-ext/static-code-analysis.json` exists, it is loaded and used for the analysis matrix and optional features.
2. Otherwise, `.github/static-code-analysis.json` is loaded.

Exactly one of the two files is in effect for a given run - the override is not merged with the generic configuration, it replaces it entirely.

This lets a repository pin its own matrix and feature flags (e.g. a different `os`/`configurations` set, or `codeql: false`) without modifying `.github/`, keeping the generic configuration free to be updated independently across repositories.

If a repository has no override, it simply does not provide `.github-ext/static-code-analysis.json`, and the generic configuration applies unchanged.

### Configuration Example

```json
{
  "os": [
    "windows-latest",
    "ubuntu-latest"
  ],
  "configurations": [
    "Debug",
    "Release"
  ],
  "codeql": true
}
```

### Configuration Properties

| Property | Type | Description |
| --- | --- | --- |
| `os` | array | GitHub Actions runner operating systems to analyze |
| `configurations` | array | CMake build configurations to analyze |
| `codeql` | boolean | Enables or disables CodeQL analysis |

The `os` and `configurations` arrays form a Cartesian-product matrix.

For example, two operating systems and two configurations produce four analysis jobs:

```text
Windows + Debug
Windows + Release
Linux   + Debug
Linux   + Release
```

### Configuration Validation

Configuration is processed before the analysis matrix is created.

The configuration processor:

- Resolves which configuration file is in effect (`.github-ext/static-code-analysis.json` if present, otherwise `.github/static-code-analysis.json`).
- Loads the resolved file.
- Validates that required properties are present.
- Validates supported operating-system values.
- Validates build configurations.
- Validates that `codeql` is a boolean.
- Rejects empty `os` or `configurations` arrays.
- Produces normalized workflow outputs.

The configuration has **no implicit defaults**.

Invalid or incomplete configuration causes the workflow to fail rather than silently selecting fallback values.

### CodeQL Configuration

CodeQL is controlled exclusively by the `codeql` property of whichever configuration file is in effect.

Enable CodeQL:

```json
{
  "os": [
    "windows-latest",
    "ubuntu-latest"
  ],
  "configurations": [
    "Debug",
    "Release"
  ],
  "codeql": true
}
```

Disable CodeQL:

```json
{
  "os": [
    "windows-latest",
    "ubuntu-latest"
  ],
  "configurations": [
    "Debug",
    "Release"
  ],
  "codeql": false
}
```

All other analysis steps remain enabled regardless of the CodeQL setting.

---

## Workflow

### Static Code Analysis Recipe

The main workflow is:

```text
.github/workflows/static-code-analysis.yml
```

Its purpose is to perform cross-platform static analysis of the C++23 project using:

- clang-format
- clang-tidy
- CMake
- the project's configured compiler
- optionally CodeQL

The workflow itself contains orchestration logic only. Implementation details are delegated to composite actions.

### Triggers

The workflow can run from:

- pushes to branches
- pull requests
- the scheduled weekly run
- manual `workflow_dispatch`

### Trigger Policy

The workflow runs for pushes and pull requests on all branches, except when all changed files match the ignore policy.

The ignore policy is defined once and reused by both `push` and `pull_request`:

```yaml
on:
  push:
    branches: ["*"]
    paths-ignore: &ignore_policy
      - '**/*.md'
      - 'docs/**'
      - 'LICENSE'
      - '.gitignore'

  pull_request:
    branches: ["*"]
    paths-ignore: *ignore_policy
```

The ignored paths are:

| Pattern | Purpose |
| --- | --- |
| `**/*.md` | Markdown documentation files |
| `docs/**` | Documentation directory |
| `LICENSE` | License file |
| `.gitignore` | Git ignore configuration |

CI is skipped only when **all changed files** match the ignore policy.

For example:

- `README.md` only → CI does not run
- `docs/` change only → CI does not run
- `README.md` + `src/main.cpp` → CI runs
- `.github/static-code-analysis.json` → CI runs
- `.clang-format` or `.clang-tidy` → CI runs
- `CMakeLists.txt` or `CMakePresets.json` → CI runs

The policy deliberately uses an ignore list rather than a whitelist. New source, build, configuration, and policy files therefore automatically remain eligible for CI.

---

## Workflow Structure

The workflow contains two jobs:

```text
Process analysis configuration
             |
             v
        Analyze matrix
             |
       +-----+-----+
       |     |     |
       v     v     v
       Matrix entries
```

The first job processes and validates the configuration.

The second job creates and executes the analysis matrix.

---

## Job 1: Process Analysis Configuration

**Job ID:**

```text
process-analysis-config
```

**Display name:**

```text
Process analysis configuration
```

This job runs on `ubuntu-latest`.

It performs a sparse checkout containing the configuration and the action required to process it, then executes:

```text
process-analysis-config
```

The job exposes the processed configuration as workflow outputs:

```text
os
configurations
codeql
```

These outputs are consumed by the analysis job.

Keeping configuration processing in a separate job prevents configuration parsing and validation from being duplicated across matrix jobs.

---

## Job 2: Analyze

**Job ID:**

```text
analyze
```

The analysis job depends on the configuration-processing job and creates its matrix from the validated outputs:

```yaml
strategy:
  fail-fast: false
  matrix:
    os: ${{ fromJSON(needs.process-analysis-config.outputs.os) }}
    config: ${{ fromJSON(needs.process-analysis-config.outputs.configurations) }}
```

The matrix is the Cartesian product of the configured operating systems and build configurations.

For example:

```text
OS                 Configuration
--------------------------------
windows-latest     Debug
windows-latest     Release
ubuntu-latest      Debug
ubuntu-latest      Release
```

`fail-fast: false` ensures that a failure in one matrix job does not cancel the remaining matrix jobs.

The job grants only the permissions required for the analysis and CodeQL operations.

---

## Analysis Sequence

Each matrix job performs the following sequence.

### 1. Checkout Repository

The repository is checked out using:

```text
actions/checkout
```

The analysis job requires the complete repository.

### 2. Setup Environment

The `setup-environment` action prepares the standard compiler, analysis, and build environment.

```text
.github/actions/setup-environment/action.yml
```

The standard environment contains:

- the platform compiler environment
- LLVM
- clang-format
- clang-tidy
- Ninja

On Windows, the environment is initialized in the following order:

```text
MSVC
LLVM
Ninja
```

The tool-version report follows the same hierarchy:

```text
MSVC
LLVM / Clang tools
Ninja
```

For example:

```text
=== Tool versions (Windows) ===
MSVC ...
clang-format ...
clang-tidy ...
Ninja: ...
```

On Linux, the corresponding compiler and tools are reported in the same logical order.

After the standard environment is prepared, the action invokes the optional project-specific extension:

```text
.github-ext/setup_environment.py
```

If the file does not exist, the extension is skipped.

If it exists, it is executed using the CI Python interpreter. A non-zero exit code fails the setup step.

The shared action therefore remains generic while allowing an individual repository to install or configure whatever it actually requires.

For example, a CUDA repository may install CUDA, while a Boost-based repository may install Boost and export `BOOST_ROOT`.

Neither requirement needs to be implemented in the shared action.

### 3. Check Code Formatting

The `check-formatting` action verifies that the C++ source code conforms to the repository's clang-format configuration.

Formatting violations fail the analysis job.

### 4. Configure CMake

The `configure-cmake` action normalizes the matrix `config` value to lowercase and configures the project using the matching preset from `CMakePresets.json`:

```text
windows-<config>
linux-<config>
```

For example:

```text
windows-debug
windows-release
linux-debug
linux-release
```

The action does not reconstruct compiler flags, build types, or generator settings.

Those settings belong to `CMakePresets.json`.

The action exposes the resolved binary directory as:

```text
build-dir
```

Example:

```yaml
- name: Configure CMake
  id: configure
  uses: ./.github/actions/configure-cmake
  with:
    config: ${{ matrix.config }}
```

### 5. Post-process Compile Commands

The optional compile-command processing step takes the configured build directory and produces the compilation database used by clang-tidy.

It:

- locates and validates `<build-dir>/compile_commands.json`
- reports the number of compilation entries
- checks whether a repository-specific post-processing script exists
- invokes the script when present
- otherwise copies the compilation database unchanged
- validates the resulting database
- verifies that the compilation-entry count has not changed
- exposes the resulting directory as `db-dir`

The generic infrastructure has no compiler-specific knowledge.

Most repositories require no post-processing at all.

A repository whose actual compiler generates compilation commands that are not directly usable by clang-tidy may provide a repository-specific post-processing script.

For example, a CUDA project may need to translate or remove NVCC-specific arguments before clang-tidy's Clang frontend consumes the database.

The original compilation database is never modified.

### 6. Initialize CodeQL

When CodeQL is enabled, the workflow initializes CodeQL before the build.

This step is skipped when:

```text
codeql: false
```

### 7. Build Project

The `build-project` action builds the project using the matching CMake build preset:

```text
cmake --build --preset windows-<config>
cmake --build --preset linux-<config>
```

The build action does not independently reconstruct the build directory or compiler configuration.

`CMakePresets.json` remains the source of truth.

### 8. Run clang-tidy

The `run-clang-tidy` action performs clang-tidy analysis using the post-processed compilation database.

The analysis:

- runs in parallel
- uses available processor cores
- treats warnings as errors
- restricts analysis to the project's source tree

The parallel execution is implemented by:

```text
.github/scripts/run-clang-tidy.py
```

### 9. Perform CodeQL Analysis

When CodeQL is enabled, the workflow performs CodeQL analysis after the build.

Results use a matrix-specific category:

```text
/language:cpp/os:<os>/config:<config>
```

This keeps results from different matrix entries distinguishable.

### 10. Upload Build Logs

When the matrix job fails, build log files are uploaded as an artifact.

The logs are retained for seven days.

### 11. Upload Compilation Databases

The original and post-processed compilation databases are uploaded as a separate artifact.

The artifact identifies the matrix entry that produced it:

```text
compile-commands-<os>-<config>
```

The artifact is retained for seven days.

Keeping the compilation database separate from build logs makes it independently available for investigating compiler and clang-tidy issues.

---

## CodeQL Behavior

CodeQL is controlled exclusively by:

```text
.github/static-code-analysis.json
```

(or its `.github-ext/` override, if present - see [Configuration Override](#configuration-override))

When enabled, each matrix job:

1. Initializes CodeQL before the build.
2. Builds the project.
3. Performs CodeQL analysis.
4. Submits the results using a matrix-specific category.

When disabled, the CodeQL steps are skipped.

Formatting, configuration, compilation, and clang-tidy analysis remain enabled.

---

## Artifacts

Each matrix job can produce two types of artifacts:

| Artifact | Content | Condition | Retention |
| --- | --- | --- | --- |
| `build-logs-<os>-<config>` | Build log files | Job failure | 7 days |
| `compile-commands-<os>-<config>` | Original and post-processed `compile_commands.json` | Compilation database generated | 7 days |

The `<os>-<config>` suffix identifies the matrix entry that produced the artifact.

---

## Failure Behavior

The pipeline is intentionally strict.

Failures in:

- configuration processing
- environment setup
- formatting
- CMake configuration
- compilation
- clang-tidy
- CodeQL

cause the corresponding matrix job to fail.

Matrix jobs are independent because `fail-fast` is disabled. A failure on one operating system or build configuration does not prevent the remaining matrix entries from running.

---

# Composite Actions

Composite actions encapsulate reusable implementation details used by the workflow.

## `process-analysis-config`

```text
.github/actions/process-analysis-config/action.yml
```

Loads and validates the effective configuration (`.github-ext/static-code-analysis.json` if present, otherwise `.github/static-code-analysis.json` - see [Configuration Override](#configuration-override)).

Responsibilities:

- resolve which configuration file is in effect
- parse the JSON configuration
- validate required properties
- validate supported operating systems
- validate supported build configurations
- validate the CodeQL flag
- reject empty matrix dimensions
- normalize values for GitHub Actions
- expose configuration as workflow outputs

This action is responsible for configuration resolution and validation rather than the workflow itself.

---

## `setup-environment`

```text
.github/actions/setup-environment/action.yml
```

Prepares the standard CI development and analysis environment.

The action provides:

- the platform compiler environment
- LLVM
- clang-format
- clang-tidy
- Ninja

On Windows:

```text
MSVC
    |
LLVM / Clang tools
    |
Ninja
```

On Linux, the corresponding compiler, LLVM tools, and Ninja environment are prepared.

The action also invokes the optional:

```text
.github-ext/setup_environment.py
```

after the standard environment has been established.

The project-specific extension is intentionally outside the shared action and may perform arbitrary repository-specific setup.

Examples include:

- Boost installation
- CUDA installation
- additional libraries
- SDK installation
- environment-variable configuration
- PATH modifications
- project-specific tools

The shared action does not need to know which project-specific requirements exist.

If the extension does not exist, it is skipped.

---

## `check-formatting`

```text
.github/actions/check-formatting/action.yml
```

Checks the project's C++ source files against the repository's clang-format configuration.

Formatting differences cause the action to fail.

---

## `configure-cmake`

```text
.github/actions/configure-cmake/action.yml
```

Configures the project using the CMake preset matching the current operating system and configuration:

```text
cmake --preset windows-<config>
cmake --preset linux-<config>
```

The action normalizes the configuration name before selecting the preset.

Compiler selection, generator selection, build type, architecture, binary directory, and project-specific cache variables remain in:

```text
CMakePresets.json
```

The action exposes the preset's binary directory as:

```text
build-dir
```

---

## `post-process-compile-commands`

```text
.github/actions/post-process-compile-commands/action.yml
```

Takes the `build-dir` output from `configure-cmake` and produces the compilation database used for static analysis.

The action:

- validates that `compile_commands.json` exists
- validates that it contains valid JSON
- reports its entry count
- detects an optional repository-specific post-processing script
- invokes the script when present
- otherwise copies the database unchanged
- validates the resulting database
- verifies that the entry count is unchanged
- exposes the resulting directory as `db-dir`

The action contains no compiler- or language-specific logic.

Repository-specific compilation-database translation belongs outside `.github/`.

For example:

```text
scripts/post_process_compile_commands.py
```

may be provided by a CUDA repository when NVCC-generated commands require adaptation for clang-tidy.

Most repositories do not need this script.

---

## `build-project`

```text
.github/actions/build-project/action.yml
```

Builds the project through the matching CMake build preset:

```text
cmake --build --preset windows-<config>
cmake --build --preset linux-<config>
```

Like `configure-cmake`, the action normalizes the configuration name before selecting the preset.

The action does not independently reconstruct compiler or build-directory settings.

---

## `run-clang-tidy`

```text
.github/actions/run-clang-tidy/action.yml
```

Runs clang-tidy against the compilation database produced by:

```text
post-process-compile-commands
```

The action delegates parallel execution to:

```text
.github/scripts/run-clang-tidy.py
```

Additional implementation details are documented in:

```text
.github/scripts/RUN_CLANG_TIDY_README.md
```

---

# CMake Presets

`CMakePresets.json` is the single source of truth for build configuration.

It defines one preset for each supported operating-system/build-configuration combination, for example:

```text
windows-debug
windows-release
linux-debug
linux-release
```

Each preset owns its:

- compiler
- generator
- build type
- architecture
- binary directory
- project-specific cache variables

The CI infrastructure does not maintain a second independent representation of these settings.

Both local development and CI use the same presets.

For example:

```text
cmake --preset windows-debug
cmake --build --preset windows-debug
```

and:

```text
cmake --preset linux-debug
cmake --build --preset linux-debug
```

This prevents CI-specific compiler or build configuration from drifting away from the project's actual build configuration.

---

# Project-Specific Extensions

A repository may provide either or both of:

```text
.github-ext/static-code-analysis.json
.github-ext/setup_environment.py
```

Both files are optional and independent.

## `static-code-analysis.json` Override

When `.github-ext/static-code-analysis.json` exists, it replaces `.github/static-code-analysis.json` entirely for that repository's analysis matrix and optional features (see [Configuration Override](#configuration-override)). This lets a repository pin its own `os`/`configurations`/`codeql` settings without modifying the generic `.github/` infrastructure.

If a repository has no override, the generic `.github/static-code-analysis.json` applies unchanged.

## `setup_environment.py` Extension

This file is optional.

It is executed by `setup-environment` after the standard CI environment has been established.

The script is intentionally free-form because repository-specific requirements cannot reliably be divided into a fixed set of categories.

For example, a project may need to:

```text
Install Boost
Install CUDA
Install an SDK
Set BOOST_ROOT
Modify PATH
Install a project-specific tool
Configure environment variables
```

These operations belong to the repository rather than the generic CI infrastructure.

A project that requires none of them simply does not provide the extension.

The extension must fail with a non-zero exit code when required project-specific setup cannot be completed.

## Why Both Live in `.github-ext/`

Keeping both the configuration override and the environment-setup script in one directory, outside `.github/`, means all per-project variation is concentrated in a single, well-known location. As a direct consequence, the entire `.github/` directory can be replaced wholesale, for example to roll out an updated CI template across many repositories, without touching any repository-specific configuration or setup logic.

---

# Usage

## Run the Workflow Manually

The workflow can be started from the GitHub Actions interface using **Run workflow**.

## Change the Analysis Matrix

Modify:

```text
.github/static-code-analysis.json
```

For example:

```json
{
  "os": [
    "windows-latest"
  ],
  "configurations": [
    "Debug"
  ],
  "codeql": true
}
```

No workflow modification is required.

## Override the Analysis Matrix Per Repository

Create:

```text
.github-ext/static-code-analysis.json
```

with the same schema as `.github/static-code-analysis.json`. When present, it is used instead of the generic configuration for that repository (see [Configuration Override](#configuration-override)). No workflow or generic-configuration modification is required.

## Disable CodeQL

Set:

```json
"codeql": false
```

in whichever configuration file is in effect (`.github-ext/static-code-analysis.json` if present, otherwise `.github/static-code-analysis.json`).

The normal build, formatting, compilation database generation, and clang-tidy analysis continue to run.

## Add Project-Specific Environment Setup

Create:

```text
.github-ext/setup_environment.py
```

The generic `setup-environment` action will detect and execute it automatically.

No modification to the shared composite action is required.

## Run Analysis Locally

The individual build components can be run locally using the same CMake presets consumed by CI:

```text
cmake --preset windows-debug
cmake --build --preset windows-debug
```

or:

```text
cmake --preset linux-debug
cmake --build --preset linux-debug
```

The GitHub Actions pipeline remains the authoritative cross-platform validation environment because it exercises the configured runner environments and analysis matrix.

---

# Extending the Pipeline

New generic functionality should normally be implemented as a composite action rather than by adding implementation details directly to the workflow.

For example:

```text
.github/actions/new-analysis-step/
└── action.yml
```

The workflow should then orchestrate the new action at the appropriate point in the analysis sequence.

Configuration-dependent behavior should be added to:

```text
.github/static-code-analysis.json
```

and processed by:

```text
process-analysis-config
```

Repository-specific analysis-matrix overrides should be made in `.github-ext/static-code-analysis.json` rather than in the generic configuration.

Repository-specific environment setup should normally be implemented in:

```text
.github-ext/setup_environment.py
```

rather than modifying the shared `setup-environment` action.

---

# Design Principles

## Separation of Concerns

Each layer has a distinct responsibility:

```text
Configuration
    |
    +-- What should be analyzed?

Workflow
    |
    +-- When and in what order?

Composite Actions
    |
    +-- How is each generic operation performed?

CMakePresets.json
    |
    +-- What configuration is built?

.github-ext/
    |
    +-- What is specific to this repository?
```

## Configuration Is Independent

The analysis matrix and optional features can be changed without modifying the workflow, whether by editing `.github/static-code-analysis.json` directly or by providing a `.github-ext/static-code-analysis.json` override.

## Validation Is Explicit

Invalid configuration fails clearly rather than silently falling back to defaults.

## Platform Details Stay in Actions and Presets

Operating-system-specific implementation belongs in composite actions and `CMakePresets.json` wherever practical.

The workflow should remain readable as a high-level recipe.

## Project-Specific Requirements Stay Outside Generic CI

The generic CI infrastructure should not accumulate project-specific dependency, tool installation, or analysis-parameter logic.

Repository-specific requirements belong in the optional `.github-ext/` extension: `static-code-analysis.json` for configuration overrides and `setup_environment.py` for environment setup.

Because all such variation is concentrated there, the entire `.github/` directory can be replaced wholesale without affecting any individual repository's specific setup.

This keeps the shared action stable as more repositories adopt the template.

## CMakePresets.json Is the Build Source of Truth

CI should invoke the project's CMake presets rather than reconstructing compiler flags, build types, generators, or binary directories.

## Matrix Jobs Are Independent

A failure in one OS/configuration combination should not prevent other combinations from producing results.

## Reusable Components

Common generic operations should be implemented once and reused rather than duplicated across workflows.

## Compiler-Specific Analysis Translation Stays Outside `.github/`

If an analysis tool requires a compilation database different from the one generated by the project's actual compiler, repository-specific translation belongs outside the generic CI infrastructure.

For example:

```text
scripts/post_process_compile_commands.py
```

may adapt CUDA/NVCC compilation commands for clang-tidy.

Most repositories require no such script.

---

# Maintenance

When modifying the CI infrastructure:

1. Keep workflow orchestration high-level.
2. Put reusable generic implementation in composite actions.
3. Put analysis parameters in `.github/static-code-analysis.json`, and per-repository overrides in `.github-ext/static-code-analysis.json`.
4. Keep configuration resolution and validation centralized in `process-analysis-config`.
5. Keep project-specific environment setup in `.github-ext/setup_environment.py`.
6. Keep platform-specific build configuration in the appropriate composite action or `CMakePresets.json`.
7. Avoid introducing implicit configuration defaults.
8. Keep compiler-specific compilation-database translation repository-specific.
9. Update `CI.md` when the CI architecture or behavior changes.
10. Test all affected matrix combinations before merging.

When adding a new capability, first determine whether it belongs in:

```text
Configuration
Workflow orchestration
Generic composite action
CMakePresets.json
Project-specific extension
```

before modifying the pipeline.

---

# Contributing

Changes to the CI infrastructure should be tested against all affected matrix combinations before being merged.

When changing the workflow architecture or behavior, update:

```text
.github/CI.md
```

to keep the documentation consistent with the implementation.

---

**Last Updated:** 22/09/2026
**Maintainer:** AmitGDev
