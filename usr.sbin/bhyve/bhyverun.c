/*-
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: head/usr.sbin/bhyve/bhyverun.c 292982 2015-12-31 10:55:50Z bz $
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: head/usr.sbin/bhyve/bhyverun.c 292982 2015-12-31 10:55:50Z bz $");

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <machine/atomic.h>
#include <machine/segments.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <libgen.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sysexits.h>
#include <stdbool.h>

#include <machine/vmm.h>
#include <vmmapi.h>

#include "bhyverun.h"
#include "acpi.h"
#include "inout.h"
#include "dbgport.h"
#include "fwctl.h"
#include "ioapic.h"
#include "mem.h"
#include "mevent.h"
#include "mptbl.h"
#include "pci_emul.h"
#include "pci_irq.h"
#include "pci_lpc.h"
#include "smbiostbl.h"
#include "xmsr.h"
#include "spinup_ap.h"
#include "rtc.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <fcntl.h>

#include <libxo/xo.h>
#include <ucl.h>

#define GUEST_NIO_PORT		0x488	/* guest upcalls via i/o port */

#define MB		(1024UL * 1024)
#define GB		(1024UL * MB)

#define MAX_SOCK_NAME 200
#define MAX_MSG_SIZE 100

#define BHYVE_RUN_DIR "/var/run/bhyve"
#define CHECKPOINT_RUN_DIR BHYVE_RUN_DIR "/checkpoint"
#define MAX_VMNAME 100

#define JSON_STRUCT_ARR_KEY		"structs"
#define JSON_BASIC_METADATA_KEY 	"basic metadata"
#define JSON_SNAPSHOT_REQ_KEY		"snapshot_req"
#define JSON_STRUCT_SIZE_KEY		"size"
#define JSON_FILE_OFFSET_KEY		"file_offset"
#define JSON_NCPUS_KEY			"ncpus"
#define JSON_VMNAME_KEY 		"vmname"
#define JSON_MEMSIZE_KEY		"memsize"
#define JSON_MEMFLAGS_KEY		"memflags"

#define SNAPSHOT_BUFFER_SIZE (4 * MB)

typedef int (*vmexit_handler_t)(struct vmctx *, struct vm_exit *, int *vcpu);
extern int vmexit_task_switch(struct vmctx *, struct vm_exit *, int *vcpu);

const char *vmname;

int guest_ncpus;
char *guest_uuid_str;

static int guest_vmexit_on_hlt, guest_vmexit_on_pause;
static int virtio_msix = 1;
static int x2apic_mode = 0;	/* default is xAPIC */

static int strictio;
static int strictmsr = 1;

static int acpi;
static int restored = 0;

static char *progname;
static const int BSP = 0;

static cpuset_t cpumask;

static void vm_loop(struct vmctx *ctx, int vcpu, uint64_t rip);

static struct vm_exit vmexit[VM_MAXCPU];

struct bhyvestats {
        uint64_t        vmexit_bogus;
	uint64_t	vmexit_reqidle;
        uint64_t        vmexit_hlt;
        uint64_t        vmexit_pause;
        uint64_t        vmexit_mtrap;
        uint64_t        vmexit_inst_emul;
        uint64_t        cpu_switch_rotate;
        uint64_t        cpu_switch_direct;
} stats;

struct mt_vmm_info {
	pthread_t	mt_thr;
	struct vmctx	*mt_ctx;
	int		mt_vcpu;
} mt_vmm_info[VM_MAXCPU];

struct checkpoint_thread_info {
	struct vmctx *ctx;
	int socket_fd;
	struct sockaddr_un *addr;
} checkpoint_info;

static cpuset_t *vcpumap[VM_MAXCPU] = { NULL };

static void
usage(int code)
{

        fprintf(stderr,
                "Usage: %s [-abehuwxACHPSWY] [-c vcpus] [-g <gdb port>] [-l <lpc>]\n"
		"       %*s [-m mem] [-p vcpu:hostcpu] [-s <pci>] [-U uuid] <vm>\n"
		"       -a: local apic is in xAPIC mode (deprecated)\n"
		"       -A: create ACPI tables\n"
		"       -c: # cpus (default 1)\n"
		"       -C: include guest memory in core file\n"
		"       -e: exit on unhandled I/O access\n"
		"       -g: gdb port\n"
		"       -h: help\n"
		"       -H: vmexit from the guest on hlt\n"
		"       -l: LPC device configuration\n"
		"       -m: memory size in MB\n"
		"       -r: path to checkpoint file\n"
		"       -p: pin 'vcpu' to 'hostcpu'\n"
		"       -P: vmexit from the guest on pause\n"
		"       -s: <slot,driver,configinfo> PCI slot config\n"
		"       -S: guest memory cannot be swapped\n"
		"       -u: RTC keeps UTC time\n"
		"       -U: uuid\n"
		"       -w: ignore unimplemented MSRs\n"
		"       -W: force virtio to use single-vector MSI\n"
		"       -x: local apic is in x2APIC mode\n"
		"       -Y: disable MPtable generation\n",
		progname, (int)strlen(progname), "");

	exit(code);
}

static int
pincpu_parse(const char *opt)
{
	int vcpu, pcpu;

	if (sscanf(opt, "%d:%d", &vcpu, &pcpu) != 2) {
		fprintf(stderr, "invalid format: %s\n", opt);
		return (-1);
	}

	if (vcpu < 0 || vcpu >= VM_MAXCPU) {
		fprintf(stderr, "vcpu '%d' outside valid range from 0 to %d\n",
		    vcpu, VM_MAXCPU - 1);
		return (-1);
	}

	if (pcpu < 0 || pcpu >= CPU_SETSIZE) {
		fprintf(stderr, "hostcpu '%d' outside valid range from "
		    "0 to %d\n", pcpu, CPU_SETSIZE - 1);
		return (-1);
	}

	if (vcpumap[vcpu] == NULL) {
		if ((vcpumap[vcpu] = malloc(sizeof(cpuset_t))) == NULL) {
			perror("malloc");
			return (-1);
		}
		CPU_ZERO(vcpumap[vcpu]);
	}
	CPU_SET(pcpu, vcpumap[vcpu]);
	return (0);
}

void
vm_inject_fault(void *arg, int vcpu, int vector, int errcode_valid,
    int errcode)
{
	struct vmctx *ctx;
	int error, restart_instruction;

	ctx = arg;
	restart_instruction = 1;

	error = vm_inject_exception(ctx, vcpu, vector, errcode_valid, errcode,
	    restart_instruction);
	assert(error == 0);
}

void *
paddr_guest2host(struct vmctx *ctx, uintptr_t gaddr, size_t len)
{

	return (vm_map_gpa(ctx, gaddr, len));
}

int
fbsdrun_vmexit_on_pause(void)
{

	return (guest_vmexit_on_pause);
}

int
fbsdrun_vmexit_on_hlt(void)
{

	return (guest_vmexit_on_hlt);
}

int
fbsdrun_virtio_msix(void)
{

	return (virtio_msix);
}

static void *
fbsdrun_start_thread(void *param)
{
	char tname[MAXCOMLEN + 1];
	struct mt_vmm_info *mtp;
	int vcpu;

	mtp = param;
	vcpu = mtp->mt_vcpu;

	snprintf(tname, sizeof(tname), "vcpu %d", vcpu);
	pthread_set_name_np(mtp->mt_thr, tname);

	vm_loop(mtp->mt_ctx, vcpu, vmexit[vcpu].rip);

	/* not reached */
	exit(1);
	return (NULL);
}

void
fbsdrun_addcpu(struct vmctx *ctx, int fromcpu, int newcpu, uint64_t rip)
{
	int error;

	assert(fromcpu == BSP);

	/*
	 * The 'newcpu' must be activated in the context of 'fromcpu'. If
	 * vm_activate_cpu() is delayed until newcpu's pthread starts running
	 * then vmm.ko is out-of-sync with bhyve and this can create a race
	 * with vm_suspend().
	 */
	error = vm_activate_cpu(ctx, newcpu);
	if (error != 0)
		err(EX_OSERR, "could not activate CPU %d", newcpu);

	CPU_SET_ATOMIC(newcpu, &cpumask);

	/*
	 * Set up the vmexit struct to allow execution to start
	 * at the given RIP
	 */
	vmexit[newcpu].rip = rip;
	vmexit[newcpu].inst_length = 0;

	mt_vmm_info[newcpu].mt_ctx = ctx;
	mt_vmm_info[newcpu].mt_vcpu = newcpu;

	error = pthread_create(&mt_vmm_info[newcpu].mt_thr, NULL,
	    fbsdrun_start_thread, &mt_vmm_info[newcpu]);
	assert(error == 0);
}

static int
fbsdrun_deletecpu(struct vmctx *ctx, int vcpu)
{

	if (!CPU_ISSET(vcpu, &cpumask)) {
		fprintf(stderr, "Attempting to delete unknown cpu %d\n", vcpu);
		exit(1);
	}

	CPU_CLR_ATOMIC(vcpu, &cpumask);
	return (CPU_EMPTY(&cpumask));
}

static int
vmexit_handle_notify(struct vmctx *ctx, struct vm_exit *vme, int *pvcpu,
		     uint32_t eax)
{
#if BHYVE_DEBUG
	/*
	 * put guest-driven debug here
	 */
#endif
        return (VMEXIT_CONTINUE);
}

static int
vmexit_inout(struct vmctx *ctx, struct vm_exit *vme, int *pvcpu)
{
	int error;
	int bytes, port, in, out;
	int vcpu;

	vcpu = *pvcpu;

	port = vme->u.inout.port;
	bytes = vme->u.inout.bytes;
	in = vme->u.inout.in;
	out = !in;

        /* Extra-special case of host notifications */
        if (out && port == GUEST_NIO_PORT) {
                error = vmexit_handle_notify(ctx, vme, pvcpu, vme->u.inout.eax);
		return (error);
	}

	error = emulate_inout(ctx, vcpu, vme, strictio);
	if (error) {
		fprintf(stderr, "Unhandled %s%c 0x%04x at 0x%lx\n",
		    in ? "in" : "out",
		    bytes == 1 ? 'b' : (bytes == 2 ? 'w' : 'l'),
		    port, vmexit->rip);
		return (VMEXIT_ABORT);
	} else {
		return (VMEXIT_CONTINUE);
	}
}

static int
vmexit_rdmsr(struct vmctx *ctx, struct vm_exit *vme, int *pvcpu)
{
	uint64_t val;
	uint32_t eax, edx;
	int error;

	val = 0;
	error = emulate_rdmsr(ctx, *pvcpu, vme->u.msr.code, &val);
	if (error != 0) {
		fprintf(stderr, "rdmsr to register %#x on vcpu %d\n",
		    vme->u.msr.code, *pvcpu);
		if (strictmsr) {
			vm_inject_gp(ctx, *pvcpu);
			return (VMEXIT_CONTINUE);
		}
	}

	eax = val;
	error = vm_set_register(ctx, *pvcpu, VM_REG_GUEST_RAX, eax);
	assert(error == 0);

	edx = val >> 32;
	error = vm_set_register(ctx, *pvcpu, VM_REG_GUEST_RDX, edx);
	assert(error == 0);

	return (VMEXIT_CONTINUE);
}

static int
vmexit_wrmsr(struct vmctx *ctx, struct vm_exit *vme, int *pvcpu)
{
	int error;

	error = emulate_wrmsr(ctx, *pvcpu, vme->u.msr.code, vme->u.msr.wval);
	if (error != 0) {
		fprintf(stderr, "wrmsr to register %#x(%#lx) on vcpu %d\n",
		    vme->u.msr.code, vme->u.msr.wval, *pvcpu);
		if (strictmsr) {
			vm_inject_gp(ctx, *pvcpu);
			return (VMEXIT_CONTINUE);
		}
	}
	return (VMEXIT_CONTINUE);
}

static int
vmexit_spinup_ap(struct vmctx *ctx, struct vm_exit *vme, int *pvcpu)
{
	int newcpu;
	int retval = VMEXIT_CONTINUE;

	newcpu = spinup_ap(ctx, *pvcpu,
			   vme->u.spinup_ap.vcpu, vme->u.spinup_ap.rip);

	return (retval);
}

#define	DEBUG_EPT_MISCONFIG
#ifdef DEBUG_EPT_MISCONFIG
#define	EXIT_REASON_EPT_MISCONFIG	49
#define	VMCS_GUEST_PHYSICAL_ADDRESS	0x00002400
#define	VMCS_IDENT(x)			((x) | 0x80000000)

static uint64_t ept_misconfig_gpa, ept_misconfig_pte[4];
static int ept_misconfig_ptenum;
#endif

static int
vmexit_vmx(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{

	fprintf(stderr, "vm exit[%d]\n", *pvcpu);
	fprintf(stderr, "\treason\t\tVMX\n");
	fprintf(stderr, "\trip\t\t0x%016lx\n", vmexit->rip);
	fprintf(stderr, "\tinst_length\t%d\n", vmexit->inst_length);
	fprintf(stderr, "\tstatus\t\t%d\n", vmexit->u.vmx.status);
	fprintf(stderr, "\texit_reason\t%u\n", vmexit->u.vmx.exit_reason);
	fprintf(stderr, "\tqualification\t0x%016lx\n",
	    vmexit->u.vmx.exit_qualification);
	fprintf(stderr, "\tinst_type\t\t%d\n", vmexit->u.vmx.inst_type);
	fprintf(stderr, "\tinst_error\t\t%d\n", vmexit->u.vmx.inst_error);
//#ifdef DEBUG_EPT_MISCONFIG
	if (vmexit->u.vmx.exit_reason == EXIT_REASON_EPT_MISCONFIG) {
		vm_get_register(ctx, *pvcpu,
		    VMCS_IDENT(VMCS_GUEST_PHYSICAL_ADDRESS),
		    &ept_misconfig_gpa);
		vm_get_gpa_pmap(ctx, ept_misconfig_gpa, ept_misconfig_pte,
		    &ept_misconfig_ptenum);
		fprintf(stderr, "\tEPT misconfiguration:\n");
		fprintf(stderr, "\t\tGPA: %#lx\n", ept_misconfig_gpa);
		fprintf(stderr, "\t\tPTE(%d): %#lx %#lx %#lx %#lx\n",
		    ept_misconfig_ptenum, ept_misconfig_pte[0],
		    ept_misconfig_pte[1], ept_misconfig_pte[2],
		    ept_misconfig_pte[3]);
	}
//#endif	/* DEBUG_EPT_MISCONFIG */
	return (VMEXIT_ABORT);
}

static int
vmexit_svm(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{

	fprintf(stderr, "vm exit[%d]\n", *pvcpu);
	fprintf(stderr, "\treason\t\tSVM\n");
	fprintf(stderr, "\trip\t\t0x%016lx\n", vmexit->rip);
	fprintf(stderr, "\tinst_length\t%d\n", vmexit->inst_length);
	fprintf(stderr, "\texitcode\t%#lx\n", vmexit->u.svm.exitcode);
	fprintf(stderr, "\texitinfo1\t%#lx\n", vmexit->u.svm.exitinfo1);
	fprintf(stderr, "\texitinfo2\t%#lx\n", vmexit->u.svm.exitinfo2);
	return (VMEXIT_ABORT);
}

static int
vmexit_bogus(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{

	assert(vmexit->inst_length == 0);

	stats.vmexit_bogus++;

	return (VMEXIT_CONTINUE);
}

static int
vmexit_reqidle(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{

	assert(vmexit->inst_length == 0);

	stats.vmexit_reqidle++;

	return (VMEXIT_CONTINUE);
}

static int
vmexit_hlt(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{

	stats.vmexit_hlt++;

	/*
	 * Just continue execution with the next instruction. We use
	 * the HLT VM exit as a way to be friendly with the host
	 * scheduler.
	 */
	return (VMEXIT_CONTINUE);
}

static int
vmexit_pause(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{

	stats.vmexit_pause++;

	return (VMEXIT_CONTINUE);
}

static int
vmexit_mtrap(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{

	assert(vmexit->inst_length == 0);

	stats.vmexit_mtrap++;

	return (VMEXIT_CONTINUE);
}

static int
vmexit_inst_emul(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{
	int err, i;
	struct vie *vie;

	stats.vmexit_inst_emul++;

	vie = &vmexit->u.inst_emul.vie;
	err = emulate_mem(ctx, *pvcpu, vmexit->u.inst_emul.gpa,
	    vie, &vmexit->u.inst_emul.paging);

	if (err) {
		if (err == ESRCH) {
			fprintf(stderr, "Unhandled memory access to 0x%lx\n",
			    vmexit->u.inst_emul.gpa);
		}

		fprintf(stderr, "Failed to emulate instruction [");
		for (i = 0; i < vie->num_valid; i++) {
			fprintf(stderr, "0x%02x%s", vie->inst[i],
			    i != (vie->num_valid - 1) ? " " : "");
		}
		fprintf(stderr, "] at 0x%lx\n", vmexit->rip);
		return (VMEXIT_ABORT);
	}

	return (VMEXIT_CONTINUE);
}

static pthread_mutex_t resetcpu_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t resetcpu_cond = PTHREAD_COND_INITIALIZER;

static int
vmexit_suspend(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{
	enum vm_suspend_how how;

	how = vmexit->u.suspended.how;

	fbsdrun_deletecpu(ctx, *pvcpu);

	if (*pvcpu != BSP) {
		pthread_mutex_lock(&resetcpu_mtx);
		pthread_cond_signal(&resetcpu_cond);
		pthread_mutex_unlock(&resetcpu_mtx);
		pthread_exit(NULL);
	}

	pthread_mutex_lock(&resetcpu_mtx);
	while (!CPU_EMPTY(&cpumask)) {
		pthread_cond_wait(&resetcpu_cond, &resetcpu_mtx);
	}
	pthread_mutex_unlock(&resetcpu_mtx);

	switch (how) {
	case VM_SUSPEND_RESET:
		exit(0);
	case VM_SUSPEND_POWEROFF:
		exit(1);
	case VM_SUSPEND_HALT:
		exit(2);
	case VM_SUSPEND_TRIPLEFAULT:
		exit(3);
	default:
		fprintf(stderr, "vmexit_suspend: invalid reason %d\n", how);
		exit(100);
	}
	return (0);	/* NOTREACHED */
}

static vmexit_handler_t handler[VM_EXITCODE_MAX] = {
	[VM_EXITCODE_INOUT]  = vmexit_inout,
	[VM_EXITCODE_INOUT_STR]  = vmexit_inout,
	[VM_EXITCODE_VMX]    = vmexit_vmx,
	[VM_EXITCODE_SVM]    = vmexit_svm,
	[VM_EXITCODE_BOGUS]  = vmexit_bogus,
	[VM_EXITCODE_REQIDLE] = vmexit_reqidle,
	[VM_EXITCODE_RDMSR]  = vmexit_rdmsr,
	[VM_EXITCODE_WRMSR]  = vmexit_wrmsr,
	[VM_EXITCODE_MTRAP]  = vmexit_mtrap,
	[VM_EXITCODE_INST_EMUL] = vmexit_inst_emul,
	[VM_EXITCODE_SPINUP_AP] = vmexit_spinup_ap,
	[VM_EXITCODE_SUSPENDED] = vmexit_suspend,
	[VM_EXITCODE_TASK_SWITCH] = vmexit_task_switch,
};

static void
vm_loop(struct vmctx *ctx, int vcpu, uint64_t startrip)
{
	int error, rc;
	enum vm_exitcode exitcode;
	cpuset_t active_cpus;

	if (vcpumap[vcpu] != NULL) {
		error = pthread_setaffinity_np(pthread_self(),
		    sizeof(cpuset_t), vcpumap[vcpu]);
		assert(error == 0);
	}

	error = vm_active_cpus(ctx, &active_cpus);
	assert(CPU_ISSET(vcpu, &active_cpus));

	error = vm_set_register(ctx, vcpu, VM_REG_GUEST_RIP, startrip);
	assert(error == 0);

	while (1) {
		error = vm_run(ctx, vcpu, &vmexit[vcpu], restored);
		restored = 0;
		if (error != 0)
			break;

		exitcode = vmexit[vcpu].exitcode;
		if (exitcode >= VM_EXITCODE_MAX || handler[exitcode] == NULL) {
			fprintf(stderr, "vm_loop: unexpected exitcode 0x%x\n",
			    exitcode);
			exit(1);
		}

		rc = (*handler[exitcode])(ctx, &vmexit[vcpu], &vcpu);

		switch (rc) {
		case VMEXIT_CONTINUE:
			break;
		case VMEXIT_ABORT:
			abort();
		default:
			exit(1);
		}
	}
	fprintf(stderr, "vm_run error %d, errno %d\n", error, errno);
}

static int
num_vcpus_allowed(struct vmctx *ctx)
{
	int tmp, error;

	error = vm_get_capability(ctx, BSP, VM_CAP_UNRESTRICTED_GUEST, &tmp);

	/*
	 * The guest is allowed to spinup more than one processor only if the
	 * UNRESTRICTED_GUEST capability is available.
	 */
	if (error == 0)
		return (VM_MAXCPU);
	else
		return (1);
}

void
fbsdrun_set_capabilities(struct vmctx *ctx, int cpu)
{
	int err, tmp;

	if (fbsdrun_vmexit_on_hlt()) {
		err = vm_get_capability(ctx, cpu, VM_CAP_HALT_EXIT, &tmp);
		if (err < 0) {
			fprintf(stderr, "VM exit on HLT not supported\n");
			exit(1);
		}
		vm_set_capability(ctx, cpu, VM_CAP_HALT_EXIT, 1);
		if (cpu == BSP)
			handler[VM_EXITCODE_HLT] = vmexit_hlt;
	}

        if (fbsdrun_vmexit_on_pause()) {
		/*
		 * pause exit support required for this mode
		 */
		err = vm_get_capability(ctx, cpu, VM_CAP_PAUSE_EXIT, &tmp);
		if (err < 0) {
			fprintf(stderr,
			    "SMP mux requested, no pause support\n");
			exit(1);
		}
		vm_set_capability(ctx, cpu, VM_CAP_PAUSE_EXIT, 1);
		if (cpu == BSP)
			handler[VM_EXITCODE_PAUSE] = vmexit_pause;
        }

	if (x2apic_mode)
		err = vm_set_x2apic_state(ctx, cpu, X2APIC_ENABLED);
	else
		err = vm_set_x2apic_state(ctx, cpu, X2APIC_DISABLED);

	if (err) {
		fprintf(stderr, "Unable to set x2apic state (%d)\n", err);
		exit(1);
	}

	vm_set_capability(ctx, cpu, VM_CAP_ENABLE_INVPCID, 1);
}

static struct vmctx *
do_open(const char *vmname)
{
	struct vmctx *ctx;
	int error;
	bool reinit, romboot;

	reinit = romboot = false;

	if (lpc_bootrom())
		romboot = true;

	error = vm_create(vmname);
	if (error) {
		if (errno == EEXIST) {
			if (romboot) {
				reinit = true;
			} else {
				/*
				 * The virtual machine has been setup by the
				 * userspace bootloader.
				 */
			}
		} else {
			perror("vm_create");
			exit(1);
		}
	} else {
		if (!romboot) {
			/*
			 * If the virtual machine was just created then a
			 * bootrom must be configured to boot it.
			 */
			fprintf(stderr, "virtual machine cannot be booted\n");
			exit(1);
		}
	}

	ctx = vm_open(vmname);
	if (ctx == NULL) {
		perror("vm_open");
		exit(1);
	}

	if (reinit) {
		error = vm_reinit(ctx);
		if (error) {
			perror("vm_reinit");
			exit(1);
		}
	}
	return (ctx);
}

/* TODO: Harden this function and all of its callers since 'base_str' is a user
 * provided string.
 */
static char *
strcat_extension(const char *base_str, const char *ext)
{
	char *res;
	size_t base_len, ext_len;

	base_len = strnlen(base_str, MAX_VMNAME);
	ext_len = strnlen(ext, MAX_VMNAME);

	if (base_len + ext_len > MAX_VMNAME) {
		fprintf(stderr, "Filename exceeds maximum length.\n");
		return (NULL);
	}

	res = malloc(base_len + ext_len + 1);
	if (res == NULL) {
		perror("Failed to allocate memory.");
		return (NULL);
	}

	memcpy(res, base_str, base_len);
	memcpy(res + base_len, ext, ext_len);
	res[base_len + ext_len] = 0;

	return (res);
}

static void
destroy_restore_state(struct restore_state *rstate)
{
	if (rstate == NULL) {
		fprintf(stderr, "Attempting to destroy NULL restore struct.\n");
		return;
	}

	if (rstate->vmmem_map != MAP_FAILED)
		munmap(rstate->vmmem_map, rstate->vmmem_len);
	if (rstate->kdata_map != MAP_FAILED)
		munmap(rstate->kdata_map, rstate->kdata_len);

	if (rstate->kdata_fd > 0)
		close(rstate->kdata_fd);
	if (rstate->vmmem_fd > 0)
		close(rstate->vmmem_fd);

	if (rstate->meta_root_obj != NULL)
		ucl_object_unref(rstate->meta_root_obj);
	if (rstate->meta_parser != NULL)
		ucl_parser_free(rstate->meta_parser);
}

static int
load_vmmem_file(const char *filename, struct restore_state *rstate)
{
	struct stat sb;
	int err;

	rstate->vmmem_fd = open(filename, O_RDONLY);
	if (rstate->vmmem_fd < 0) {
		perror("Failed to open restore file");
		return (-1);
	}

	err = fstat(rstate->vmmem_fd, &sb);
	if (err < 0) {
		perror("Failed to stat restore file");
		goto err_load_vmmem;
	}

	if (sb.st_size == 0) {
		fprintf(stderr, "Restore file is empty.\n");
		goto err_load_vmmem;
	}

	rstate->vmmem_len = sb.st_size;
	rstate->vmmem_map = mmap(NULL, rstate->vmmem_len, PROT_READ,
				 MAP_SHARED, rstate->vmmem_fd, 0);
	if (rstate->vmmem_map == MAP_FAILED) {
		perror("Failed to map restore file");
		goto err_load_vmmem;
	}

	return (0);

err_load_vmmem:
	if (rstate->vmmem_fd > 0)
		close(rstate->vmmem_fd);
	return (-1);
}

static int
load_kdata_file(const char *filename, struct restore_state *rstate)
{
	struct stat sb;
	int err;

	rstate->kdata_fd = open(filename, O_RDONLY);
	if (rstate->kdata_fd < 0) {
		perror("Failed to open kernel data file");
		return (-1);
	}

	err = fstat(rstate->kdata_fd, &sb);
	if (err < 0) {
		perror("Failed to stat kernel data file");
		goto err_load_kdata;
	}

	if (sb.st_size == 0) {
		fprintf(stderr, "Kernel data file is empty.\n");
		goto err_load_kdata;
	}

	rstate->kdata_len = sb.st_size;
	rstate->kdata_map = mmap(NULL, rstate->kdata_len, PROT_READ,
				 MAP_SHARED, rstate->kdata_fd, 0);
	if (rstate->kdata_map == MAP_FAILED) {
		perror("Failed to map restore file");
		goto err_load_kdata;
	}

	return (0);

err_load_kdata:
	if (rstate->kdata_fd > 0)
		close(rstate->kdata_fd);
	return (-1);
}

static int
load_metadata_file(const char *filename, struct restore_state *rstate)
{
	const ucl_object_t *obj;
	struct ucl_parser *parser;
	int err;

	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	if (parser == NULL) {
		fprintf(stderr, "Failed to initialize UCL parser.\n");
		goto err_load_metadata;
	}

	err = ucl_parser_add_file(parser, filename);
	if (err == 0) {
		fprintf(stderr, "Failed to parse metadata file: '%s'\n",
			filename);
		err = -1;
		goto err_load_metadata;
	}

	obj = ucl_parser_get_object(parser);
	if (obj == NULL) {
		fprintf(stderr, "Failed to parse object.\n");
		err = -1;
		goto err_load_metadata;
	}

	rstate->meta_parser = parser;
	rstate->meta_root_obj = (ucl_object_t *)obj;

	return (0);

err_load_metadata:
	if (parser != NULL)
		ucl_parser_free(parser);
	return (err);
}

static int
load_restore_file(const char *filename, struct restore_state *rstate)
{
	int err = 0;
	char *kdata_filename = NULL, *meta_filename = NULL;

	assert(filename != NULL);
	assert(rstate != NULL);

	memset(rstate, 0, sizeof(*rstate));
	rstate->vmmem_map = MAP_FAILED;
	rstate->kdata_map = MAP_FAILED;

	err = load_vmmem_file(filename, rstate);
	if (err != 0) {
		fprintf(stderr, "Failed to load guest RAM file.\n");
		goto err_restore;
	}

	kdata_filename = strcat_extension(filename, ".kern");
	if (kdata_filename == NULL) {
		fprintf(stderr, "Failed to construct kernel data filename.\n");
		goto err_restore;
	}

	err = load_kdata_file(kdata_filename, rstate);
	if (err != 0) {
		fprintf(stderr, "Failed to load guest kernel data file.\n");
		goto err_restore;
	}

	meta_filename = strcat_extension(filename, ".meta");
	if (meta_filename == NULL) {
		fprintf(stderr, "Failed to construct kernel metadata filename.\n");
		goto err_restore;
	}

	err = load_metadata_file(meta_filename, rstate);
	if (err != 0) {
		fprintf(stderr, "Failed to load guest metadata file.\n");
		goto err_restore;
	}

	return (0);

err_restore:
	destroy_restore_state(rstate);
	if (kdata_filename != NULL)
		free(kdata_filename);
	if (meta_filename != NULL)
		free(meta_filename);
	return (-1);
}

#define JSON_GET_INT_OR_RETURN(key, obj, result_ptr, ret)			\
do {										\
	const ucl_object_t *obj__;						\
	obj__ = ucl_object_lookup(obj, key);					\
	if (obj__ == NULL) {							\
		fprintf(stderr, "Missing key: '%s'", key);			\
		return (ret);							\
	}									\
	if (!ucl_object_toint_safe(obj__, result_ptr)) {			\
		fprintf(stderr, "Cannot convert '%s' value to int.", key);	\
		return (ret);							\
	}									\
} while(0)

#define JSON_GET_STRING_OR_RETURN(key, obj, result_ptr, ret)			\
do {										\
	const ucl_object_t *obj__;						\
	obj__ = ucl_object_lookup(obj, key);					\
	if (obj__ == NULL) {							\
		fprintf(stderr, "Missing key: '%s'", key);			\
		return (ret);							\
	}									\
	if (!ucl_object_tostring_safe(obj__, result_ptr)) {			\
		fprintf(stderr, "Cannot convert '%s' value to string.", key);	\
		return (ret);							\
	}									\
} while(0)

static void *
lookup_struct(enum snapshot_req struct_id, struct restore_state *rstate,
	      size_t *struct_size)
{
	const ucl_object_t *structs = NULL, *obj = NULL;
	ucl_object_iter_t it = NULL;
	int64_t snapshot_req, size, file_offset;

	structs = ucl_object_lookup(rstate->meta_root_obj,
				    JSON_STRUCT_ARR_KEY);
	if (structs == NULL) {
		fprintf(stderr, "Failed to find '%s' object.\n",
			JSON_STRUCT_ARR_KEY);
		return (NULL);
	}

	if (ucl_object_type((ucl_object_t *)structs) != UCL_ARRAY) {
		fprintf(stderr, "Object '%s' is not an array.\n",
		JSON_STRUCT_ARR_KEY);
		return (NULL);
	}

	while ((obj = ucl_object_iterate(structs, &it, true)) != NULL) {
		snapshot_req = -1;
		JSON_GET_INT_OR_RETURN(JSON_SNAPSHOT_REQ_KEY, obj,
				       &snapshot_req, NULL);
		assert(snapshot_req >= 0);
		if ((enum snapshot_req) snapshot_req == struct_id) {
			JSON_GET_INT_OR_RETURN(JSON_STRUCT_SIZE_KEY, obj,
					       &size, NULL);
			assert(size >= 0);

			JSON_GET_INT_OR_RETURN(JSON_FILE_OFFSET_KEY, obj,
					       &file_offset, NULL);
			assert(file_offset >= 0);
			assert(file_offset + size <= rstate->kdata_len);

			*struct_size = (size_t)size;
			return (rstate->kdata_map + file_offset);
		}
	}

	return (NULL);
}

static const ucl_object_t *
lookup_basic_metadata_object(struct restore_state *rstate)
{
	const ucl_object_t *basic_meta_obj = NULL;

	basic_meta_obj = ucl_object_lookup(rstate->meta_root_obj,
					   JSON_BASIC_METADATA_KEY);
	if (basic_meta_obj == NULL) {
		fprintf(stderr, "Failed to find '%s' object.\n",
			JSON_BASIC_METADATA_KEY);
		return (NULL);
	}

	if (ucl_object_type((ucl_object_t *)basic_meta_obj) != UCL_OBJECT) {
		fprintf(stderr, "Object '%s' is not a JSON object.\n",
		JSON_BASIC_METADATA_KEY);
		return (NULL);
	}

	return (basic_meta_obj);
}

static const char *
lookup_vmname(struct restore_state *rstate)
{
	const char *vmname;
	const ucl_object_t *obj;

	obj = lookup_basic_metadata_object(rstate);
	if (obj == NULL)
		return (NULL);

	JSON_GET_STRING_OR_RETURN(JSON_VMNAME_KEY, obj, &vmname, NULL);
	return (vmname);
}

static int
lookup_memflags(struct restore_state *rstate)
{
	int64_t memflags;
	const ucl_object_t *obj;

	obj = lookup_basic_metadata_object(rstate);
	if (obj == NULL)
		return (0);

	JSON_GET_INT_OR_RETURN(JSON_MEMFLAGS_KEY, obj, &memflags, 0);

	return ((int)memflags);
}

static size_t
lookup_memsize(struct restore_state *rstate)
{
	int64_t memsize;
	const ucl_object_t *obj;

	obj = lookup_basic_metadata_object(rstate);
	if (obj == NULL)
		return (0);

	JSON_GET_INT_OR_RETURN(JSON_MEMSIZE_KEY, obj, &memsize, 0);
	if (memsize < 0)
		memsize = 0;

	return ((size_t)memsize);
}


static int
lookup_guest_ncpus(struct restore_state *rstate)
{
	int64_t ncpus;
	const ucl_object_t *obj;

	obj = lookup_basic_metadata_object(rstate);
	if (obj == NULL)
		return (0);

	JSON_GET_INT_OR_RETURN(JSON_NCPUS_KEY, obj, &ncpus, 0);
	return ((int)ncpus);
}

static void
restore_vm_mem(struct vmctx *ctx, struct restore_state *rstate)
{
	vm_restore_mem(ctx, rstate->vmmem_map, rstate->vmmem_len);
}

static int
restore_kernel_structs(struct vmctx *ctx, struct restore_state *rstate)
{
	void *struct_ptr;
	size_t struct_size;
	int ret;
	int i;
	enum snapshot_req structs[] = {
		STRUCT_VMX,
		STRUCT_VM,
		STRUCT_VLAPIC,
		STRUCT_LAPIC,
		STRUCT_VIOAPIC,
	};

	for (i = 0; i < sizeof(structs)/sizeof(structs[0]); i++) {
		struct_ptr = lookup_struct(structs[i], rstate, &struct_size);
		if (struct_ptr == NULL) {
			fprintf(stderr, "Failed to lookup struct vmx\n");
			return (-1);
		}

		ret = vm_restore_req(ctx, structs[i], struct_ptr, struct_size);
		if (ret != 0) {
			fprintf(stderr, "Failed to restore struct: %d\n", structs[i]);
		}
	}

	return 0;
}

static int
vm_snapshot_structs(struct vmctx *ctx, int data_fd, xo_handle_t *xop)
{
	int ret, i, error = 0;
	size_t data_size, offset = 0;
	char *buffer = NULL;
	enum snapshot_req structs[] = {
		STRUCT_VM,
		STRUCT_VMX,
		STRUCT_VIOAPIC,
		STRUCT_VLAPIC,
		STRUCT_LAPIC,
	};

	char *snapshot_struct_names[] = {"vm", "vmx", "vioapic", "vlapic", "lapic"};

	buffer = malloc(SNAPSHOT_BUFFER_SIZE * sizeof(char));
	if (buffer == NULL) {
		perror("Failed to allocate memory for snapshot buffer");
		goto err_vm_snapshot_structs;
	}

	xo_open_list_h(xop, JSON_STRUCT_ARR_KEY);
	for (i = 0; i < sizeof(structs) / sizeof(structs[0]); i++) {
		memset(buffer, 0, SNAPSHOT_BUFFER_SIZE);
		ret = vm_snapshot_req(ctx, structs[i], buffer, SNAPSHOT_BUFFER_SIZE,
				&data_size);

		if (ret != 0) {
			fprintf(stderr, "Failed to snapshot struct %s; ret=%d\n",
				snapshot_struct_names[i], ret);
			error = -1;
			goto err_vm_snapshot_structs;
		}

		ret = write(data_fd, buffer, data_size);
		if (ret != data_size) {
			perror("Failed to write all snapshotted data.");
			error = -1;
			goto err_vm_snapshot_structs;
		}

		/* Write metadata. */
		xo_open_instance_h(xop, JSON_STRUCT_ARR_KEY);
		xo_emit_h(xop, "{:debug_name/%s}\n", snapshot_struct_names[i]);
		xo_emit_h(xop, "{:" JSON_SNAPSHOT_REQ_KEY "/%d}\n", structs[i]);
		xo_emit_h(xop, "{:" JSON_STRUCT_SIZE_KEY "/%lu}\n", data_size);
		xo_emit_h(xop, "{:" JSON_FILE_OFFSET_KEY "/%lu}\n", offset);
		xo_close_instance_h(xop, JSON_STRUCT_ARR_KEY);

		offset += data_size;
	}
	xo_close_list_h(xop, JSON_STRUCT_ARR_KEY);

err_vm_snapshot_structs:
	if (buffer != NULL)
		free(buffer);
	return (error);
}

static int
vm_snapshot_basic_metadata(struct vmctx *ctx, xo_handle_t *xop)
{
	size_t memsize;
	int memflags;
	char vmname_buf[MAX_VMNAME];

	memset(vmname_buf, 0, MAX_VMNAME);
	vm_get_name(ctx, vmname_buf, MAX_VMNAME - 1);

	memsize = vm_get_lowmem_size(ctx) + vm_get_highmem_size(ctx);
	memflags = vm_get_memflags(ctx);

	xo_open_container_h(xop, JSON_BASIC_METADATA_KEY);
	xo_emit_h(xop, "{:" JSON_NCPUS_KEY "/%ld}\n", guest_ncpus);
	xo_emit_h(xop, "{:" JSON_VMNAME_KEY "/%s}\n", vmname_buf);
	xo_emit_h(xop, "{:" JSON_MEMSIZE_KEY "/%lu}\n", memsize);
	xo_emit_h(xop, "{:" JSON_MEMFLAGS_KEY "/%d}\n", memflags);
	xo_close_container_h(xop, JSON_BASIC_METADATA_KEY);

	return 0;
}

static int
vm_snapshot_kern_data(struct vmctx *ctx, char *checkpoint_file, xo_handle_t *xop)
{
	int ret = 0, error = 0, data_fd = 0;
	char *data_filename = NULL;

	data_filename = strcat_extension(checkpoint_file, ".kern");
	if (data_filename == NULL) {
		fprintf(stderr, "Failed to construct kernel data filename.\n");
		return (-1);
	}

	data_fd = open(data_filename, O_WRONLY | O_CREAT | O_TRUNC, 0700);
	if (data_fd < 0) {
		perror("Failed to open kernel data snapshot file.");
		goto err_vm_snapshot_kern_data;
	}

	ret = vm_snapshot_structs(ctx, data_fd, xop);
	if (ret != 0) {
		fprintf(stderr, "Failed to snapshot kernel structs.\n");
		goto err_vm_snapshot_kern_data;
	}

err_vm_snapshot_kern_data:
	if (data_filename != NULL)
		free(data_filename);
	if (data_fd > 0)
		close(data_fd);

	return (error);
}

static int
vm_checkpoint(struct vmctx *ctx, char *checkpoint_file)
{
	int fd_checkpoint = 0;
	int ret = 0;
	int error = 0;
	char *mmap_vm_lowmem = MAP_FAILED;
	char *mmap_vm_highmem = MAP_FAILED;
	char *mmap_checkpoint_file = MAP_FAILED;
	size_t guest_lowmem, guest_highmem, guest_memsize;
	char *guest_baseaddr;
	xo_handle_t *xop = NULL;
	char *meta_filename = NULL;
	FILE *meta_file = NULL;

	fd_checkpoint = open(checkpoint_file, O_RDWR | O_CREAT | O_TRUNC, 0700);

	if (fd_checkpoint < 0) {
		perror("Failed to create checkpoint file");
		error = -1;
		goto done;
	}

	ret = vm_get_guestmem_from_ctx(ctx, &guest_baseaddr, &guest_lowmem, &guest_highmem);
	guest_memsize = guest_lowmem + guest_highmem;
	if (ret < 0) {
		fprintf(stderr, "Failed to get guest mem information (base, low, high)\n");
		error = -1;
		goto done;
	}

	/* make space for VMs address space */
	ret = ftruncate(fd_checkpoint, guest_memsize);
	if (ret < 0) {
		perror("Failed to truncate checkpoint file\n");
		goto done;
	}

	meta_filename = strcat_extension(checkpoint_file, ".meta");
	if (meta_filename == NULL) {
		fprintf(stderr, "Failed to construct vm metadata filename.\n");
		goto done;
	}

	meta_file = fopen(meta_filename, "w");
	if (meta_file == NULL) {
		perror("Failed to open vm metadata snapshot file.");
		goto done;
	}

	xop = xo_create_to_file(meta_file, XO_STYLE_JSON, XOF_PRETTY);
	if (xop == NULL) {
		perror("Failed to get libxo handle on metadata file.");
		goto done;
	}

	ret = vm_snapshot_basic_metadata(ctx, xop);
	if (ret != 0) {
		fprintf(stderr, "Failed to snapshot vm basic metadata.\n");
		error = -1;
		goto done;
	}

	/*
	 * mmap VMs memory in bhyverun virtual memory: the original address space
	 * (of the VM) will be COW
	 */
	ret = vm_get_vm_mem(ctx, &mmap_vm_lowmem, &mmap_vm_highmem,
			guest_baseaddr, guest_lowmem, guest_highmem);
	if (ret != 0) {
		fprintf(stderr, "Could not mmap guests lowmem and highmem\n");
		error = ret;
		goto done;
	}

	vm_vcpu_lock_all(ctx);

	/*
	 * mmap checkpoint file in memory so we can easily copy VMs
	 * system address space (lowmem + highmem) from kernel space
	 */
	mmap_checkpoint_file = mmap(NULL, guest_memsize, PROT_WRITE | PROT_READ,
			MAP_SHARED, fd_checkpoint, 0);

	if (mmap_checkpoint_file == MAP_FAILED) {
		perror("Failed to mmap checkpoint file");
		error = -1;
		goto done_unlock;
	}

	memcpy(mmap_checkpoint_file, mmap_vm_lowmem, guest_lowmem);

	if (guest_highmem > 0)
		memcpy(mmap_checkpoint_file + guest_lowmem,
		       mmap_vm_highmem, guest_highmem);

	ret = vm_snapshot_kern_data(ctx, checkpoint_file, xop);
	if (ret != 0) {
		fprintf(stderr, "Failed to snapshot vm kernel data.\n");
		error = -1;
		goto done_unlock;
	}

	xo_finish_h(xop);

done_unlock:
	vm_vcpu_unlock_all(ctx);
done:

	if (fd_checkpoint > 0)
		close(fd_checkpoint);
	if (mmap_checkpoint_file != MAP_FAILED)
		munmap(mmap_checkpoint_file, guest_memsize);
	if (mmap_vm_lowmem != MAP_FAILED)
		munmap(mmap_vm_lowmem, guest_lowmem);
	if (mmap_vm_highmem != MAP_FAILED)
		munmap(mmap_vm_highmem, guest_highmem);
	if (meta_filename != NULL)
		free(meta_filename);
	if (xop != NULL)
		xo_destroy(xop);
	if (meta_file != NULL)
		fclose(meta_file);
	return (error);
}

int get_checkpoint_msg(int conn_fd, struct vmctx *ctx)
{
	unsigned char buf[MAX_MSG_SIZE];
	struct checkpoint_op *checkpoint_op;
	int len, recv_len, total_recv = 0;
	int err = 0;

	len = sizeof(struct checkpoint_op); /* expected length */
	while ((recv_len = recv(conn_fd, buf + total_recv, len - total_recv, 0)) > 0) {
		total_recv += recv_len;
	}
	if (recv_len < 0) {
		perror("Error while receiving data from bhyvectl");
		err = -1;
		goto done;
	}

	checkpoint_op = (struct checkpoint_op *)buf;
	switch (checkpoint_op->op) {
		case START_CHECKPOINT:
			err = vm_checkpoint(ctx, checkpoint_op->snapshot_filename);
		break;
		default:
			fprintf(stderr, "Unrecognized checkpoint operation.\n");
			err = -1;
	}

done:
	close(conn_fd);
	return (err);
}

/*
 * Listen for commands from bhyvectl
 */
void * checkpoint_thread(void *param)
{
	struct checkpoint_thread_info *thread_info;
	socklen_t addr_len;
	int conn_fd, ret;

	thread_info = (struct checkpoint_thread_info *)param;

	addr_len = sizeof(thread_info->addr);
	while ((conn_fd = accept(thread_info->socket_fd,
			(struct sockaddr *) thread_info->addr,
			&addr_len)) > -1) {
		ret = get_checkpoint_msg(conn_fd, thread_info->ctx);
		if (ret != 0) {
			fprintf(stderr, "Failed to read message on checkpoint "
					"socket. Retrying.\n");
		}

		addr_len = sizeof(struct sockaddr_un);
	}
	if (conn_fd < -1) {
		perror("Failed to accept connection");
	}

	return (NULL);
}

/*
 * Create directory tree to store runtime specific information:
 * i.e. UNIX sockets for IPC with bhyvectl.
 */
static int
make_checkpoint_dir()
{
	int err;

	err = mkdir(BHYVE_RUN_DIR, 0755);
	if (err < 0 && errno != EEXIST)
		return (err);

	err = mkdir(CHECKPOINT_RUN_DIR, 0755);
	if (err < 0 && errno != EEXIST)
		return (err);

	return 0;
}

/*
 * Create the listening socket for IPC with bhyvectl
 */
int init_checkpoint_thread(struct vmctx *ctx)
{
	struct sockaddr_un addr;
	int socket_fd;
	pthread_t checkpoint_pthread;
	char vmname_buf[MAX_VMNAME];
	int ret, err = 0;

	socket_fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (socket_fd < 0) {
		perror("Socket creation failed (IPC with bhyvectl");
		err = -1;
		goto fail;
	}

	err = make_checkpoint_dir();
	if (err < 0) {
		perror("Failed to create checkpoint runtime directory");
		goto fail;
	}

	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	vm_get_name(ctx, vmname_buf, MAX_VMNAME - 1);
	snprintf(addr.sun_path, PATH_MAX, "%s/%s",
		 CHECKPOINT_RUN_DIR, vmname_buf);
	unlink(addr.sun_path);

	if (bind(socket_fd, (struct sockaddr *)&addr,
			sizeof(struct sockaddr_un)) != 0) {
		perror("Failed to bind socket (IPC with bhyvectl)");
		err = -1;
		goto fail;
	}

	if (listen(socket_fd, 10) < 0) {
		perror("Failed to listen on socket (IPC with bhyvectl)");
		err = -1;
		goto fail;
	}

	memset(&checkpoint_info, 0, sizeof(struct checkpoint_thread_info));
	checkpoint_info.ctx = ctx;
	checkpoint_info.socket_fd = socket_fd;
	checkpoint_info.addr = &addr;


	/* TODO: start thread for listening connections */
	pthread_set_name_np(checkpoint_pthread, "checkpoint thread");
	ret = pthread_create(&checkpoint_pthread, NULL, checkpoint_thread,
		&checkpoint_info);
	if (ret < 0) {
		err = ret;
		goto fail;
	}

	return (0);
fail:
	if (socket_fd > 0)
		close(socket_fd);
	unlink(addr.sun_path);

	return (err);
}

int
main(int argc, char *argv[])
{
	int c, error, gdb_port, err, bvmcons;
	int max_vcpus, mptgen, memflags;
	int rtc_localtime;
	struct vmctx *ctx;
	uint64_t rip;
	size_t memsize;
	char *optstr, *restore_file = NULL;
	struct restore_state rstate;

	bvmcons = 0;
	progname = basename(argv[0]);
	gdb_port = 0;
	guest_ncpus = 1;
	memsize = 256 * MB;
	mptgen = 1;
	rtc_localtime = 1;
	memflags = 0;

	optstr = "abehuwxACHIPSWYp:g:c:s:m:l:U:r:";
	while ((c = getopt(argc, argv, optstr)) != -1) {
		switch (c) {
		case 'a':
			x2apic_mode = 0;
			break;
		case 'A':
			acpi = 1;
			break;
		case 'b':
			bvmcons = 1;
			break;
		case 'p':
                        if (pincpu_parse(optarg) != 0) {
                            errx(EX_USAGE, "invalid vcpu pinning "
                                 "configuration '%s'", optarg);
                        }
			break;
                case 'c':
			guest_ncpus = atoi(optarg);
			break;
		case 'C':
			memflags |= VM_MEM_F_INCORE;
			break;
		case 'g':
			gdb_port = atoi(optarg);
			break;
		case 'l':
			if (lpc_device_parse(optarg) != 0) {
				errx(EX_USAGE, "invalid lpc device "
				    "configuration '%s'", optarg);
			}
			break;
		case 'r':
			restore_file = optarg;
			restored = 1;
			break;
		case 's':
			if (pci_parse_slot(optarg) != 0)
				exit(1);
			else
				break;
		case 'S':
			memflags |= VM_MEM_F_WIRED;
			break;
                case 'm':
			error = vm_parse_memsize(optarg, &memsize);
			if (error)
				errx(EX_USAGE, "invalid memsize '%s'", optarg);
			break;
		case 'H':
			guest_vmexit_on_hlt = 1;
			break;
		case 'I':
			/*
			 * The "-I" option was used to add an ioapic to the
			 * virtual machine.
			 *
			 * An ioapic is now provided unconditionally for each
			 * virtual machine and this option is now deprecated.
			 */
			break;
		case 'P':
			guest_vmexit_on_pause = 1;
			break;
		case 'e':
			strictio = 1;
			break;
		case 'u':
			rtc_localtime = 0;
			break;
		case 'U':
			guest_uuid_str = optarg;
			break;
		case 'w':
			strictmsr = 0;
			break;
		case 'W':
			virtio_msix = 0;
			break;
		case 'x':
			x2apic_mode = 1;
			break;
		case 'Y':
			mptgen = 0;
			break;
		case 'h':
			usage(0);
		default:
			usage(1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 1 || (argc == 0 && restore_file == NULL))
		usage(1);

	if (restore_file != NULL) {
		error = load_restore_file(restore_file, &rstate);
		if (error) {
			fprintf(stderr, "Failed to read checkpoint info from "
					"file: '%s'.\n", restore_file);
			exit(1);
		}
	}

	if (argc == 1) {
		vmname = argv[0];
	} else {
		vmname = lookup_vmname(&rstate);
		if (vmname == NULL) {
			fprintf(stderr, "Cannot find VM name in restore file. "
					"Please specify one.\n");
			exit(1);
		}
	}
	ctx = do_open(vmname);

	if (restore_file != NULL) {
		guest_ncpus = lookup_guest_ncpus(&rstate);
		memflags = lookup_memflags(&rstate);
		memsize = lookup_memsize(&rstate);
	}

	if (guest_ncpus < 1) {
		fprintf(stderr, "Invalid guest vCPUs (%d)\n", guest_ncpus);
		exit(1);
	}

	max_vcpus = num_vcpus_allowed(ctx);
	if (guest_ncpus > max_vcpus) {
		fprintf(stderr, "%d vCPUs requested but only %d available\n",
			guest_ncpus, max_vcpus);
		exit(1);
	}

	fbsdrun_set_capabilities(ctx, BSP);

	vm_set_memflags(ctx, memflags);
	err = vm_setup_memory(ctx, memsize, VM_MMAP_ALL);
	if (err) {
		fprintf(stderr, "Unable to setup memory (%d)\n", errno);
		exit(1);
	}

	error = init_msr();
	if (error) {
		fprintf(stderr, "init_msr error %d", error);
		exit(1);
	}

	init_mem();
	init_inout();
	pci_irq_init(ctx);
	ioapic_init(ctx);

	rtc_init(ctx, rtc_localtime);
	sci_init(ctx);

	/*
	 * Exit if a device emulation finds an error in it's initilization
	 */
	if (init_pci(ctx) != 0)
		exit(1);

	if (gdb_port != 0)
		init_dbgport(gdb_port);

	if (bvmcons)
		init_bvmcons();

	if (lpc_bootrom()) {
		if (vm_set_capability(ctx, BSP, VM_CAP_UNRESTRICTED_GUEST, 1)) {
			fprintf(stderr, "ROM boot failed: unrestricted guest "
			    "capability not available\n");
			exit(1);
		}
		error = vcpu_reset(ctx, BSP);
		assert(error == 0);
	}

	if (restore_file != NULL) {
		restore_vm_mem(ctx, &rstate);

		if (restore_kernel_structs(ctx, &rstate) != 0) {
			fprintf(stderr, "Failed to restore kernel structs.\n");
			exit(1);
		}

	}
	error = vm_get_register(ctx, BSP, VM_REG_GUEST_RIP, &rip);
	assert(error == 0);

	/*
	 * build the guest tables, MP etc.
	 */
	if (mptgen) {
		error = mptable_build(ctx, guest_ncpus);
		if (error)
			exit(1);
	}

	error = smbios_build(ctx);
	assert(error == 0);

	if (acpi) {
		error = acpi_build(ctx, guest_ncpus);
		assert(error == 0);
	}

	if (lpc_bootrom())
		fwctl_init();

	if (restore_file != NULL)
		destroy_restore_state(&rstate);

	/*
	 * checkpointing thread for communication with bhyvectl
	 */
	if(init_checkpoint_thread(ctx) < 0)
		printf("Failed to start checkpoint thread!\n");


	/*
	 * Change the proc title to include the VM name.
	 */
	setproctitle("%s", vmname);

	/*
	 * Add CPU 0
	 */
	fbsdrun_addcpu(ctx, BSP, BSP, rip);

	/*
	 * Head off to the main event dispatch loop
	 */
	mevent_dispatch();

	exit(1);
}
