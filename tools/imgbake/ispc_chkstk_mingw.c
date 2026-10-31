/* ISPC's Windows objects call __chkstk, the stack probe of Microsoft's C
 * runtime, which MinGW doesn't have. MinGW's libgcc has the same probe under
 * the name ___chkstk_ms: it probes the number of bytes in rax, keeps every
 * register and doesn't move the stack pointer. */

__asm__(".text\n"
		".globl __chkstk\n"
		"__chkstk:\n"
		"\tjmp ___chkstk_ms\n");
