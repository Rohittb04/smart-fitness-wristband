#pragma once

// Call once in setup() — loads model, allocates tensors
void initInference();

// FreeRTOS task — runs LSTM inference every second
void InferenceTask(void *pvParameters);
