package com.example.heartfit;

import android.Manifest;
import android.annotation.SuppressLint;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;

public class display extends AppCompatActivity implements BLEManager.BLEDataListener {

    private static final String TAG = "HeartRateActivity";
    private static final int REQUEST_CODE_PERMISSIONS = 1;

    // ── UI views — add more TextViews in your layout to show all four fields ──
    private TextView tvHR;
    private TextView tvLSTM;
    private TextView tvSpO2;
    private TextView tvSteps;

    private BLEManager bleManager;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_heart_rate);

        // Wire up TextViews — make sure these IDs exist in activity_heart_rate.xml
        tvHR    = findViewById(R.id.bpm);          // existing view
        tvLSTM  = findViewById(R.id.lstm_hr);      // new: AI-predicted HR
        tvSpO2  = findViewById(R.id.spo2);         // new: SpO2 %
        tvSteps = findViewById(R.id.steps);        // new: step count

        bleManager = BLEManager.getInstance(this);
        requestPermissionsIfNeeded();
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (hasPermissions()) {
            bleManager.addListener(this);
            bleManager.startScanAndConnect();
        }
    }

    @Override
    protected void onStop() {
        super.onStop();
        bleManager.removeListener(this);
    }

    // ── BLEDataListener ───────────────────────────────────────────────────────
    /**
     * Called once per BLE notification with all four values parsed from the
     * CSV string "HR:72,LSTM:73.5,SPO2:98,STEPS:1234".
     */
    @SuppressLint("SetTextI18n")
    @Override
    public void onDataUpdate(int hr, float lstmHr, int spo2, int steps) {
        Log.d(TAG, "Data received → HR:" + hr + " LSTM:" + lstmHr
                + " SpO2:" + spo2 + " Steps:" + steps);

        runOnUiThread(() -> {
            if (tvHR    != null) tvHR.setText("HR: "    + hr    + " bpm");
            if (tvLSTM  != null) tvLSTM.setText("AI HR: " + String.format("%.1f", lstmHr) + " bpm");
            if (tvSpO2  != null) tvSpO2.setText("SpO2: " + spo2  + "%");
            if (tvSteps != null) tvSteps.setText("Steps: " + steps);
        });
    }

    @Override
    public void onConnectionStateChange(boolean connected) {
        runOnUiThread(() ->
            Toast.makeText(this,
                connected ? "✅ Connected to B-Fit Tracker" : "❌ Disconnected",
                Toast.LENGTH_SHORT).show()
        );
    }

    // ── Permission helpers ────────────────────────────────────────────────────
    private void requestPermissionsIfNeeded() {
        if (hasPermissions()) {
            startBle();
            return;
        }
        String[] perms = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                ? new String[]{
                        Manifest.permission.BLUETOOTH_CONNECT,
                        Manifest.permission.BLUETOOTH_SCAN }
                : new String[]{ Manifest.permission.ACCESS_FINE_LOCATION };

        ActivityCompat.requestPermissions(this, perms, REQUEST_CODE_PERMISSIONS);
    }

    private boolean hasPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return ActivityCompat.checkSelfPermission(this,
                    Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
                && ActivityCompat.checkSelfPermission(this,
                    Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED;
        }
        return ActivityCompat.checkSelfPermission(this,
                Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    private void startBle() {
        bleManager.addListener(this);
        bleManager.startScanAndConnect();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode,
                                           @NonNull String[] permissions,
                                           @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_CODE_PERMISSIONS) {
            boolean granted = true;
            for (int result : grantResults) {
                if (result != PackageManager.PERMISSION_GRANTED) { granted = false; break; }
            }
            if (granted) startBle();
            else Toast.makeText(this,
                    "Permissions required for BLE", Toast.LENGTH_SHORT).show();
        }
    }
}
