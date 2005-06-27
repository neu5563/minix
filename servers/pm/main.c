/* This file contains the main program of the process manager and some related
 * procedures.  When MINIX starts up, the kernel runs for a little while,
 * initializing itself and its tasks, and then it runs PM and FS.  Both PM
 * and FS initialize themselves as far as they can. PM asks the kernel for
 * all free memory and starts serving requests.
 *
 * The entry points into this file are:
 *   main:	starts PM running
 *   setreply:	set the reply to be sent to process making an PM system call
 */

#include "pm.h"
#include <minix/utils.h>
#include <minix/keymap.h>
#include <minix/callnr.h>
#include <minix/com.h>
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioc_memory.h>
#include <string.h>
#include "mproc.h"
#include "param.h"

#include "../../kernel/const.h"
#include "../../kernel/type.h"

FORWARD _PROTOTYPE( void get_work, (void)				);
FORWARD _PROTOTYPE( void pm_init, (void)				);
FORWARD _PROTOTYPE( void get_mem_chunks, (struct memory *mem_chunks) 	);
FORWARD _PROTOTYPE( void patch_mem_chunks, (struct memory *mem_chunks, 
	struct mem_map *map_ptr) 	);

#define click_to_round_k(n) \
	((unsigned) ((((unsigned long) (n) << CLICK_SHIFT) + 512) / 1024))

/*===========================================================================*
 *				main					     *
 *===========================================================================*/
PUBLIC void main()
{
/* Main routine of the process manager. */

  int result, s, proc_nr;
  struct mproc *rmp;

  pm_init();			/* initialize process manager tables */

  /* This is PM's main loop-  get work and do it, forever and forever. */
  while (TRUE) {
	get_work();		/* wait for an PM system call */

	/* Check for system notifications first. Special cases. */
	if (call_nr == HARD_STOP) {		/* MINIX is shutting down */
		check_sig(-1, SIGKILL);		/* kill all processes */
		sys_exit(0);
		/* never reached */
	} else if (call_nr == KSIG_PENDING) {	/* signals pending */
		(void) ksig_pending();
		result = SUSPEND;		/* don't reply */
	}
	/* Else, if the system call number is valid, perform the call. */
	else if ((unsigned) call_nr >= NCALLS) {
		result = ENOSYS;
	} else {
		result = (*call_vec[call_nr])();
	}

	/* Send the results back to the user to indicate completion. */
	if (result != SUSPEND) setreply(who, result);

	swap_in();		/* maybe a process can be swapped in? */

	/* Send out all pending reply messages, including the answer to
	 * the call just made above.  The processes must not be swapped out.
	 */
	for (proc_nr=0, rmp=mproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if ((rmp->mp_flags & (REPLY | ONSWAP)) == REPLY) {
			if ((s=send(proc_nr, &rmp->mp_reply)) != OK) {
				panic(__FILE__,"PM can't reply to", proc_nr);
			}
			rmp->mp_flags &= ~REPLY;
		}
	}
  }
}


/*===========================================================================*
 *				get_work				     *
 *===========================================================================*/
PRIVATE void get_work()
{
/* Wait for the next message and extract useful information from it. */
  if (receive(ANY, &m_in) != OK) panic(__FILE__,"PM receive error", NO_NUM);
  who = m_in.m_source;		/* who sent the message */
  call_nr = m_in.m_type;	/* system call number */

  /* Process slot of caller. Misuse PM's own process slot if the kernel is
   * calling. The can happen in case of pending kernel signals.
   */
  mp = &mproc[who < 0 ? PM_PROC_NR : who];
}


/*===========================================================================*
 *				setreply				     *
 *===========================================================================*/
PUBLIC void setreply(proc_nr, result)
int proc_nr;			/* process to reply to */
int result;			/* result of call (usually OK or error #) */
{
/* Fill in a reply message to be sent later to a user process.  System calls
 * may occasionally fill in other fields, this is only for the main return
 * value, and for setting the "must send reply" flag.
 */
  register struct mproc *rmp = &mproc[proc_nr];

  rmp->mp_reply.reply_res = result;
  rmp->mp_flags |= REPLY;	/* reply pending */

  if (rmp->mp_flags & ONSWAP)
	swap_inqueue(rmp);	/* must swap this process back in */
}


/*===========================================================================*
 *				pm_init					     *
 *===========================================================================*/
PRIVATE void pm_init()
{
/* Initialize the process manager. */
  int key, i, s;
  static struct system_image image[IMAGE_SIZE];
  register struct system_image *ip;
  static char core_sigs[] = { SIGQUIT, SIGILL, SIGTRAP, SIGABRT,
			SIGEMT, SIGFPE, SIGUSR1, SIGSEGV, SIGUSR2 };
  static char ign_sigs[] = { SIGCHLD };
  register int proc_nr;
  register struct mproc *rmp;
  register char *sig_ptr;
  phys_clicks total_clicks, minix_clicks, free_clicks;
  message mess;
  struct mem_map mem_map[NR_LOCAL_SEGS];
  struct memory mem_chunks[NR_MEMS];

  /* Build the set of signals which cause core dumps, and the set of signals
   * that are by default ignored.
   */
  sigemptyset(&core_sset);
  for (sig_ptr = core_sigs; sig_ptr < core_sigs+sizeof(core_sigs); sig_ptr++)
	sigaddset(&core_sset, *sig_ptr);
  sigemptyset(&ign_sset);
  for (sig_ptr = ign_sigs; sig_ptr < ign_sigs+sizeof(ign_sigs); sig_ptr++)
	sigaddset(&ign_sset, *sig_ptr);

  /* Obtain a copy of the boot monitor parameters and the kernel info struct.  
   * Parse the list of free memory chunks. This list is what the boot monitor 
   * reported, but it must be corrected for the kernel and system processes.
   */
  if ((s=sys_getmonparams(monitor_params, sizeof(monitor_params))) != OK)
      panic(__FILE__,"get monitor params failed",s);
  if ((s=sys_getkinfo(&kinfo)) != OK)
      panic(__FILE__,"get kernel info failed",s);
  get_mem_chunks(mem_chunks);

  /* Get the memory map of the kernel to see how much memory it uses. */
  if ((s=get_mem_map(SYSTASK, mem_map)) != OK)
  	panic(__FILE__,"PM couldn't get memory map of SYSTASK",s);
  minix_clicks = (mem_map[S].mem_phys+mem_map[S].mem_len)-mem_map[T].mem_phys;
  patch_mem_chunks(mem_chunks, mem_map);

  /* Initialize PM's process table. Request a copy of the system image table 
   * that is defined at the kernel level to see which slots to fill in.
   */
  if (OK != (s=sys_getimage(image))) 
  	panic(__FILE__,"PM: warning, couldn't get image table: %d\n", s);
  procs_in_use = 0;				/* start populating table */
  for (ip = &image[0]; ip < &image[IMAGE_SIZE]; ip++) {		
  	if (ip->proc_nr >= 0) {			/* task have negative nrs */
  		procs_in_use += 1;		/* found user process */

		/* Set process details found in the image table. */
		rmp = &mproc[ip->proc_nr];	
		rmp->mp_flags |= IN_USE | DONT_SWAP; 
  		rmp->mp_pid = get_free_pid();
		rmp->mp_parent = INIT_PROC_NR;
  		strncpy(rmp->mp_name, ip->proc_name, PROC_NAME_LEN); 
		sigfillset(&rmp->mp_ignore);
  		sigfillset(&rmp->mp_sigmask);
  		sigemptyset(&rmp->mp_catch);

  		/* Get memory map for this process from the kernel. */
		if ((s=get_mem_map(ip->proc_nr, rmp->mp_seg)) != OK)
  			panic(__FILE__,"couldn't get process entry",s);
		if (rmp->mp_seg[T].mem_len != 0) rmp->mp_flags |= SEPARATE;
		minix_clicks += rmp->mp_seg[S].mem_phys + 
			rmp->mp_seg[S].mem_len - rmp->mp_seg[T].mem_phys;
  		patch_mem_chunks(mem_chunks, rmp->mp_seg);

		/* Tell FS about this system process. */
		mess.PR_PROC_NR = ip->proc_nr;
		mess.PR_PID = rmp->mp_pid;
  		if (OK != (s=send(FS_PROC_NR, &mess)))
			panic(__FILE__,"PM can't sync up with FS", s);
  	}
  }

  /* PM and INIT are somewhat special. Override some details. Set signal
   * handling behaviour for PM, since PM cannot call sigaction() as others.
   */
  mproc[INIT_PROC_NR].mp_pid = INIT_PID;
  mproc[INIT_PROC_NR].mp_parent = PM_PROC_NR;
  sigemptyset(&mproc[INIT_PROC_NR].mp_ignore);
  sigemptyset(&mproc[INIT_PROC_NR].mp_sigmask);

  mproc[PM_PROC_NR].mp_pid = PM_PID;
  mproc[PM_PROC_NR].mp_parent = PM_PARENT;
  sigfillset(&mproc[PM_PROC_NR].mp_ignore);
  sigfillset(&mproc[PM_PROC_NR].mp_sigmask);

  /* Tell FS that no more system processes follow and synchronize. */
  mess.PR_PROC_NR = NONE;
  if (sendrec(FS_PROC_NR, &mess) != OK || mess.m_type != OK)
	panic(__FILE__,"PM can't sync up with FS", NO_NUM);

  /* Possibly we must correct the memory chunks for the boot device. */
  if (kinfo.bootdev_size > 0) {
      mem_map[T].mem_phys = kinfo.bootdev_base >> CLICK_SHIFT;
      mem_map[T].mem_len = 0;
      mem_map[D].mem_len = (kinfo.bootdev_size+CLICK_SIZE-1) >> CLICK_SHIFT;
      patch_mem_chunks(mem_chunks, mem_map);
  }

  /* Initialize tables to all physical memory and print memory information. */
  mem_init(mem_chunks, &free_clicks);
  total_clicks = minix_clicks + free_clicks;
  printf("Memory size=%uK   ", click_to_round_k(total_clicks));
  printf("System services=%uK   ", click_to_round_k(minix_clicks));
  printf("Available=%uK\n\n", click_to_round_k(free_clicks));
}


/* In real mode only 1M can be addressed, and in 16-bit protected we can go
 * no further than we can count in clicks.  (The 286 is further limited by
 * its 24 bit address bus, but we can assume in that case that no more than
 * 16M memory is reported by the BIOS.)
 */
#define MAX_REAL	0x00100000L
#define MAX_16BIT	(0xFFF0L << CLICK_SHIFT)

/*=========================================================================*
 *				get_mem_chunks				   *
 *=========================================================================*/
PRIVATE void get_mem_chunks(mem_chunks)
struct memory *mem_chunks;			/* store mem chunks here */
{
/* Initialize the free memory list from the 'memory' boot variable.  Translate
 * the byte offsets and sizes in this list to clicks, properly truncated. Also
 * make sure that we don't exceed the maximum address space of the 286 or the
 * 8086, i.e. when running in 16-bit protected mode or real mode.
 */
  long base, size, limit;
  char *s, *end;			/* use to parse boot variable */ 
  int i, done = 0;
  struct memory *memp;
#if _WORD_SIZE == 2
  unsigned long max_address;
  struct machine machine;
  if (OK != (i=sys_getmachine(&machine)))
	panic(__FILE__, "sys_getmachine failed", i);
#endif

  /* Initialize everything to zero. */
  for (i = 0; i < NR_MEMS; i++) {
	memp = &mem_chunks[i];		/* next mem chunk is stored here */
	memp->base = memp->size = 0;
  }
  
  /* The available memory is determined by MINIX' boot loader as a list of 
   * (base:size)-pairs in boothead.s. The 'memory' boot variable is set in
   * in boot.s.  The format is "b0:s0,b1:s1,b2:s2", where b0:s0 is low mem,
   * b1:s1 is mem between 1M and 16M, b2:s2 is mem above 16M. Pairs b1:s1 
   * and b2:s2 are combined if the memory is adjacent. 
   */
  s = find_param("memory");		/* get memory boot variable */
  for (i = 0; i < NR_MEMS && !done; i++) {
	memp = &mem_chunks[i];		/* next mem chunk is stored here */
	base = size = 0;		/* initialize next base:size pair */
	if (*s != 0) {			/* get fresh data, unless at end */	

	    /* Read fresh base and expect colon as next char. */ 
	    base = strtoul(s, &end, 0x10);		/* get number */
	    if (end != s && *end == ':') s = ++end;	/* skip ':' */ 
	    else *s=0;			/* terminate, should not happen */

	    /* Read fresh size and expect comma or assume end. */ 
	    size = strtoul(s, &end, 0x10);		/* get number */
	    if (end != s && *end == ',') s = ++end;	/* skip ',' */
	    else done = 1;
	}
	limit = base + size;	
#if _WORD_SIZE == 2
	max_address = machine.protected ? MAX_16BIT : MAX_REAL;
	if (limit > max_address) limit = max_address;
#endif
	base = (base + CLICK_SIZE-1) & ~(long)(CLICK_SIZE-1);
	limit &= ~(long)(CLICK_SIZE-1);
	if (limit <= base) continue;
	memp->base = base >> CLICK_SHIFT;
	memp->size = (limit - base) >> CLICK_SHIFT;
  }
}

/*=========================================================================*
 *				patch_mem_chunks			   *
 *=========================================================================*/
PRIVATE void patch_mem_chunks(mem_chunks, map_ptr)
struct memory *mem_chunks;			/* store mem chunks here */
struct mem_map *map_ptr;			/* memory to remove */
{
/* Remove server memory from the free memory list. The boot monitor
 * promises to put processes at the start of memory chunks. The 
 * tasks all use same base address, so only the first task changes
 * the memory lists. The servers and init have their own memory
 * spaces and their memory will be removed from the list. 
 */
  struct memory *memp;
  for (memp = mem_chunks; memp < &mem_chunks[NR_MEMS]; memp++) {
	if (memp->base == map_ptr[T].mem_phys) {
		memp->base += map_ptr[T].mem_len + map_ptr[D].mem_len;
		memp->size -= map_ptr[T].mem_len + map_ptr[D].mem_len;
	}
  }
}

