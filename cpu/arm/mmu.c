#include <jicama/system.h>
#include <jicama/process.h>
#include "./s3c4510/s3c4510.h"

#define	SDRAM_SIZE		0x04000000		/*64M*/
#define 	SDRAM_BASE		0x30000000

/*以下为虚拟地址*/ 
#define	MMU_TABLE_BASE	SDRAM_BASE
#define	PROCESS0_BASE	SDRAM_BASE+0x4000
#define 	VECTORS_BASE	0xffff0000
#define	VECTORS_PHY_BASE	SDRAM_BASE+SDRAM_SIZE-0x100000


/*虚拟地址SDRAM_RAW_RW_VA_BASE + i*0x100000指向PID=i的进程的1M内存，并且此虚拟地址>32M，所以与PID无关*/
/*SDRAM raw read/write vitual address base*/
#define	SDRAM_RAW_RW_VA_BASE	((VECTORS_BASE & 0xfff00000)-SDRAM_SIZE)	


  

void mmu_tlb_init(void);
void mmu_init(void);


static unsigned long *mmu_tlb_base = (unsigned long *) MMU_TABLE_BASE;


/****************************************************************************
* 段页表项entry:[31:20]段基址，[11:10]为AP(控制访问权限)，[8:5]域，
*   [3:2]=CP(decide cached&buffered)，[1:0]=0b10-->页表项为段描述符
* MMU_SECDESC:
*   AP=0b11
*   DOMAIN=0
*   [1:0]=0b10--->页表项为段描述符
* MMU_CACHEABLE:
*   C=1(bit[3])
*1. 将页表放在SDRAM 开始处，即:MMU_TABLE_BASE = 0x3000000(in mmu.h)
*2. 对于64M SDRAM，其物理地址为0x30000000-0x33f00000，
令其虚拟地址=物理地址
*3. 对于SFR，其物理地址为0x48000000-0x60000000,
令其虚拟地址等于物理地址(请参考数据手册P192)
*4. exception vector:虚拟地址0xffff0000，物理地址0x33f000000
*5. 进程号为PID的进程空间块的虚拟地址为：
PID*0x02000000到PID*0x02000000+0x01ffffff
*6. 进程0物理地址：0x30000000-0x300fffff
*7. 进程1物理地址：0x30100000-0x301fffff
*8. 进程2物理地址：0x30200000-0x302fffff
*9. .... ....
*10.进程62物理地址：0x33e00000-0x33efffff
*
******************************************************************************/

void mmu_tlb_init()
{
	unsigned long idx;
	unsigned long kernel_addr = SDRAM_BASE;

	/* map space of kernel and process 0 */
	/* section table's entry:AP=0b11,domain=0,Cached,write-through mode(WT) */
	idx = SDRAM_BASE;
	*(mmu_tlb_base+(idx>>20)) 
		= kernel_addr |(0x03<<10)|(0<<5)|(1<<4)|(1<<3)|0x02;


	/*进程1-23,25-35,48-62*/
	for(idx = 1; idx < 24; idx++){
		/*section table's entry:AP=0b11,domain=0,Cached,write-through mode(WT)*/		
		*(mmu_tlb_base+((idx*0x02000000)>>20)) 
			= (idx*0x00100000+SDRAM_BASE) |(0x03<<10)|(0<<5)|(1<<4)|(1<<3)|0x02;
	}

	/*SFR*/
	for(idx = S3C4510_BASE_ADDR; idx < S3C4510_BASE_ADDR+0x1000000; idx += 0x100000){
		/*section table's entry:AP=0b11,domain=0,NCNB*/		
		*(mmu_tlb_base+(idx>>20)) 
			= idx |(0x03<<10)|(0<<5)|(1<<4)| 0x02;
	}
#if 0
	for(idx = 25; idx < 36; idx++){
		/*section table's entry:AP=0b11,domain=0,Cached,write-through mode(WT)*/		
		*(mmu_tlb_base+((idx*0x02000000)>>20)) 
			= (idx*0x00100000+SDRAM_BASE) |(0x03<<10)|(0<<5)|(1<<4)|(1<<3)|0x02;
	}

	for(idx = 48; idx < NR_PROC; idx++){
		/*section table's entry:AP=0b11,domain=0,Cached,write-through mode(WT)*/		
		*(mmu_tlb_base+((idx*0x02000000)>>20)) 
			= (idx*0x00100000+SDRAM_BASE) |(0x03<<10)|(0<<5)|(1<<4)|(1<<3)|0x02;
	}
#endif

	/*exception vector*/
	/*section table's entry:AP=0b11,domain=0,Cached,write-through mode(WT)*/			
	*(mmu_tlb_base+(0xffff0000>>20)) = (VECTORS_PHY_BASE)|(0x03<<10)|(0<<5)|(1<<4)|(1<<3)|0x02;

	/*SDRAM_RAW_RW_VA_BASE开始的64M虚拟地址*/
	/*大于32M，所以不需要经过PID转换即可访问物理内存*/
	/*用于在不同进程中从nand flash中复制代码到内存，
	以及从其他进程访问另一个进程的空间*/
	for(idx = SDRAM_RAW_RW_VA_BASE;
		idx < SDRAM_RAW_RW_VA_BASE + SDRAM_SIZE;
		idx += 0x100000){
		/*section table's entry:AP=0b11,domain=0,Cached,write-through mode(WT)*/		
		*(mmu_tlb_base+((idx)>>20))
			= (idx-SDRAM_RAW_RW_VA_BASE+SDRAM_BASE)|(0x03<<10)|(0<<5)|(1<<4)|(1<<3)|0x02; 
	}
}

/***************************************************************************
* 1.Invalidate I,D caches,drain write buffer,invalidate I,D TLBS
* 2.Load page table pointer
* 3.Write domain ID
* 4.Set MMU control registers(read-modify-write):
*	a.read: mrc p15, 0, r0, c1, c0, 0
*	b.modify:
*	       bit[13]=0,异常向量起始地址为0x00000000
*	       bit[12]=0,Instruction cache disable
*	       bit[9:8]=0b00,RS=0b00(因为页表中AP=0b11,所以RS的作用忽略)
*	       bit[7]=0,Little-endian operation
*	       bit[2]=0b000,Data cache disabel
*	       bit[1:0]=0b11,Data alignment checking enable,MMU enable
*	c.write: mcr p15, 0, r0, c1, c0, 0
***************************************************************************/
void mmu_init()
{
	unsigned long ttb = MMU_TABLE_BASE;
/*是否使用Cache*/
#define	 CONFIG_CPU_D_CACHE_ON	1
#define	 CONFIG_CPU_I_CACHE_ON	1
__asm__(
	"mov	r0, #0\n"	
	/* invalidate I,D caches on v4 */
	"mcr	p15, 0, r0, c7, c7, 0\n"	
	/* drain write buffer on v4 */
	"mcr	p15, 0, r0, c7, c10, 4\n"	
	/* invalidate I,D TLBs on v4 */
	"mcr	p15, 0, r0, c8, c7, 0\n"	
	/* Load page table pointer */
	"mov	r4, %0\n"
	"mcr	p15, 0, r4, c2, c0, 0\n"	
	/* Write domain id (cp15_r3) */
	"mvn	r0, #0\n"		/*0b11=Manager*/
	"mcr	p15, 0, r0, c3, c0, 0\n"
	/* Set control register v4 */
	"mrc	p15, 0, r0, c1, c0, 0\n"	
	/* Clear out 'unwanted' bits (then put them in if we need them) */
	"ldr 	r1, =0x1384\n"
	"bic	r0, r0, r1\n"	
	/* Turn on what we want */
	/*Base location of exceptions = 0xffff0000*/
	"orr	r0, r0, #0x2000\n"	
	/* Fault checking enabled */
	"orr	r0, r0, #0x0002\n"
	/* MMU enabled */
	//"orr	r0, r0, #0x0001\n"	
	/* write control register *//*write control register P545*/
	"mcr	p15, 0, r0, c1, c0, 0\n"
	: /* no outputs */
	: "r" (ttb)
	: "r0","r1","r4");	
	flush_tlb();
}



int dump_pages(char *buf, int len)
{
	return 0;
}


int free_page(pte_t p)
{
	mm_free(p, PAGE_SIZE);
}

unsigned long get_page()
{
#if 1
static char pagex[10][4096];
	static int idx = 0;
	if (idx>4)
	{
		panic("no pages!");
	}
	return (unsigned long )pagex[idx++];//
#else
	return mm_malloc(PAGE_SIZE);
#endif
}

/* CP15 Flush Instruction TLB & Data TLB */ 
inline void flush_tlb()
{
    __asm__ __volatile__
	("mcr     p15, 0, r0, c8, c7, 0");
}


inline void flush_tlbent(u32_t addr)
{
    __asm__ __volatile__
	("mcr     p15, 0, r0, c8, c7, 0");
}



int mem_writeable(void* address,int size)
{
	return 0;
}


void copy_vector()
{
	#define _TEXT_BASE 0x30004000
	//memcpy((void*)0, (void*)_TEXT_BASE, 32);
	//memcpy((unsigned char*)(VECTORS_PHY_BASE+0xf0000), 
	//	(void*)_TEXT_BASE, 512);
}

void copy_vectors()
{
	#define _TEXT_BASE 0x30004000
	//memcpy((void*)0, (void*)_TEXT_BASE, 32);
	memcpy((unsigned char*)(VECTORS_PHY_BASE+0xf0000), 
		(void*)_TEXT_BASE, 512);
}

