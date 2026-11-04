/* main.c - SegaTube on the master SH-2.
 *
 * Marker strings below are checked by tests/verify_rom.py: if the linker
 * ever drops the player, the ROM fails the build instead of booting black.
 */
#include "hw.h"
#include "audio.h"
#include "player.h"

const char segatube_marker[] __attribute__((used)) = "SEGATUBE PLAYER";

int main(void)
{
    (void)*(const volatile char *)segatube_marker;     /* keep it linked */
    hw_init();
    hw_stage(0xC);                  /* running */
    audio_init(22050);
    library_run();
    return 0;
}
