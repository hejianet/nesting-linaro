/*
 * Copyright (C) 2012,2013 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 *
 * Derived from arch/arm/kvm/handle_exit.c:
 * Copyright (C) 2012 - Virtual Open Systems and Columbia University
 * Author: Christoffer Dall <c.dall@virtualopensystems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kvm.h>
#include <linux/kvm_host.h>

#include <asm/esr.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_coproc.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_psci.h>

#define CREATE_TRACE_POINTS
#include "trace.h"

typedef int (*exit_handle_fn)(struct kvm_vcpu *, struct kvm_run *);

static int handle_hvc(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	int ret;

	trace_kvm_hvc_arm64(*vcpu_pc(vcpu), vcpu_get_reg(vcpu, 0),
			    kvm_vcpu_hvc_get_imm(vcpu));
	vcpu->stat.hvc_exit_stat++;

	/* Forward hvc instructions to the virtual EL2 if the guest has EL2. */
	if (nested_virt_in_use(vcpu)) {
#ifndef CONFIG_KVM_ARM_NESTED_PV
		kvm_inject_nested_sync(vcpu, kvm_vcpu_get_hsr(vcpu));
		return 1;
#else
		ret = handle_hvc_nested(vcpu);
		if (ret < 0 && ret != -EINVAL)
			return ret;
		else if (ret >= 0)
			return ret;
#endif
	}

	ret = kvm_psci_call(vcpu);
	if (ret < 0) {
		kvm_inject_undefined(vcpu);
		return 1;
	}

	return ret;
}

static int handle_smc(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	int ret;

	/*
	 * Forward this trapped smc instruction to the virtual EL2.
	 */
	if (forward_nv_traps(vcpu) && (vcpu_sys_reg(vcpu, HCR_EL2) & HCR_TSC))
		return kvm_inject_nested_sync(vcpu, kvm_vcpu_get_hsr(vcpu));

	/* If imm is non-zero, it's not defined */
	if (kvm_vcpu_hvc_get_imm(vcpu)) {
		kvm_inject_undefined(vcpu);
		return 1;
	}

	/*
	 * If imm is zero, it's a psci call.
	 * Note that on ARMv8.3, even if EL3 is not implemented, SMC executed
	 * at Non-secure EL1 is trapped to EL2 if HCR_EL2.TSC==1, rather than
	 * being treated as UNDEFINED.
	 */
	ret = kvm_psci_call(vcpu);
	if (ret < 0) {
		kvm_inject_undefined(vcpu);
		return 1;
	}
	kvm_skip_instr(vcpu, kvm_vcpu_trap_il_is32bit(vcpu));

	return ret;
}

/*
 * When the system supports FP/ASMID and we are NOT running nested
 * virtualization, FP/ASMID traps are handled in EL2 directly.
 * This handler handles the cases those are not belong to the above case.
 */
static int kvm_handle_fpasimd(struct kvm_vcpu *vcpu, struct kvm_run *run)
{

	/* This is for nested virtualization */
	if (vcpu_sys_reg(vcpu, CPTR_EL2) & CPTR_EL2_TFP)
		return kvm_inject_nested_sync(vcpu, kvm_vcpu_get_hsr(vcpu));

	/* This is the case when the system doesn't support FP/ASIMD. */
	kvm_inject_undefined(vcpu);
	return 1;
}

/**
 * kvm_handle_wfx - handle a wait-for-interrupts or wait-for-event
 *		    instruction executed by a guest
 *
 * @vcpu:	the vcpu pointer
 *
 * WFE: Yield the CPU and come back to this vcpu when the scheduler
 * decides to.
 * WFI: Simply call kvm_vcpu_block(), which will halt execution of
 * world-switches and schedule other host processes until there is an
 * incoming IRQ or FIQ to the VM.
 */
static int kvm_handle_wfx(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	bool is_wfe = !!(kvm_vcpu_get_hsr(vcpu) & ESR_ELx_WFx_ISS_WFE);

	if (nested_virt_in_use(vcpu)) {
		int ret = handle_wfx_nested(vcpu, is_wfe);

		if (ret < 0 && ret != -EINVAL)
			return ret;
		else if (ret >= 0)
			return ret;
	}

	if (is_wfe) {
		trace_kvm_wfx_arm64(*vcpu_pc(vcpu), true);
		vcpu->stat.wfe_exit_stat++;
		kvm_vcpu_on_spin(vcpu);
	} else {
		trace_kvm_wfx_arm64(*vcpu_pc(vcpu), false);
		vcpu->stat.wfi_exit_stat++;
		kvm_vcpu_block(vcpu);
		kvm_clear_request(KVM_REQ_UNHALT, vcpu);
	}

	kvm_skip_instr(vcpu, kvm_vcpu_trap_il_is32bit(vcpu));

	return 1;
}

/**
 * kvm_handle_guest_debug - handle a debug exception instruction
 *
 * @vcpu:	the vcpu pointer
 * @run:	access to the kvm_run structure for results
 *
 * We route all debug exceptions through the same handler. If both the
 * guest and host are using the same debug facilities it will be up to
 * userspace to re-inject the correct exception for guest delivery.
 *
 * @return: 0 (while setting run->exit_reason), -1 for error
 */
static int kvm_handle_guest_debug(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	u32 hsr = kvm_vcpu_get_hsr(vcpu);
	int ret = 0;

	run->exit_reason = KVM_EXIT_DEBUG;
	run->debug.arch.hsr = hsr;

	switch (ESR_ELx_EC(hsr)) {
	case ESR_ELx_EC_WATCHPT_LOW:
		run->debug.arch.far = vcpu->arch.fault.far_el2;
		/* fall through */
	case ESR_ELx_EC_SOFTSTP_LOW:
	case ESR_ELx_EC_BREAKPT_LOW:
	case ESR_ELx_EC_BKPT32:
	case ESR_ELx_EC_BRK64:
		break;
	default:
		kvm_err("%s: un-handled case hsr: %#08x\n",
			__func__, (unsigned int) hsr);
		ret = -1;
		break;
	}

	return ret;
}

static int kvm_handle_unknown_ec(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	u32 hsr = kvm_vcpu_get_hsr(vcpu);

	kvm_pr_unimpl("Unknown exception class: hsr: %#08x -- %s\n",
		      hsr, esr_get_class_string(hsr));

	kvm_inject_undefined(vcpu);
	return 1;
}

int kvm_handle_eret(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	trace_kvm_nested_eret(vcpu, vcpu_el2_sreg(vcpu, ELR_EL2),
			      vcpu_el2_sreg(vcpu, SPSR_EL2));

	/*
	 * Forward this trap to the virtual EL2 if the virtual HCR_EL2.NV
	 * bit is set.
	 */
	if (forward_nv_traps(vcpu))
		return kvm_inject_nested_sync(vcpu, kvm_vcpu_get_hsr(vcpu));

	/*
	 * Note that the current exception level is always the virtual EL2,
	 * since we set HCR_EL2.NV bit only when entering the virtual EL2.
	 */
	*vcpu_pc(vcpu) = vcpu_el2_sreg(vcpu, ELR_EL2);
	*vcpu_cpsr(vcpu) = vcpu_el2_sreg(vcpu, SPSR_EL2);

	/*
	 * When a VHE host kernel running in a VM returns to itself, the vcpu
	 * mode should stay in the virtual EL2. However, the target exception
	 * level stored in the virtual SPSR_EL2 can be EL1; The target EL
	 * is set when the VHE host kernel is taking an exception to itself,
	 * and it is the physical EL1. We set it back to the virtual EL2 mode.
	 */
	if (vcpu_mode_el1(vcpu) && vcpu_el2_e2h_is_set(vcpu)
	    && vcpu_el2_tge_is_set(vcpu)) {
		u32 mode = *vcpu_cpsr(vcpu) & PSR_MODE_MASK;

		*vcpu_cpsr(vcpu) &= ~PSR_MODE_MASK;
		if (mode == PSR_MODE_EL1h)
			*vcpu_cpsr(vcpu) |= PSR_MODE_EL2h;
		else
			*vcpu_cpsr(vcpu) |= PSR_MODE_EL2t;
	}

	return 1;
}

static exit_handle_fn arm_exit_handlers[] = {
	[0 ... ESR_ELx_EC_MAX]	= kvm_handle_unknown_ec,
	[ESR_ELx_EC_WFx]	= kvm_handle_wfx,
	[ESR_ELx_EC_CP15_32]	= kvm_handle_cp15_32,
	[ESR_ELx_EC_CP15_64]	= kvm_handle_cp15_64,
	[ESR_ELx_EC_CP14_MR]	= kvm_handle_cp14_32,
	[ESR_ELx_EC_CP14_LS]	= kvm_handle_cp14_load_store,
	[ESR_ELx_EC_CP14_64]	= kvm_handle_cp14_64,
	[ESR_ELx_EC_HVC32]	= handle_hvc,
	[ESR_ELx_EC_SMC32]	= handle_smc,
	[ESR_ELx_EC_HVC64]	= handle_hvc,
	[ESR_ELx_EC_SMC64]	= handle_smc,
	[ESR_ELx_EC_SYS64]	= kvm_handle_sys,
	[ESR_ELx_EC_ERET]	= kvm_handle_eret,
	[ESR_ELx_EC_IABT_LOW]	= kvm_handle_guest_abort,
	[ESR_ELx_EC_DABT_LOW]	= kvm_handle_guest_abort,
	[ESR_ELx_EC_SOFTSTP_LOW]= kvm_handle_guest_debug,
	[ESR_ELx_EC_WATCHPT_LOW]= kvm_handle_guest_debug,
	[ESR_ELx_EC_BREAKPT_LOW]= kvm_handle_guest_debug,
	[ESR_ELx_EC_BKPT32]	= kvm_handle_guest_debug,
	[ESR_ELx_EC_BRK64]	= kvm_handle_guest_debug,
	[ESR_ELx_EC_FP_ASIMD]	= kvm_handle_fpasimd,
};

static exit_handle_fn kvm_get_exit_handler(struct kvm_vcpu *vcpu)
{
	u32 hsr = kvm_vcpu_get_hsr(vcpu);
	u8 hsr_ec = ESR_ELx_EC(hsr);

	return arm_exit_handlers[hsr_ec];
}

/*
 * Return > 0 to return to guest, < 0 on error, 0 (and set exit_reason) on
 * proper exit to userspace.
 */
int handle_exit(struct kvm_vcpu *vcpu, struct kvm_run *run,
		       int exception_index)
{
	exit_handle_fn exit_handler;

	if (ARM_SERROR_PENDING(exception_index)) {
		u8 hsr_ec = ESR_ELx_EC(kvm_vcpu_get_hsr(vcpu));

		/*
		 * HVC/SMC already have an adjusted PC, which we need
		 * to correct in order to return to after having
		 * injected the SError.
		 */
		if (hsr_ec == ESR_ELx_EC_HVC32 || hsr_ec == ESR_ELx_EC_HVC64 ||
		    hsr_ec == ESR_ELx_EC_SMC32 || hsr_ec == ESR_ELx_EC_SMC64) {
			u32 adj =  kvm_vcpu_trap_il_is32bit(vcpu) ? 4 : 2;
			*vcpu_pc(vcpu) -= adj;
		}

		kvm_inject_vabt(vcpu);
		return 1;
	}

	exception_index = ARM_EXCEPTION_CODE(exception_index);

	switch (exception_index) {
	case ARM_EXCEPTION_IRQ:
		return 1;
	case ARM_EXCEPTION_EL1_SERROR:
		kvm_inject_vabt(vcpu);
		return 1;
	case ARM_EXCEPTION_TRAP:
		/*
		 * See ARM ARM B1.14.1: "Hyp traps on instructions
		 * that fail their condition code check"
		 */
		if (!kvm_condition_valid(vcpu)) {
			kvm_skip_instr(vcpu, kvm_vcpu_trap_il_is32bit(vcpu));
			return 1;
		}

		exit_handler = kvm_get_exit_handler(vcpu);

		return exit_handler(vcpu, run);
	case ARM_EXCEPTION_HYP_GONE:
		/*
		 * EL2 has been reset to the hyp-stub. This happens when a guest
		 * is pre-empted by kvm_reboot()'s shutdown call.
		 */
		run->exit_reason = KVM_EXIT_FAIL_ENTRY;
		return 0;
	default:
		kvm_pr_unimpl("Unsupported exception type: %d",
			      exception_index);
		run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
		return 0;
	}
}
