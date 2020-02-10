#
# Makefile for the Linux kernel device drivers.
#
# 15 Sep 2000, Christoph Hellwig <hch@infradead.org>
# Rewritten to use lists instead of if-statements.
#

obj-y				+= simplefs/
obj-y				+= mm_spinlock/
#Hook Just Only For X86
#obj-y				+= hook/
obj-y				+= globalfifo/
obj-y				+= mm/
