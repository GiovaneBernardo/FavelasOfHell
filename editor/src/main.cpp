#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "editor.h"
#include "foh/log.h"

// Usage: foh_editor [--frames N] [--screenshot out.png] [--no-validation] [--no-vsync] [--stress]
//   --frames N       render N frames, then exit (smoke test; exit code 2 on graphics API errors)
//   --screenshot f   with --frames: save the last frame as a PNG
//   --group-passes N passes per command list before a graph is split (1: one list per pass, QA)
//   --import file    import a model/texture at startup and place it, exactly like dropping it on the window
//   --prefab-edit m  open the prefab editor on an imported model (path relative to assets/)
//   --inspect p      show a file in the asset inspector (path relative to assets/)
//   --window WxH     window size for smoke runs (default 1600x900)
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
        } else if (!std::strcmp(argv[i], "--prefab-edit") && i + 1 < argc) {
            options.prefabEdit = argv[++i];
        } else if (!std::strcmp(argv[i], "--prefab-qa")) {
            options.prefabQa = true;
        } else if (!std::strcmp(argv[i], "--inspect") && i + 1 < argc) {
            options.inspect = argv[++i];
        } else if (!std::strcmp(argv[i], "--window") && i + 1 < argc) {
            unsigned w = 0, h = 0;
            if (std::sscanf(argv[++i], "%ux%u", &w, &h) == 2 && w >= 640 && h >= 360) {
                options.windowWidth = w;
                options.windowHeight = h;
            }
        } else if (!std::strcmp(argv[i], "--workshop-test")) {
            options.openWorkshop = options.workshopTest = true;
        } else if (!std::strcmp(argv[i], "--workshop")) {
            options.openWorkshop = true;
        } else if (!std::strcmp(argv[i], "--stress")) {
            options.stress = true;
        } else {
            FOH_WARN("unknown argument '{}'", argv[i]);
        }
    }
    foh::Editor editor(options);
    return editor.run();
}
