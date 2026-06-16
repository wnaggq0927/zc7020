#ifndef OTDR_EVENT_H
#define OTDR_EVENT_H

#include "otdr_protocol.h"

#include <stdint.h>

#define OTDR_MAX_EVENTS 16U

uint32_t otdr_event_analyze(const float *curve,
                            uint32_t point_count,
                            otdr_event_point_t *events,
                            uint32_t event_capacity);

#endif
