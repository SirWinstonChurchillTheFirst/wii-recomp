// GXAbortFrame's FIFO reset, through the real gx_fifo.c.
#include "../../recomp/runtime/gx_fifo.c"
#include "../../recomp/runtime/timeline.c"
#include "../harness.h"
#include "fifo_stubs.h"
static int cp_irq;
void raise_irq(int b) { if (b == IRQ_CP) cp_irq = 1; }
void clear_irq(int b) { if (b == IRQ_CP) cp_irq = 0; }
void gap_hit(const char *i, uint32_t d) {}
uint32_t gx_vertices_size(int f, int c) { return 0; }
int gx_read_vertices(const uint8_t *d, int f, int c, Vertex *o) { return c; }
int main(void) {
  memory_start();
  hw_poke(CP_CONTROL, CONTROL_READ_ENABLE | CONTROL_INTERRUPT_ENABLE | CONTROL_LINK, 2);
  graphics_write(PIPE_FIRST, 0x61000000u, 4);  // Half a burst, not yet sent.
  CHECK_EQ(burst_used, 4);
  fifo_flags = CP_STATUS_OVERFLOW;
  cp_irq = 1;
  CHECK(graphics_write(PI_FIFO_RESET, 0, 4));  // Only the reset bit acts.
  CHECK_EQ(burst_used, 4);
  CHECK(graphics_write(PI_FIFO_RESET, 1, 4));
  CHECK_EQ(burst_used, 0);
  CHECK_EQ(stream_used, 0);
  CHECK_EQ(fifo_flags, 0);
  CHECK(!cp_irq);
  CHECK_EQ(hw_peek(CP_CONTROL, 2), CONTROL_LINK);  // Linked, with reads and interrupts off.
  CHECK_EQ(read_pair(CP_HIGH_MARK), 0x1fffffe0u);
  CHECK_EQ(read_pair(CP_LOW_MARK), 0);
  return finish();
}
