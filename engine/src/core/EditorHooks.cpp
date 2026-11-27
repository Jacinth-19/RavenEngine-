#include "raven/core/EditorHooks.h"

namespace raven::EditorHooks {

static IEditorHooks* s_Hooks = nullptr;

void Register(IEditorHooks* hooks) { s_Hooks = hooks; }

IEditorHooks* Get() { return s_Hooks; }

} // namespace raven::EditorHooks
