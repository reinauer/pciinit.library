#ifndef PCIINIT_BOING_H
#define PCIINIT_BOING_H
#include <libraries/pciinit.h>
struct BoingBall
{
    uint32_t width, height, radius;
    int32_t *latitude, *longitude, *inverse;
};
/* Prepare before mapping the framebuffer; only integer work occurs in draw.
 * Initialize the structure to zero, then release it after use. */
int boing_prepare(struct BoingBall *ball, uint32_t width, uint32_t height);
void boing_release(struct BoingBall *ball);
int boing_draw(const struct BoingBall *ball, const struct PCIInitFramebuffer *fb, uint32_t phase);
#endif
