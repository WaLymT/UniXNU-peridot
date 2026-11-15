// Minimal self-contained arm64 Mach-O "launchd": validates a few syscalls against
// /dev/console, then stays resident as init.
//   open("/dev/console", O_WRONLY)
//   getpid(), write() of "PERIDOT-USERSPACE pid=0x... fd=0x..."
// Scratch space comes from the user stack the kernel sets up: __TEXT is read-only.
.text
.globl _start
.align 4
_start:
    sub  sp, sp, #64             // scratch hex buffer (writable)
    mov  x21, sp

    adr  x0, console
    mov  w1, #1                  // O_WRONLY
    mov  w2, #0
    mov  x16, #5                 // SYS_open
    svc  #0x80
    b.cs 1f                      // carry set = errno in x0
    mov  x19, x0
    b    2f
1:  mov  x19, #-1
2:
    mov  x16, #20                // SYS_getpid
    svc  #0x80
    mov  x20, x0

    adr  x1, msg_head
    mov  x2, #18                 // "PERIDOT-USERSPACE"
    bl   emit
    adr  x1, msg_pid
    mov  x2, #6                  // " pid=0x"
    bl   emit
    mov  x0, x20
    bl   emit_hex
    adr  x1, msg_fd
    mov  x2, #6                  // " fd=0x"
    bl   emit
    mov  x0, x19
    bl   emit_hex
    adr  x1, msg_nl
    mov  x2, #1
    bl   emit

hang:
    b    hang

// emit: write(x1, x2) to the console fd (x19); fd 0 is the console too
emit:
    mov  x0, x19
    cmp  x0, #0
    b.ge 3f
    mov  x0, #1
3:  mov  x16, #4                 // SYS_write
    svc  #0x80
    ret

// emit_hex: write 16 hex digits of x0 using the stack buffer in x21
emit_hex:
    mov  x10, x21
    mov  x11, #60
4:  lsr  x12, x0, x11
    and  x12, x12, #0xf
    cmp  x12, #10
    b.lt 5f
    add  x12, x12, #87           // 'a' - 10
    b    6f
5:  add  x12, x12, #48           // '0'
6:  strb w12, [x10], #1
    cmp  x11, #0
    b.eq 7f
    sub  x11, x11, #4
    b    4b
7:  mov  x1, x21
    mov  x2, #16
    b    emit

.align 3
console:
    .asciz "/dev/console"
msg_head:
    .ascii "PERIDOT-USERSPACE"
msg_pid:
    .ascii " pid=0x"
msg_fd:
    .ascii " fd=0x"
msg_nl:
    .ascii "\n"
