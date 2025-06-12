.text
    .align 2
    .global rp2040_syscall
rp2040_syscall:
svc #0
bx lr
nop