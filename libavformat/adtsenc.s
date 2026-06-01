	.file	"adtsenc.c"
	.section .text
	.p2align 4
_adts_write_header:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%ebx
	movl	8(%ebp), %edx
	movl	96(%edx), %eax
	movl	8(%eax), %eax
	movl	28(%eax), %ecx
	testl	%ecx, %ecx
	jle	L2
	movl	24(%eax), %ebx
	movl	12(%edx), %edx
	movl	(%ebx), %eax
/APP
# 60 "../libavutil/bswap.h" 1
	bswap   %eax
# 0 "" 2
/NO_APP
	movl	%eax, %ecx
/APP
# 66 "../libavcodec/bitstream.h" 1
	shrl $-5, %ecx
	
# 0 "" 2
/NO_APP
	sall	$5, %eax
	decl	%ecx
/APP
# 66 "../libavcodec/bitstream.h" 1
	shrl $-4, %eax
	
# 0 "" 2
/NO_APP
	movl	%ecx, 4(%edx)
	movl	%eax, 8(%edx)
	movl	1(%ebx), %eax
	movl	$1, (%edx)
/APP
# 60 "../libavutil/bswap.h" 1
	bswap   %eax
# 0 "" 2
/NO_APP
	addl	%eax, %eax
/APP
# 66 "../libavcodec/bitstream.h" 1
	shrl $-4, %eax
	
# 0 "" 2
/NO_APP
	movl	%eax, 12(%edx)
L2:
	popl	%ebx
	xorl	%eax, %eax
	popl	%ebp
	ret
	.p2align 4
_adts_write_packet:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%esi
	pushl	%ebx
	subl	$16, %esp
	movl	12(%ebp), %ecx
	movl	20(%ecx), %eax
	testl	%eax, %eax
	je	L6
	movl	8(%ebp), %ebx
	addl	$16, %ebx
	movl	-4(%ebx), %esi
	movl	(%esi), %edx
	testl	%edx, %edx
	jne	L14
L7:
	pushl	%edx
	pushl	%eax
	movl	16(%ecx), %ecx
	pushl	%ecx
	pushl	%ebx
	call	_put_buffer
	movl	%ebx, (%esp)
	call	_put_flush_packet
	addl	$16, %esp
L6:
	leal	-8(%ebp), %esp
	xorl	%eax, %eax
	popl	%ebx
	popl	%esi
	popl	%ebp
	ret
	.p2align 4,,7
	.p2align 3
L14:
	movl	4(%esi), %edx
	movl	8(%esi), %ecx
	sall	$4, %edx
	addl	$7, %eax
	orl	%ecx, %edx
	movl	12(%esi), %ecx
	sall	$4, %edx
	movl	%eax, %esi
	shrl	$11, %esi
	orl	$67093504, %edx
	sall	$21, %eax
	orl	%ecx, %edx
	sall	$6, %edx
	orl	$2096128, %eax
	orl	%esi, %edx
	leal	-12(%ebp), %esi
/APP
# 60 "../libavutil/bswap.h" 1
	bswap   %edx
# 0 "" 2
/NO_APP
	movl	%edx, -15(%ebp)
	leal	-15(%ebp), %edx
	.p2align 4,,7
	.p2align 3
L8:
	movl	%eax, %ecx
	incl	%edx
	shrl	$24, %ecx
	sall	$8, %eax
	movb	%cl, 3(%edx)
	cmpl	%esi, %edx
	jne	L8
	pushl	%esi
	leal	-15(%ebp), %eax
	pushl	$7
	pushl	%eax
	pushl	%ebx
	call	_put_buffer
	movl	12(%ebp), %ecx
	addl	$16, %esp
	movl	20(%ecx), %eax
	jmp	L7
	.globl	_adts_muxer
LC0:
	.ascii "adts\0"
LC1:
	.ascii "ADTS AAC\0"
LC2:
	.ascii "audio/aac\0"
LC3:
	.ascii "aac\0"
	.section .data
	.p2align 5
_adts_muxer:
	.long	LC0
	.long	LC1
	.long	LC2
	.long	LC3
	.long	16
	.long	86018
	.long	0
	.long	_adts_write_header
	.long	_adts_write_packet
	.space 28
	.ident	"GCC: (DJGPP 2.05) 15.2.0"
