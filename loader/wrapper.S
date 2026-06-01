.section .data
.globl _caller_return
_caller_return:
	.long 0
.globl _report_entry
_report_entry:
	.long _null_call
.globl _report_ret
_report_ret:	
	.long _null_call
.global _wrapper_target
_wrapper_target:	
	.long _null_call

.section .text
.globl _null_call
	.balign 16,0x90
_null_call:
	ret
.globl _wrapper
	.balign 16,0x90
_wrapper:
	pusha			# store registers (EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI) 
	pushf			# store flags
	
	push %ebp		# set up a stack frame 
	movl %esp, %ebp

	leal 4(%ebp), %eax	# push flags addr
	push %eax
	leal 8(%ebp), %eax	# push registers addr
	push %eax
	
	leal 40(%ebp), %edx	
	movl (%ebp), %eax
	subl %edx, %eax
	push %eax
	push %edx
	
	call *_report_entry	# report entry
	
	test %eax, %eax
	jnz .Ldone

	leave			# restore %esp, %ebp
	popf			# restore flags
	popa			# restore registers
	
	popl _caller_return	# switch return addresses
	pushl $.Lwrapper_return 
	
	jmp *_wrapper_target	# wrapper_target should return at .Lwrapper_return

	.balign 16, 0x90
.Lwrapper_return:	
	pushl _caller_return	# restore the original return address
	pusha			# more for reference sake here
	pushf
	
	push %ebp		# set up a stack frame
	movl %esp, %ebp

	leal 4(%ebp), %eax	# push flags addr
	push %eax
	leal 8(%ebp), %eax	# push registers addr
	push %eax

	leal 40(%ebp), %edx	# push stack top address (relative to our entry)
	movl (%ebp), %eax
	subl %edx, %eax		# calculate difference between entry and previous frame
	push %eax
	push %edx

	call *_report_ret	# report the return information (same args)
.Ldone:	

	leave
	popf
	popa
	ret
