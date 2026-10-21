#pragma once

namespace wireview::host {

// Wait for one IddCx driver relay session, validate its stream description,
// then acknowledge that the Host is ready to receive shared texture handles.
int run_idd_frame_relay_listener();

// Exercises the exact named-pipe handshake used by the driver/Host boundary.
int run_idd_frame_relay_pipe_loopback_test();

}  // namespace wireview::host
