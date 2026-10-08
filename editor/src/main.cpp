#include <cstdlib>
#include <cstring>

#include "editor.h"
#include "foh/log.h"

// Usage: foh_editor [--frames N] [--screenshot out.png] [--no-validation] [--no-vsync] [--stress]
//   --frames N       render N frames, then exit (smoke test; exit code 2 on graphics API errors)
//   --screenshot f   with --frames: save the last frame as a PNG
//   --group-passes N passes per command list before a graph is split (1: one list per pass, QA)
//   --import file    import a model/texture at startup and place it, exactly like dropping it on the window
//   --stress         scripted resizes / scene switches / panel toggles (QA)
int main(int argc, char** argv) {
    foh::EditorOptions options;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
            options.frames = static_cast<foh::u32>(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            options.screenshot = argv[++i];
        } else if (!std::strcmp(argv[i], "--no-validation")) {
            options.validation = false;
        } else if (!std::strcmp(argv[i], "--no-vsync")) {
            options.vsync = false;
        } else if (!std::strcmp(argv[i], "--group-passes") && i + 1 < argc) {
            options.minPassesPerGroup = static_cast<foh::u32>(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--import") && i + 1 < argc) {
            options.importFile = argv[++i];
        } else if (!std::strcmp(argv[i], "--stress")) {
            options.stress = true;
        } else {
            FOH_WARN("unknown argument '{}'", argv[i]);
        }
    }
    foh::Editor editor(options);
    return editor.run();
}
