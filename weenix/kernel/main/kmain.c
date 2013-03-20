#include "types.h"
#include "globals.h"
#include "kernel.h"

#include "util/gdb.h"
#include "util/init.h"
#include "util/debug.h"
#include "util/string.h"
#include "util/printf.h"

#include "mm/mm.h"
#include "mm/page.h"
#include "mm/pagetable.h"
#include "mm/pframe.h"

#include "vm/vmmap.h"
#include "vm/shadow.h"
#include "vm/anon.h"

#include "main/acpi.h"
#include "main/apic.h"
#include "main/interrupt.h"
#include "main/cpuid.h"
#include "main/gdt.h"

#include "proc/sched.h"
#include "proc/proc.h"
#include "proc/kthread.h"

#include "drivers/dev.h"
#include "drivers/blockdev.h"
#include "drivers/tty/virtterm.h"

#include "api/exec.h"
#include "api/syscall.h"

#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/vfs_syscall.h"
#include "fs/fcntl.h"
#include "fs/stat.h"

#include "test/kshell/kshell.h"

GDB_DEFINE_HOOK(boot)
GDB_DEFINE_HOOK(initialized)
GDB_DEFINE_HOOK(shutdown)

static void      *bootstrap(int arg1, void *arg2);
static void      *idleproc_run(int arg1, void *arg2);
static kthread_t *initproc_create(void);
static void      *initproc_run(int arg1, void *arg2);
static void       hard_shutdown(void);

static context_t bootstrap_context;

/**
 * This is the first real C function ever called. It performs a lot of
 * hardware-specific initialization, then creates a pseudo-context to
 * execute the bootstrap function in.
 */
void
kmain()
{
        GDB_CALL_HOOK(boot);

        dbg_init();
        dbgq(DBG_CORE, "Kernel binary:\n");
        dbgq(DBG_CORE, "  text: 0x%p-0x%p\n", &kernel_start_text, &kernel_end_text);
        dbgq(DBG_CORE, "  data: 0x%p-0x%p\n", &kernel_start_data, &kernel_end_data);
        dbgq(DBG_CORE, "  bss:  0x%p-0x%p\n", &kernel_start_bss, &kernel_end_bss);

        page_init();

        pt_init();
        slab_init();
        pframe_init();

        acpi_init();
        apic_init();
        intr_init();

        gdt_init();

        /* initialize slab allocators */
#ifdef __VM__
        anon_init();
        shadow_init();
#endif
        vmmap_init();
        proc_init();
        kthread_init();

#ifdef __DRIVERS__
        bytedev_init();
        blockdev_init();
#endif

        void *bstack = page_alloc();
        pagedir_t *bpdir = pt_get();
        KASSERT(NULL != bstack && "Ran out of memory while booting.");
        context_setup(&bootstrap_context, bootstrap, 0, NULL, bstack, PAGE_SIZE, bpdir);
        context_make_active(&bootstrap_context);

        panic("\nReturned to kmain()!!!\n");
}

/**
 * This function is called from kmain, however it is not running in a
 * thread context yet. It should create the idle process which will
 * start executing idleproc_run() in a real thread context.  To start
 * executing in the new process's context call context_make_active(),
 * passing in the appropriate context. This function should _NOT_
 * return.
 *
 * Note: Don't forget to set curproc and curthr appropriately.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *bootstrap(int arg1, void *arg2)
{
        /* necessary to finalize page table information */
        pt_template_init();
        char name[4]="IDLE";                 
        curproc=proc_create(name);
       KASSERT(curproc != NULL && "Could not create Idle process"); /* make sure that the "idle" process has been created successfully */
        KASSERT(curproc->p_pid == PID_IDLE);
        KASSERT(PID_IDLE == curproc->p_pid && "Process created is not Idle");        
      
        curthr=kthread_create(curproc,idleproc_run,arg1,arg2);
        KASSERT(curthr != NULL && "Could not create thread for Idle process");      

        context_make_active(&(curthr->kt_ctx));
        NOT_YET_IMPLEMENTED("PROCS: bootstrap");
        panic("weenix returned to bootstrap()!!! BAD!!!\n");
        return NULL;
}

/**
 * Once we're inside of idleproc_run(), we are executing in the context of the
 * first process-- a real context, so we can finally begin running
 * meaningful code.
 *
 * This is the body of process 0. It should initialize all that we didn't
 * already initialize in kmain(), launch the init process (initproc_run),
 * wait for the init process to exit, then halt the machine.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *idleproc_run(int arg1, void *arg2)
{
        int status;
        pid_t child;

        /* create init proc */
        kthread_t *initthr = initproc_create();        
        init_call_all();
        GDB_CALL_HOOK(initialized);

        /* Create other kernel threads (in order) */

#ifdef __VFS__
        /* Once you have VFS remember to set the current working directory
         * of the idle and init processes */

        /* Here you need to make the null, zero, and tty devices using mknod */
        /* You can't do this until you have VFS, check the include/drivers/dev.h
         * file for macros with the device ID's you will need to pass to mknod */
#endif

        /* Finally, enable interrupts (we want to make sure interrupts
         * are enabled AFTER all drivers are initialized) */
        intr_enable();
        
        /* Run initproc */
        sched_make_runnable(initthr);
/*        dbg_print("\nidleproc_run returned from sched_make_rinnable and calling do wait_pid \n");*/
        /* Now wait for it */        
        child = do_waitpid(-1, 0, &status);
        dbg_print("Process %d cleaned successfully\n", child);
        dbg_print("\n idleproc_run wait over for child die \n");
        KASSERT(PID_INIT == child);

#ifdef __MTP__
        kthread_reapd_shutdown();
#endif


#ifdef __VFS__
        /* Shutdown the vfs: */
        dbg_print("weenix: vfs shutdown...\n");
        vput(curproc->p_cwd);
        if (vfs_shutdown())
                panic("vfs shutdown FAILED!!\n");

#endif

        /* Shutdown the pframe system */
#ifdef __S5FS__
        pframe_shutdown();
#endif

        dbg_print("\nweenix: halted cleanly!\n");
        GDB_CALL_HOOK(shutdown);
        hard_shutdown();
        return NULL;
}

/**
 * This function, called by the idle process (within 'idleproc_run'), creates the
 * process commonly refered to as the "init" process, which should have PID 1.
 *
 * The init process should contain a thread which begins execution in
 * initproc_run().
 *
 * @return a pointer to a newly created thread which will execute
 * initproc_run when it begins executing
 */
static kthread_t *initproc_create(void)
{
        proc_t *procc;
        char name[4]="INIT";
        procc=proc_create(name);
        KASSERT(NULL != procc);
        KASSERT(PID_INIT == procc->p_pid);
        kthread_t *initthr;
        initthr=kthread_create(procc,initproc_run,NULL,NULL);       
        KASSERT(initthr != NULL && "Could not create thread for Idle process");      
        NOT_YET_IMPLEMENTED("PROCS: initproc_create");
        return initthr;
}

/**
 * The init thread's function changes depending on how far along your Weenix is
 * developed. Before VM/FI, you'll probably just want to have this run whatever
 * tests you've written (possibly in a new process). After VM/FI, you'll just
 * exec "/bin/init".
 *
 * Both arguments are unused.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
void *get_sum1(int arg1,void *arg2);
void *get_sum2(int arg1,void *arg2);
void *get_mul(int arg1,void *arg2);
static void *
initproc_run(int arg1, void *arg2)
{
        
        dbg_print("\n inside initproc_run \n");

        /* 1st child proc */
        proc_t *proc1 = proc_create("proc1");
        KASSERT(proc1 != NULL);
        kthread_t *thread1 = kthread_create(proc1,get_sum1,10,(void*)20);
        KASSERT(thread1 !=NULL);
     

        /* 2nd child proc */
        proc_t *proc2 = proc_create("proc2");
        KASSERT(proc2 != NULL);
        kthread_t *thread2 = kthread_create(proc2,get_sum2,40,(void*)20);
        KASSERT(thread2 !=NULL);
       
        sched_make_runnable(thread1);
        sched_make_runnable(thread2);
       
	int status;
       while(!list_empty(&curproc->p_children))
        {
                pid_t child = do_waitpid(-1, 0, &status);
                dbg_print("Process %d cleaned successfully\n", child);
        }

        NOT_YET_IMPLEMENTED("PROCS: initproc_run");

        return NULL;
}

void *get_sum1(int arg1,void *arg2)
{

int result = arg1 + (int)arg2;

dbg_print("\n pid %d: Sum is == %d\n",curproc->p_pid,result);
return NULL;
}
void *get_sum2(int arg1,void *arg2)
{

int result = arg1 + (int)arg2;
dbg_print("\n pid %d:  Sum is == %d\n",curproc->p_pid,result);
proc_t *proc3 = proc_create("proc3");
        KASSERT(proc3 != NULL);
        kthread_t *thread3 = kthread_create(proc3,get_mul,40,(void*)20);
        KASSERT(thread3 !=NULL);
          sched_make_runnable(thread3);
          int status;
       while(!list_empty(&curproc->p_children))
        {
                pid_t child = do_waitpid(-1, 0, &status);
                dbg_print("Process %d cleaned successfully\n", child);
        }
return NULL;
}
void *get_mul(int arg1,void *arg2)
{

int result = arg1 * (int)arg2;
dbg_print("\n pid %d:  Mul is == %d\n",curproc->p_pid,result);
return NULL;
}
/**
 * Clears all interrupts and halts, meaning that we will never run
 * again.
 */
static void
hard_shutdown()
{
#ifdef __DRIVERS__
        vt_print_shutdown();
#endif
        __asm__ volatile("cli; hlt");
}
