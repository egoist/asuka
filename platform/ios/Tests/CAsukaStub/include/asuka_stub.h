// The core's functions as the host's tests see them: no core, the events
// the host sends kept for the tests to read.
#include <stddef.h>
#include <stdint.h>

// The events sent since the last reset, each a u32 length (little
// endian) then the event's bytes.
size_t asuka_stub_events(const uint8_t **out);
void asuka_stub_reset(void);
