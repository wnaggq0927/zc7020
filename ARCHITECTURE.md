# OTDR Vitis Source Architecture

## Runtime flow

```text
main
  -> network polling
  -> watchdog polling
  -> measurement engine
       -> DMA capture
       -> software accumulation
       -> mode processing
       -> DSP correlation
       -> protocol upload
```

## Module ownership

- `main.c`
  - Hardware and network initialization.
  - Top-level polling loop only.

- `otdr_measurement.c`
  - Single-pulse and Golay measurement state.
  - DMA capture sequencing and timeout recovery.
  - Software accumulation and averaging.
  - AP/AN and BP/BN differential preparation.
  - DSP invocation and result upload selection.

- `otdr_hardware.c`
  - FPGA GPIO register programming.
  - Pulse and acquisition trigger sequencing.
  - Capture-size alignment for the three-sample hardware packing.
  - Optical channel and hardware accumulation configuration.

- `otdr_config.c`
  - Active measurement configuration and defaults.
  - Start-command compatibility parsing.
  - Capture-size selection from range or explicit point count.
  - Read-only configuration access for other modules.

- `otdr_upload.c`
  - Upload frame construction and measurement metadata.
  - int32, float32, reference, and official curve serialization.

- OTDR event analysis runs on the Python host.
  - Vitis uploads measurement parameters and curve data only.
  - The protocol event count remains zero for host compatibility.

- `otdr_ota.c`
  - OTA transfer state.
  - QSPI update execution and reboot sequence.

- `otdr_dsp.c`
  - FFT-based correlation.
  - DSP self-test.

- `otdr_reference.c`
  - Golay AP/AN/BP/BN code ownership.
  - Pulse-width to samples-per-symbol conversion.
  - Active reference selection.

- `otdr_protocol.c`
  - Host command parsing.
  - Command dispatch and response state.

- `otdr_tcp_server.c`
  - UDP transport.
  - Client endpoint tracking.
  - Packet slicing and watchdog handling.

- `dma.c`
  - AXI DMA initialization, transfer control, and interrupt state.

## Measurement data flow

### Single pulse

```text
DMA int32 samples
  -> software accumulation
  -> divide by hardware_acc * software_acc
  -> float32 upload
```

### Golay

```text
AP / AN / BP / BN DMA int32 frames
  -> software accumulation
  -> convert to float and average
  -> AP - AN and BP - BN
  -> tail DC removal
  -> correlation A and correlation B
  -> (correlation A + correlation B) / 2
  -> float32 upload
```

## Refactoring rules

1. Preserve protocol fields and numerical behavior during structural changes.
2. Keep hardware register access out of DSP modules.
3. Keep network framing out of measurement algorithms.
4. Use explicit point counts and data types at every module boundary.
5. Compile and link after each extraction step.
6. Compare output data against `stable-before-refactor` before changing algorithms.
7. Only `otdr_config.c` may modify active measurement configuration.
