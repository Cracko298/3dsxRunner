.syntax unified
.arm
.section .text.ui_music_gain11, "ax"
.balign 4
.global ui_music_gain11
.type ui_music_gain11, %function
ui_music_gain11:
    ldr r1, =0x10103424
    str r0, [r1]
    mcr p15, 0, r0, c7, c10, 4
    bx lr
.size ui_music_gain11, .-ui_music_gain11
.ltorg
