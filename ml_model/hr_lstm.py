"""
B-Fit LSTM Heart Rate Predictor
================================
Input  : sliding window of 100 samples × 4 features
         [Acc_X, Acc_Y, Acc_Z, PPG_Raw]  (100 Hz → 1-second context)
Output : predicted heart rate (bpm)

Run:
    pip install tensorflow scikit-learn matplotlib pandas numpy
    python train_lstm.py

Outputs:
    model.h            ← paste into your Arduino sketch folder
    scaler_params.h    ← normalisation constants for ESP32
    training_plots.png ← loss + prediction charts
"""

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")          # headless — no display needed
import matplotlib.pyplot as plt
from sklearn.preprocessing import MinMaxScaler
from sklearn.model_selection import train_test_split
from sklearn.metrics import mean_absolute_error, mean_squared_error

import tensorflow as tf
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import (LSTM, Dense, Dropout,
                                     Input, Bidirectional)
from tensorflow.keras.callbacks import (EarlyStopping, ReduceLROnPlateau,
                                        ModelCheckpoint)
import os, struct

# ─── 0. Reproducibility ───────────────────────────────────────────────────────
SEED = 42
np.random.seed(SEED)
tf.random.set_seed(SEED)

# ─── 1. Load data ─────────────────────────────────────────────────────────────
CSV_PATH = "ml_model/sensor_dataset.csv"
FEATURES = ["Acc_X", "Acc_Y", "Acc_Z", "PPG_Raw"]
TARGET   = "HeartRate_Label"

print("[1] Loading data …")
df = pd.read_csv(CSV_PATH)
print(f"    Rows: {len(df):,}  |  HR range: {df[TARGET].min()}–{df[TARGET].max()} bpm")

# ─── 2. Normalise ─────────────────────────────────────────────────────────────
# MinMax per feature — we'll bake the params into scaler_params.h for the ESP32
print("[2] Normalising features …")
scaler = MinMaxScaler()
X_scaled = scaler.fit_transform(df[FEATURES].values).astype(np.float32)
y        = df[TARGET].values.astype(np.float32)

# Also normalise the target (helps LSTM converge; we'll denormalise predictions)
y_min, y_max = float(y.min()), float(y.max())
y_scaled = (y - y_min) / (y_max - y_min + 1e-8)

print(f"    Feature mins : {scaler.data_min_}")
print(f"    Feature maxs : {scaler.data_max_}")
print(f"    HR  min={y_min:.1f}  max={y_max:.1f}")

# ─── 3. Build sliding windows ─────────────────────────────────────────────────
# 100-sample window = 1 second at 100 Hz — captures one full cardiac cycle.
# Stride = 1 → maximum training examples, rich temporal coverage.
TIMESTEPS = 100    # ← update LSTM_TIMESTEPS in config.h to 100
STRIDE    = 1

print("[3] Building windows …")
X_wins, y_wins = [], []
for i in range(TIMESTEPS, len(X_scaled), STRIDE):
    X_wins.append(X_scaled[i - TIMESTEPS : i])
    y_wins.append(y_scaled[i])

X_wins = np.array(X_wins)          # (N, 100, 4)
y_wins = np.array(y_wins)
print(f"    Windows: {X_wins.shape}  Labels: {y_wins.shape}")

# ─── 4. Train / val / test split ──────────────────────────────────────────────
# Temporal split — no shuffle! (future can't inform past)
print("[4] Splitting …")
n       = len(X_wins)
n_train = int(n * 0.70)
n_val   = int(n * 0.15)

X_train, y_train = X_wins[:n_train],          y_wins[:n_train]
X_val,   y_val   = X_wins[n_train:n_train+n_val], y_wins[n_train:n_train+n_val]
X_test,  y_test  = X_wins[n_train+n_val:],    y_wins[n_train+n_val:]

print(f"    Train={len(X_train):,}  Val={len(X_val):,}  Test={len(X_test):,}")

# ─── 5. Model architecture ────────────────────────────────────────────────────
# Design principles for TFLite Micro on ESP32:
#   • Single LSTM layer (maps to UnidirectionalSequenceLSTM op)
#   • Small hidden dim (32) — <20 KB weights
#   • Float32 (no INT8 quantisation to avoid calibration headache)
#   • ~5 K total parameters → fits in 24 KB tensor arena
print("[5] Building model …")

model = Sequential([
    Input(shape=(TIMESTEPS, len(FEATURES))),
    LSTM(32, return_sequences=False),    # core temporal feature extractor
    Dropout(0.2),
    Dense(16, activation="relu"),
    Dense(1,  activation="sigmoid"),     # output in [0,1] → denorm → bpm
], name="hr_lstm")

model.compile(
    optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3),
    loss="mse",
    metrics=["mae"]
)
model.summary()

# ─── 6. Train ─────────────────────────────────────────────────────────────────
print("[6] Training …")
callbacks = [
    EarlyStopping(monitor="val_mae", patience=10,
                  restore_best_weights=True, verbose=1),
    ReduceLROnPlateau(monitor="val_loss", factor=0.5,
                      patience=5, min_lr=1e-6, verbose=1),
    ModelCheckpoint("best_model.keras", save_best_only=True,
                    monitor="val_mae", verbose=0),
]

history = model.fit(
    X_train, y_train,
    validation_data=(X_val, y_val),
    epochs=6,
    batch_size=256,
    callbacks=callbacks,
    verbose=1,
)

# ─── 7. Evaluate ──────────────────────────────────────────────────────────────
print("[7] Evaluating on test set …")
y_pred_scaled = model.predict(X_test, batch_size=256).flatten()

# Denormalise
y_pred_bpm = y_pred_scaled * (y_max - y_min) + y_min
y_true_bpm = y_test       * (y_max - y_min) + y_min

mae  = mean_absolute_error(y_true_bpm, y_pred_bpm)
rmse = np.sqrt(mean_squared_error(y_true_bpm, y_pred_bpm))
print(f"    Test MAE  = {mae:.3f} bpm")
print(f"    Test RMSE = {rmse:.3f} bpm")

# ─── 8. Plots ─────────────────────────────────────────────────────────────────
print("[8] Saving plots …")
fig, axes = plt.subplots(1, 3, figsize=(18, 5))

# Loss curves
axes[0].plot(history.history["loss"],     label="Train loss")
axes[0].plot(history.history["val_loss"], label="Val loss")
axes[0].set_title("MSE Loss"); axes[0].legend(); axes[0].set_xlabel("Epoch")

# MAE curves
axes[1].plot(history.history["mae"],     label="Train MAE")
axes[1].plot(history.history["val_mae"], label="Val MAE")
axes[1].set_title("MAE (normalised)"); axes[1].legend(); axes[1].set_xlabel("Epoch")

# Prediction vs ground truth (first 500 test samples)
n_show = min(500, len(y_true_bpm))
axes[2].plot(y_true_bpm[:n_show], label="True HR", alpha=0.7)
axes[2].plot(y_pred_bpm[:n_show], label="Predicted HR", alpha=0.7)
axes[2].set_title(f"Test Predictions (MAE={mae:.2f} bpm)")
axes[2].legend(); axes[2].set_xlabel("Sample"); axes[2].set_ylabel("bpm")

plt.tight_layout()
plt.savefig("training_plots.png", dpi=150)
print("    Saved training_plots.png")

# ─── 9. Convert to TFLite ─────────────────────────────────────────────────────
print("[9] Converting to TFLite …")
def convert_to_tflite(keras_model, quantize=False):
    converter = tf.lite.TFLiteConverter.from_keras_model(keras_model)
    converter.experimental_enable_resource_variables = True
    converter.target_spec.supported_ops = [
        tf.lite.OpsSet.TFLITE_BUILTINS,
        tf.lite.OpsSet.SELECT_TF_OPS
    ]
    converter._experimental_lower_tensor_list_ops = False
    if quantize:
        converter.optimizations = [tf.lite.Optimize.DEFAULT]

    return converter.convert()

model.save("accurate_hr_lstm_v2.h5")
tflite_path = "accurate_hr_lstm_v2.tflite"
with open(tflite_path, "wb") as f:
    f.write(convert_to_tflite(model))

with open("accurate_hr_lstm_quant_v2.tflite", "wb") as f:
    f.write(convert_to_tflite(model, quantize=True))

print("✅ Models exported successfully!")

# ─── 10. Verify TFLite model ──────────────────────────────────────────────────
print("[10] Verifying TFLite model …")
interpreter = tf.lite.Interpreter(model_path=tflite_path)
interpreter.allocate_tensors()
inp_details  = interpreter.get_input_details()
out_details  = interpreter.get_output_details()
print(f"    Input  shape : {inp_details[0]['shape']}")
print(f"    Output shape : {out_details[0]['shape']}")

# Run one sample
test_input = X_test[0:1].astype(np.float32)
interpreter.set_tensor(inp_details[0]["index"], test_input)
interpreter.invoke()
tflite_out = interpreter.get_tensor(out_details[0]["index"])[0][0]
keras_out  = model.predict(test_input, verbose=0)[0][0]
print(f"    Keras  pred : {keras_out * (y_max-y_min) + y_min:.2f} bpm")
print(f"    TFLite pred : {tflite_out * (y_max-y_min) + y_min:.2f} bpm")
print(f"    Delta       : {abs(keras_out - tflite_out) * (y_max-y_min):.4f} bpm  ✓")

# ─── 11. Export model.h ───────────────────────────────────────────────────────
print("[11] Writing model.h …")
def bytes_to_c_array(data: bytes, var_name: str) -> str:
    hex_vals = ", ".join(f"0x{b:02x}" for b in data)
    return (
        f"// Auto-generated by train_lstm.py — do not edit manually\n"
        f"// TFLite model: {var_name}  ({len(data)} bytes)\n\n"
        f"#pragma once\n\n"
        f"alignas(8) const unsigned char {var_name}[] = {{\n  "
        + ",\n  ".join(
            ", ".join(f"0x{b:02x}" for b in data[i:i+12])
            for i in range(0, len(data), 12)
        )
        + f"\n}};\n\n"
        f"const unsigned int {var_name}_len = {len(data)};\n"
    )
# Convert model
tflite_model = convert_to_tflite(model)

# Save .tflite
with open("model.tflite", "wb") as f:
    f.write(tflite_model)

# Export to C array
with open("model.h", "w") as f:
    f.write(bytes_to_c_array(tflite_model, "heart_rate_lstm_tflite"))
    
print(f"    Saved model.h  ({len(tflite_model)} bytes)")

# ─── 12. Export scaler_params.h ───────────────────────────────────────────────
print("[12] Writing scaler_params.h …")
feat_min = scaler.data_min_
feat_max = scaler.data_max_

header = f"""\
// Auto-generated by train_lstm.py — do not edit manually
// Apply these before feeding raw sensor data into the LSTM input buffer.
//
// Normalised value = (raw - FEAT_MIN[i]) / (FEAT_MAX[i] - FEAT_MIN[i])
// Denormalised HR  = predicted * (HR_MAX - HR_MIN) + HR_MIN

#pragma once

// Feature order: [Acc_X, Acc_Y, Acc_Z, PPG_Raw]
constexpr float FEAT_MIN[4] = {{
  {feat_min[0]:.6f}f,  // Acc_X
  {feat_min[1]:.6f}f,  // Acc_Y
  {feat_min[2]:.6f}f,  // Acc_Z
  {feat_min[3]:.6f}f   // PPG_Raw
}};

constexpr float FEAT_MAX[4] = {{
  {feat_max[0]:.6f}f,  // Acc_X
  {feat_max[1]:.6f}f,  // Acc_Y
  {feat_max[2]:.6f}f,  // Acc_Z
  {feat_max[3]:.6f}f   // PPG_Raw
}};

// Heart-rate denormalisation
constexpr float HR_MIN = {y_min:.1f}f;
constexpr float HR_MAX = {y_max:.1f}f;

// Inline helper — normalise one feature value
inline float normalise(float raw, int feat_idx) {{
  float range = FEAT_MAX[feat_idx] - FEAT_MIN[feat_idx];
  if (range < 1e-6f) return 0.0f;
  return (raw - FEAT_MIN[feat_idx]) / range;
}}

// Inline helper — convert raw LSTM output [0,1] back to bpm
inline float denormalise_hr(float pred) {{
  return pred * (HR_MAX - HR_MIN) + HR_MIN;
}}
"""

with open("scaler_params.h", "w") as f:
    f.write(header)
print("    Saved scaler_params.h")

print("\n✅  Done!")
print(f"    Test MAE  = {mae:.3f} bpm")
print(f"    Test RMSE = {rmse:.3f} bpm")
print("\n    Files written:")
print("      model.h          → copy to your bfit/ Arduino sketch folder")
print("      scaler_params.h  → copy to your bfit/ Arduino sketch folder")
print("      training_plots.png")