/*
 * initcall.lds - initcall/pumpcall 段收集 (增补式链接脚本)
 *
 * 用法: 链接时加 -Wl,-T,initcall.lds
 * INSERT 增补默认链接脚本 (不替换), 与内核 initcall 布局一致:
 *   .initcallN.init 按层序拼接成连续数组, 边界符号供 initcall.c 迭代。
 */
SECTIONS
{
	.initcall : ALIGN(4)
	{
		__ic_start = .;
		__ic_l0 = .;		/* EARLY  */
		KEEP(*(.initcall0.init))
		__ic_l1 = .;		/* SAL    */
		KEEP(*(.initcall1.init))
		__ic_l2 = .;		/* DRIVER */
		KEEP(*(.initcall2.init))
		__ic_l3 = .;		/* CORE   */
		KEEP(*(.initcall3.init))
		__ic_l4 = .;		/* APP    */
		KEEP(*(.initcall4.init))
		__ic_l5 = .;		/* LATE   */
		KEEP(*(.initcall5.init))
		__ic_stop = .;
	}

	.pumpcalls : ALIGN(4)
	{
		__pc_start = .;
		KEEP(*(.pumpcalls))
		__pc_stop = .;
	}
}
INSERT AFTER .rodata;
