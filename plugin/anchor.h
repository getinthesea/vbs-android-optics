#pragma once
// SQF that picks where the observer is, into vao_anchor. Shared by the optics camera and the DAGR so they agree.
// JFSim's order: var_ig (the OP, which JFSim's binos also look from), else igl, ig2 or projector; without JFSim,
// the player.
inline const char* ANCHOR_SQF =
    "vao_anchor = player; "
    "{ if (!isNil _x) then { vao_u = call compile _x; "
    "if (typeName vao_u == \"OBJECT\") then { if (!isNull vao_u) then { vao_anchor = vao_u; }; }; }; } "
    "forEach [\"projector\", \"ig2\", \"igl\", \"var_ig\"]; ";
