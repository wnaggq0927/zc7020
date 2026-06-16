#ifndef OTDR_REFERENCE_H
#define OTDR_REFERENCE_H

/*
 * Select the active Golay code and derive samples per symbol from the
 * configured sample rate and pulse width.
 */
void otdr_reference_select(int wave_type, int pulse_width_ns);

const char *otdr_reference_code(void);
const char *otdr_reference_ap_code(void);
int otdr_reference_length(void);
int otdr_reference_sps(void);

#endif
