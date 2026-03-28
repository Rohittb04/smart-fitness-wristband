package com.example.heartfit;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothProfile;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.util.Log;

import androidx.core.app.ActivityCompat;

import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Set;
import java.util.UUID;

class BLEManager extends Activity {

    private static final String TAG = "BLEManager";

    private static BLEManager instance;

    // ── Listener: now delivers all four values + LSTM ────────────────────────
    public interface BLEDataListener {
        void onDataUpdate(int hr, float lstmHr, int spo2, int steps);
        void onConnectionStateChange(boolean connected);
    }

    // ── UUIDs — only ONE characteristic now ──────────────────────────────────
    private static final String DEVICE_NAME   = "B-Fit Tracker";
    private static final UUID   SERVICE_UUID  = UUID.fromString("12345678-1234-1234-1234-1234567890ab");
    // Single characteristic that receives: "HR:72,LSTM:73.5,SPO2:98,STEPS:1234"
    private static final UUID   DATA_CHAR_UUID = UUID.fromString("00002a00-0000-1000-8000-00805f9b34fb");
    private static final UUID   CCCD_UUID      = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private BluetoothAdapter            bluetoothAdapter;
    private BluetoothGatt               bluetoothGatt;
    private BluetoothGattCharacteristic dataCharacteristic;

    private final Set<BLEDataListener> listeners = new HashSet<>();
    private Context context;

    // ── Singleton ─────────────────────────────────────────────────────────────
    private BLEManager(Context context) {
        this.context        = context.getApplicationContext();
        this.bluetoothAdapter = BluetoothAdapter.getDefaultAdapter();
    }

    public static synchronized BLEManager getInstance(Context context) {
        if (instance == null) instance = new BLEManager(context);
        return instance;
    }

    // ── Listener management ───────────────────────────────────────────────────
    public void addListener(BLEDataListener listener)    { listeners.add(listener);    }
    public void removeListener(BLEDataListener listener) { listeners.remove(listener); }

    // ── Scan + connect ────────────────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    public void startScanAndConnect() {
        if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled()) {
            notifyConnectionState(false);
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ActivityCompat.checkSelfPermission(context,
                    Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this,
                        new String[]{
                                Manifest.permission.BLUETOOTH_SCAN,
                                Manifest.permission.BLUETOOTH_CONNECT,
                                Manifest.permission.ACCESS_FINE_LOCATION
                        }, 1001);
                return;
            }
        }

        bluetoothAdapter.startLeScan((device, rssi, scanRecord) -> {
            if (DEVICE_NAME.equals(device.getName())) {
                bluetoothAdapter.stopLeScan(null);
                connectToDevice(device);
            }
        });
    }

    // ── GATT connection ───────────────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    private void connectToDevice(BluetoothDevice device) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                ActivityCompat.checkSelfPermission(context,
                        Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this,
                    new String[]{Manifest.permission.BLUETOOTH_CONNECT}, 1001);
            return;
        }

        bluetoothGatt = device.connectGatt(context, false, new BluetoothGattCallback() {

            @Override
            public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    notifyConnectionState(true);
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                            ActivityCompat.checkSelfPermission(context,
                                    Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED)
                        return;
                    gatt.discoverServices();

                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    notifyConnectionState(false);
                }
            }

            @Override
            public void onServicesDiscovered(BluetoothGatt gatt, int status) {
                if (status != BluetoothGatt.GATT_SUCCESS) return;

                // Only subscribe to the single combined characteristic
                dataCharacteristic = gatt
                        .getService(SERVICE_UUID)
                        .getCharacteristic(DATA_CHAR_UUID);

                if (dataCharacteristic != null) {
                    enableNotification(dataCharacteristic);
                    Log.d(TAG, "Subscribed to DATA characteristic");
                } else {
                    Log.e(TAG, "DATA characteristic not found!");
                }
            }

            @Override
            public void onCharacteristicChanged(BluetoothGatt gatt,
                                                BluetoothGattCharacteristic characteristic) {
                if (!DATA_CHAR_UUID.equals(characteristic.getUuid())) return;

                // Decode the UTF-8 CSV string: "HR:72,LSTM:73.5,SPO2:98,STEPS:1234"
                String raw = new String(characteristic.getValue(), StandardCharsets.UTF_8).trim();
                Log.d(TAG, "BLE payload: " + raw);
                parseAndNotify(raw);
            }
        });
    }

    // ── CSV parser ────────────────────────────────────────────────────────────
    /**
     * Parses "HR:72,LSTM:73.5,SPO2:98,STEPS:1234" and delivers values to listeners.
     * Safe — any missing/malformed field is treated as 0 / 0.0f.
     */
    private void parseAndNotify(String payload) {
        int   hr    = 0;
        float lstm  = 0f;
        int   spo2  = 0;
        int   steps = 0;

        try {
            // Split on comma → ["HR:72", "LSTM:73.5", "SPO2:98", "STEPS:1234"]
            for (String token : payload.split(",")) {
                String[] kv = token.split(":");
                if (kv.length != 2) continue;
                String key = kv[0].trim();
                String val = kv[1].trim();

                switch (key) {
                    case "HR":    hr    = Integer.parseInt(val);   break;
                    case "LSTM":  lstm  = Float.parseFloat(val);   break;
                    case "SPO2":  spo2  = Integer.parseInt(val);   break;
                    case "STEPS": steps = Integer.parseInt(val);   break;
                    default:
                        Log.w(TAG, "Unknown key: " + key);
                }
            }
        } catch (NumberFormatException e) {
            Log.e(TAG, "Parse error on payload: " + payload, e);
        }

        // Deliver all four values in a single callback
        final int   fHr    = hr;
        final float fLstm  = lstm;
        final int   fSpo2  = spo2;
        final int   fSteps = steps;

        for (BLEDataListener listener : listeners) {
            listener.onDataUpdate(fHr, fLstm, fSpo2, fSteps);
        }
    }

    // ── Enable BLE notifications ──────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    private void enableNotification(BluetoothGattCharacteristic characteristic) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                ActivityCompat.checkSelfPermission(context,
                        Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED)
            return;

        bluetoothGatt.setCharacteristicNotification(characteristic, true);
        BluetoothGattDescriptor descriptor = characteristic.getDescriptor(CCCD_UUID);
        if (descriptor != null) {
            descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            bluetoothGatt.writeDescriptor(descriptor);
        }
    }

    // ── Disconnect ────────────────────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    public void disconnect() {
        if (bluetoothGatt != null) {
            bluetoothGatt.disconnect();
            bluetoothGatt.close();
            bluetoothGatt = null;
        }
        notifyConnectionState(false);
    }

    // ── Internal notify helpers ───────────────────────────────────────────────
    private void notifyConnectionState(boolean connected) {
        for (BLEDataListener listener : listeners) {
            listener.onConnectionStateChange(connected);
        }
    }
}