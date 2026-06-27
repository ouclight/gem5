/*
 * Copyright 2007 The Hewlett-Packard Development Company
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright 2020 Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "arch/x86/linux/se_workload.hh"

#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <sys/syscall.h>

#include "arch/x86/linux/linux.hh"
#include "arch/x86/page_size.hh"
#include "arch/x86/process.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/se_workload.hh"
#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "kern/linux/linux.hh"
#include "mem/se_translating_port_proxy.hh"
#include "sim/process.hh"
#include "sim/syscall_desc.hh"
#include "sim/syscall_emul.hh"

namespace gem5
{

namespace
{

class LinuxLoader : public Process::Loader
{
  public:
    Process *
    load(const ProcessParams &params, loader::ObjectFile *obj_file)
    {
        auto arch = obj_file->getArch();
        auto opsys = obj_file->getOpSys();

        if (arch != loader::X86_64 && arch != loader::I386)
            return nullptr;

        if (opsys == loader::UnknownOpSys) {
            warn("Unknown operating system; assuming Linux.");
            opsys = loader::Linux;
        }

        if (opsys != loader::Linux)
            return nullptr;

        if (arch == loader::X86_64)
            return new X86ISA::X86_64Process(params, obj_file);
        else
            return new X86ISA::I386Process(params, obj_file);
    }
};

LinuxLoader linuxLoader;

} // anonymous namespace

namespace X86ISA
{

EmuLinux::EmuLinux(const Params &p) : SEWorkload(p, PageShift)
{}

const std::vector<RegId> EmuLinux::SyscallABI64::ArgumentRegs = {
    int_reg::Rdi, int_reg::Rsi, int_reg::Rdx,
    int_reg::R10, int_reg::R8, int_reg::R9
};

const std::vector<RegId> EmuLinux::SyscallABI32::ArgumentRegs = {
    int_reg::Ebx, int_reg::Ecx, int_reg::Edx,
    int_reg::Esi, int_reg::Edi, int_reg::Ebp
};

void
EmuLinux::syscall(ThreadContext *tc)
{
    Process *process = tc->getProcessPtr();
    // Call the syscall function in the base Process class to update stats.
    // This will move into the base SEWorkload function at some point.
    process->Process::syscall(tc);

    RegVal rax = tc->getReg(int_reg::Rax);
    if (dynamic_cast<X86_64Process *>(process)) {
        syscallDescs64.get(rax)->doSyscall(tc);
    } else if (auto *proc32 = dynamic_cast<I386Process *>(process)) {
        PCState pc = tc->pcState().as<PCState>();
        Addr eip = pc.pc();
        const auto &vsyscall = proc32->getVSyscallPage();
        if (eip >= vsyscall.base && eip < vsyscall.base + vsyscall.size) {
            pc.set(vsyscall.base + vsyscall.vsysexitOffset);
            tc->pcState(pc);
        }
        syscallDescs32.get(rax)->doSyscall(tc);
    } else {
        panic("Unrecognized process type.");
    }
}

void
EmuLinux::event(ThreadContext *tc)
{
    Process *process = tc->getProcessPtr();
    Addr pc = tc->pcState().instAddr();

    if (process->kvmInSE) {
        Addr pc_page = mbits(pc, 63, 12);
        if (pc_page == syscallCodeVirtAddr) {
            syscall(tc);
            return;
        } else if (pc_page == PFHandlerVirtAddr) {
            pageFault(tc);
            return;
        }
    }
    warn("Unexpected workload event at pc %#x.", pc);
}

void
EmuLinux::pageFault(ThreadContext *tc)
{
    Process *p = tc->getProcessPtr();
    const Addr fault_addr = tc->readMiscReg(misc_reg::Cr2);
    const bool mapped_before_fixup = p->pTable->lookup(fault_addr);
    SETranslatingPortProxy proxy(tc);
    // At this point we should have 6 values on the interrupt stack.
    constexpr int interrupt_stack_entries = 6;
    const size_t interrupt_stack_bytes =
        sizeof(uint64_t) * interrupt_stack_entries;
    auto is = std::make_unique<uint64_t[]>(interrupt_stack_entries);
    proxy.readBlob(ISTVirtAddr + PageBytes - interrupt_stack_bytes,
                   is.get(), interrupt_stack_bytes);

    if (tc->contextId() != 0) {
        std::ostringstream trace;
        trace << "KVM SE page fault: context=" << tc->contextId()
              << " pid=" << p->pid()
              << std::hex << std::showbase
              << " addr=" << fault_addr
              << " rip=" << is[1]
              << " rbx=" << tc->getReg(int_reg::Rbx)
              << " rsp=" << is[4]
              << " fs_base="
              << tc->readMiscRegNoEffect(misc_reg::FsBase)
              << "\n";
        inform("%s", trace.str());
    }

    if (!p->fixupFault(fault_addr)) {
        std::array<uint8_t, 16> instruction_bytes;
        proxy.readBlob(is[1], instruction_bytes.data(),
                       instruction_bytes.size());
        uint64_t start_thread_saved_rdi = 0;
        uint64_t start_thread_saved_rbx = 0;
        std::array<uint64_t, 2> original_child_stack = {};
        proxy.readBlob(is[4] + 0x8, &start_thread_saved_rdi,
                       sizeof(start_thread_saved_rdi));
        proxy.readBlob(is[4] + 0xa0, &start_thread_saved_rbx,
                       sizeof(start_thread_saved_rbx));
        proxy.readBlob(is[4] + 0xb0, original_child_stack.data(),
                       sizeof(original_child_stack));
        const auto read_kvm_backing = [p](Addr vaddr) {
            Addr paddr = 0;
            if (!p->pTable->translate(vaddr, paddr))
                return uint64_t{0};

            for (const auto &backing :
                    p->system->getPhysMem().getBackingStore()) {
                if (!backing.kvmMap || !backing.range.contains(paddr))
                    continue;

                uint64_t value = 0;
                const Addr offset = paddr - backing.range.start();
                std::memcpy(&value, backing.pmem + offset, sizeof(value));
                return value;
            }
            return uint64_t{0};
        };
        const uint64_t kvm_start_thread_saved_rdi =
            read_kvm_backing(is[4] + 0x8);
        const uint64_t kvm_start_thread_saved_rbx =
            read_kvm_backing(is[4] + 0xa0);
        const uint64_t kvm_original_child_function =
            read_kvm_backing(is[4] + 0xb0);
        const uint64_t kvm_original_child_argument =
            read_kvm_backing(is[4] + 0xb8);
        std::ostringstream diagnostic;
        diagnostic << std::hex << std::showbase
                   << "Page fault at addr " << fault_addr
                   << "\n\tInterrupt handler stack:"
                   << "\n\tss: " << is[5]
                   << "\n\trsp: " << is[4]
                   << "\n\trflags: " << is[3]
                   << "\n\tcs: " << is[2]
                   << "\n\trip: " << is[1]
                   << "\n\terr_code: " << is[0]
                   << std::dec
                   << "\n\tcontext_id: " << tc->contextId()
                   << "\n\tpid: " << p->pid()
                   << "\n\tpte_mapped_before_fixup: "
                   << mapped_before_fixup
                   << std::hex
                   << "\n\tcr3: " << tc->readMiscReg(misc_reg::Cr3)
                   << "\n\tfs_base: "
                   << tc->readMiscRegNoEffect(misc_reg::FsBase)
                   << "\n\tgs_base: "
                   << tc->readMiscRegNoEffect(misc_reg::GsBase)
                   << "\n\trax: " << tc->getReg(int_reg::Rax)
                   << "\n\trbx: " << tc->getReg(int_reg::Rbx)
                   << "\n\trcx: " << tc->getReg(int_reg::Rcx)
                   << "\n\trdx: " << tc->getReg(int_reg::Rdx)
                   << "\n\trbp: " << tc->getReg(int_reg::Rbp)
                   << "\n\trsp: " << tc->getReg(int_reg::Rsp)
                   << "\n\trdi: " << tc->getReg(int_reg::Rdi)
                   << "\n\trsi: " << tc->getReg(int_reg::Rsi)
                   << "\n\tr8: " << tc->getReg(int_reg::R8)
                   << "\n\tr9: " << tc->getReg(int_reg::R9)
                   << "\n\tr10: " << tc->getReg(int_reg::R10)
                   << "\n\tr11: " << tc->getReg(int_reg::R11)
                   << "\n\tr12: " << tc->getReg(int_reg::R12)
                   << "\n\tr13: " << tc->getReg(int_reg::R13)
                   << "\n\tr14: " << tc->getReg(int_reg::R14)
                   << "\n\tr15: " << tc->getReg(int_reg::R15)
                   << "\n\tstart_thread_saved_rdi: "
                   << start_thread_saved_rdi
                   << "\n\tstart_thread_saved_rbx: "
                   << start_thread_saved_rbx
                   << "\n\toriginal_child_function: "
                   << original_child_stack[0]
                   << "\n\toriginal_child_argument: "
                   << original_child_stack[1]
                   << "\n\tkvm_start_thread_saved_rdi: "
                   << kvm_start_thread_saved_rdi
                   << "\n\tkvm_start_thread_saved_rbx: "
                   << kvm_start_thread_saved_rbx
                   << "\n\tkvm_original_child_function: "
                   << kvm_original_child_function
                   << "\n\tkvm_original_child_argument: "
                   << kvm_original_child_argument
                   << "\n\tinstruction_bytes:";
        diagnostic << std::noshowbase << std::setfill('0');
        for (const auto byte : instruction_bytes) {
            diagnostic << " " << std::setw(2)
                       << static_cast<unsigned int>(byte);
        }
        diagnostic << "\n\tvmas:\n" << p->memState->printVmaList();
        panic("%s", diagnostic.str());
   }
}

} // namespace X86ISA
} // namespace gem5
