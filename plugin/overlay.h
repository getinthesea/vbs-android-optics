#pragma once
// The binocular graticule over VBS's window on the PC, matching the phone's (CrosshairView.kt). Drawn in a
// transparent, click-through window owned by VBS's, so it stays over VBS but not over other programs, and
// stays out of the phone's video (the streamer captures VBS's own window only).

// VBS thread, every step: show it (while the phone drives the view) at this horizontal field of view
void overlay_update(bool show, float fov_deg);
void overlay_stop();
