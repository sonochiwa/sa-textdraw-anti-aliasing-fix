#pragma once

// True when `returnAddress` lies inside samp.dll. The preview cameras are
// owned by the multiplayer client, so the plugin only reacts to RenderWare
// calls that are entered from it.
bool IsMultiplayerCaller(const void* returnAddress);
