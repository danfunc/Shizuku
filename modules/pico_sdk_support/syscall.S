.text
.cpu cortex-m0plus
.thumb
.global rp2040_syscall
.type   rp2040_syscall, %function
rp2040_syscall:
svc #0
bx lr
nop