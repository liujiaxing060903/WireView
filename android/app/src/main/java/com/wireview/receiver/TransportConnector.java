package com.wireview.receiver;

import java.io.IOException;

/** Opens one WireView transport connection. */
interface TransportConnector {
    TransportConnection open() throws IOException;
}
