#ifndef OTDR_HARDWARE_H
#define OTDR_HARDWARE_H

#include <stdint.h>

/*
 * Convert a requested measurement range to a capture size in bytes.
 * The returned sample count is aligned to the FPGA three-sample packing.
 */
uint32_t otdr_hardware_capture_size_for_range(uint32_t range_m);

/*
 * Align an explicit sample count to the FPGA packing requirement and
 * convert it to a byte count for the 32-bit DMA stream.
 */
uint32_t otdr_hardware_capture_size_for_samples(uint32_t sample_count);

/*
 * Configure the optical channel and the number of FPGA accumulation cycles.
 */
void otdr_hardware_configure_frontend(uint32_t channel,
                                      uint32_t hardware_acc_count);

/*
 * Program the pulse generator and issue all triggers for one DMA frame.
 */
void otdr_hardware_trigger_capture(uint32_t capture_bytes,
                                   uint32_t adc_delay,
                                   uint32_t wave_type,
                                   uint32_t pulse_width_ns,
                                   uint32_t downsample_enable,
                                   uint32_t hardware_acc_count);

#endif
