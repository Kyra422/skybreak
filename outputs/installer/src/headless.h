#pragma once
// INSTALL19: --headless runs the same engine steps as the wizard, with the player's choices from --answers FILE.json.
// Used by the tests and the sandbox E2E; exit codes per ExitCode (engine.h). A result JSON is written to --result.
#include "engine.h"

namespace sb {

struct HeadlessIo {
    fs::path result; // --result FILE (optional)
};

int RunHeadless(Engine& e, const HeadlessIo& io);
// Fill Choices from an answers object (defaults from the engine for missing keys).
Choices ChoicesFromAnswers(Engine& e, const Json& a);

} // namespace sb
