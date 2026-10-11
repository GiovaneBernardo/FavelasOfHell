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
//   --terrain-sculpt  with --terrain: scripted brush strokes where the camera looks, from frame 60 (QA)
//   --roads-test      with --terrain: lays and carves a few roads along Leblon beach (QA)
//   --profile       log the average CPU time of each frame phase every 120 frames (QA)
//   --city [n]        the Leblon / Ipanema district (streets, buildings, beach) around the camera; --cam places the camera (QA)
//   --terrain [n]   create the terrain, open its panel and look at it: 0 from the sea, 1 Vidigal slope, 2 hill over the sea, 3 Leblon beach (QA)
//   --cam x,z,eye,yaw,pitch  with --terrain: the Scene camera there (eye = metres above the ground; QA)
//   --water-test [n] open the Water Test panel (coast scene) on camera preset n (0 hill, 1 beach, 2 aerial, 3 islet)
//   --autoplay s     scripted game input, e.g. "W:30-150,SPACE:90,C:150-210,V:240,MOUSEX:40-60=12" (frame ranges; QA)
//   --walk m/s       placed characters walk straight ahead at this speed (animation QA)
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
        } else if (!std::strcmp(argv[i], "--roads-test")) {
            options.roadsTest = true;
        } else if (!std::strcmp(argv[i], "--city")) {
            options.terrain = options.city = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') options.terrainView = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--terrain-sculpt")) {
            options.terrainSculpt = true;
        } else if (!std::strcmp(argv[i], "--profile")) {
            options.profile = true;
        } else if (!std::strcmp(argv[i], "--terrain")) {
            options.terrain = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') options.terrainView = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--cam") && i + 1 < argc) {
            options.cam = argv[++i];
        } else if (!std::strcmp(argv[i], "--water-test")) {
            options.waterTest = (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') ? std::atoi(argv[++i]) : 0;
        } else if (!std::strcmp(argv[i], "--player-model") && i + 1 < argc) {
            options.playerModel = argv[++i];
        } else if (!std::strcmp(argv[i], "--import-settings") && i + 1 < argc) {
            options.importSettings = argv[++i];
        } else if (!std::strcmp(argv[i], "--preview") && i + 1 < argc) {
            options.preview = argv[++i];
        } else if (!std::strcmp(argv[i], "--shots") && i + 1 < argc) {
            options.shots = argv[++i];
        } else if (!std::strcmp(argv[i], "--fullgame")) {
            options.fullGame = true;
        } else if (!std::strcmp(argv[i], "--autoplay") && i + 1 < argc) {
            options.autoplay = argv[++i];
        } else if (!std::strcmp(argv[i], "--walk") && i + 1 < argc) {
            options.walkSpeed = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(argv[i], "--stress")) {
            options.stress = true;
        } else {
            FOH_WARN("unknown argument '{}'", argv[i]);
        }
    }
    foh::Editor editor(options);
    return editor.run();
}
