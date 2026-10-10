#pragma once

// Input as seen by game code: a resource the host (editor or runtime) refreshes before every world update. Game
// systems read it through Res<GameInput>; they never touch the window.
//
//   captured   the game owns the mouse (relative mode) and the keyboard. When false (the editor UI has focus) the
//              state is empty, so a player standing in the Game viewport does not run around while you type.
//
// Thread-safety: written on the main thread between updates, read-only while systems run.

#include "foh/window.h"

namespace foh {

struct GameInput {
    InputState state;
    bool captured = false;
};

} // namespace foh
