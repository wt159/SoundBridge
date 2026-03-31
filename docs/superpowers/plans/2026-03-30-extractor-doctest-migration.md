# Extractor Doctest Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the first real-media extractor assertions from `TestSdkSuite` into `UnitTests` so extractor coverage starts shifting to the doctest-based suite without destabilizing the existing compatibility harness.

**Architecture:** Keep the current dual-runner model. `UnitTests` remains the main doctest target for new work, while `TestSdkSuite` stays as the transitional smoke/spec harness. Add a small reusable real-media helper inside `sdk/test/unit/test_extractor.cpp`, then migrate three stable formats first: WAV, AAC, and FLAC.

**Tech Stack:** C++11, doctest 2.4.11, CMake/CTest, existing `FileSource`, `ExtractorFactory`, `AudioSpec`, `SB_MEDIA_DIR` environment variable.

---

### Task 1: Correct the design document and lock the migration direction

**Files:**
- Modify: `doc/testing_arch_design.md`
- Create: `docs/superpowers/plans/2026-03-30-extractor-doctest-migration.md`

- [ ] **Step 1: Update the framework recommendation to match the repository**

Replace the stale “Doctest + GoogleTest mixed” recommendation with “Doctest primary + TestSdkSuite transitional compatibility layer”.

- [ ] **Step 2: Update the architecture and migration sections**

Document the current test targets (`sdk_unit_tests`, `sdk_extractor_spec_tests`, `sdk_media_smoke`) and the first migration slice (WAV/AAC/FLAC into `sdk/test/unit/test_extractor.cpp`).

- [ ] **Step 3: Record coverage risks explicitly**

Call out duplicated coverage flag injection in root `CMakeLists.txt` and `sdk/test/CMakeLists.txt`, plus the need for staged thresholds.

### Task 2: Add the first failing real-media doctest cases

**Files:**
- Modify: `sdk/test/unit/test_extractor.cpp`
- Reference: `sdk/test/TestSdkSuite.cpp:576-827`
- Test: `build/sdk/test/UnitTests`

- [ ] **Step 1: Write a small real-media helper in the extractor doctest file**

Add a helper that:

```cpp
static std::string getMediaDir();
static std::string joinMediaPath(const std::string &fileName);
static bool fileExists(const std::string &path);
```

and a helper for opening an extractor:

```cpp
static std::unique_ptr<ExtractorHelper> createRealMediaExtractor(
    const std::string &fileName,
    const std::string &extension,
    std::shared_ptr<FileSource> &source);
```

- [ ] **Step 2: Write failing doctest cases for WAV, AAC, and FLAC**

Add tests that use the real files:

```cpp
TEST_CASE("Extractor real media WAV audio spec")
TEST_CASE("Extractor real media AAC audio spec")
TEST_CASE("Extractor real media FLAC audio spec")
```

Each case should:
- Resolve `SB_MEDIA_DIR` with fallback `../../music`
- Fail clearly if the file is unexpectedly missing in the checked-in media directory
- Assert `initCheck() == sdk_utils::OK`
- Assert core `AudioSpec` fields (`sampleRate`, `numChannel`, `durationMs`)
- For WAV/AAC/FLAC only, assert `format != AudioFormatUnknown`

- [ ] **Step 3: Run only the new extractor doctest cases and confirm RED**

Run:

```bash
cmake -E env "LD_LIBRARY_PATH=/home/wtp/workspace/SoundBridge/sdk/3rdparty/dist/linux_x86_64_gcc_debug/lib" ./sdk/test/UnitTests --test-suite=ExtractorRealMedia
```

Expected: FAIL because the new helper/tests are incomplete or missing.

### Task 3: Make the new doctest cases pass with minimal implementation

**Files:**
- Modify: `sdk/test/unit/test_extractor.cpp`

- [ ] **Step 1: Add the minimal helper implementation**

Implement the path/environment helpers with only standard library and existing project types.

- [ ] **Step 2: Keep scope intentionally small**

Do not migrate all 12 formats. Only support:
- `music.wav`
- `48000_fltp_1.aac`
- `小镇姑娘-陶喆.flac`

- [ ] **Step 3: Run the new extractor doctest cases and confirm GREEN**

Run:

```bash
cmake -E env "LD_LIBRARY_PATH=/home/wtp/workspace/SoundBridge/sdk/3rdparty/dist/linux_x86_64_gcc_debug/lib" ./sdk/test/UnitTests --test-suite=ExtractorRealMedia
```

Expected: PASS.

### Task 4: Verify against the existing harness

**Files:**
- Verify: `sdk/test/unit/test_extractor.cpp`
- Verify: `doc/testing_arch_design.md`

- [ ] **Step 1: Run the full doctest suite**

Run:

```bash
ctest -R sdk_unit_tests --output-on-failure
```

Expected: PASS.

- [ ] **Step 2: Run the legacy extractor spec suite**

Run:

```bash
ctest -R sdk_extractor_spec_tests --output-on-failure
```

Expected: PASS or explicit skips only for genuinely unavailable media; no new regressions.

- [ ] **Step 3: Check diagnostics on changed files**

Run diagnostics on:
- `sdk/test/unit/test_extractor.cpp`
- `doc/testing_arch_design.md`

- [ ] **Step 4: Record remaining follow-up work**

List the next migration batch and the separate coverage-cleanup task. Do not mix coverage refactoring into this extractor migration commit.
