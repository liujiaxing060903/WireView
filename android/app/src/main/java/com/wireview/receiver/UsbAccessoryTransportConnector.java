package com.wireview.receiver;

import android.content.Context;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/** Product transport opened from an Android Open Accessory attachment. */
final class UsbAccessoryTransportConnector implements TransportConnector {
    private static final String TAG = "WireViewUsb";
    private final UsbManager usbManager;
    private final UsbAccessory accessory;

    UsbAccessoryTransportConnector(Context context, UsbAccessory accessory) {
        usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        this.accessory = accessory;
    }

    @Override
    public TransportConnection open() throws IOException {
        Log.i(TAG, "Opening Android USB accessory: " + accessory);
        PersistentDiagnosticLog.record("UsbConnector", "open requested: " + accessory);
        if (usbManager == null) {
            PersistentDiagnosticLog.record("UsbConnector", "USB manager is unavailable");
            throw new IOException("Android USB manager is unavailable");
        }
        if (!usbManager.hasPermission(accessory)) {
            PersistentDiagnosticLog.record("UsbConnector", "permission is not granted");
            throw new IOException("WireView USB accessory permission was not granted");
        }
        ParcelFileDescriptor descriptor = usbManager.openAccessory(accessory);
        if (descriptor == null) {
            PersistentDiagnosticLog.record("UsbConnector", "openAccessory returned null");
            throw new IOException("WireView USB accessory could not be opened");
        }
        Log.i(TAG, "Android USB accessory file descriptor opened");
        PersistentDiagnosticLog.record("UsbConnector", "accessory file descriptor opened");
        return new AccessoryConnection(descriptor);
    }

    private static final class AccessoryConnection implements TransportConnection {
        private final ParcelFileDescriptor descriptor;
        private final FileInputStream input;
        private final FileOutputStream output;

        AccessoryConnection(ParcelFileDescriptor descriptor) {
            this.descriptor = descriptor;
            input = new FileInputStream(descriptor.getFileDescriptor());
            output = new FileOutputStream(descriptor.getFileDescriptor());
        }

        @Override
        public InputStream inputStream() {
            return input;
        }

        @Override
        public OutputStream outputStream() {
            return output;
        }

        @Override
        public String displayName() {
            return "USB accessory";
        }

        @Override
        public void close() throws IOException {
            // Both streams refer to the same ParcelFileDescriptor. Closing the
            // descriptor once interrupts any blocking read/write on disconnect.
            PersistentDiagnosticLog.record("UsbConnector", "closing accessory descriptor");
            descriptor.close();
        }
    }
}
