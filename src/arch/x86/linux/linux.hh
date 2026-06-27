/*
 * Copyright (c) 2024 Arm Limited
 * Copyright (c) 2007 The Hewlett-Packard Development Company
 * All rights reserved.
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

#ifndef __ARCH_X86_LINUX_LINUX_HH__
#define __ARCH_X86_LINUX_LINUX_HH__

#include <array>
#include <iomanip>
#include <map>
#include <sstream>

#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/utility.hh"
#include "base/compiler.hh"
#include "base/logging.hh"
#include "kern/linux/flag_tables.hh"
#include "kern/linux/linux.hh"
#include "mem/se_translating_port_proxy.hh"
#include "sim/guest_abi.hh"
#include "sim/process.hh"
#include "sim/system.hh"
#include "sim/syscall_return.hh"

namespace gem5
{

class X86Linux : public Linux
{
  public:
    static const ByteOrder byteOrder = ByteOrder::little;

    static void
    archClone(uint64_t flags,
                          Process *pp, Process *cp,
                          ThreadContext *ptc, ThreadContext *ctc,
                          uint64_t stack, uint64_t tls)
    {
        ctc->getIsaPtr()->copyRegsFrom(ptc);

        if (flags & TGT_CLONE_SETTLS) {
            ctc->setMiscRegNoEffect(X86ISA::misc_reg::FsBase, tls);
            ctc->setMiscRegNoEffect(X86ISA::misc_reg::FsEffBase, tls);
        }

        if (stack)
            ctc->setReg(X86ISA::int_reg::Rsp, stack);

        if (pp->kvmInSE && stack) {
            constexpr uint64_t address_mask = 0x000ffffffffff000ULL;
            const std::array<std::pair<int, int>, 4> index_bits = {{
                {47, 39}, {38, 30}, {29, 21}, {20, 12}
            }};
            std::array<uint64_t, 4> entries = {};
            std::array<Addr, 4> entry_addrs = {};
            Addr table = ctc->readMiscRegNoEffect(
                X86ISA::misc_reg::Cr3) & address_mask;
            bool walk_valid = true;

            for (size_t level = 0; level < index_bits.size(); ++level) {
                const auto [high, low] = index_bits[level];
                entry_addrs[level] =
                    table + bits(stack, high, low) * sizeof(uint64_t);
                entries[level] =
                    pp->system->physProxy.read<uint64_t>(entry_addrs[level]);
                if (!(entries[level] & 1)) {
                    walk_valid = false;
                    break;
                }
                table = entries[level] & address_mask;
            }

            const Addr walked_paddr =
                walk_valid ? table + bits(stack, 11, 0) : 0;
            std::array<uint64_t, 2> walked_words = {};
            if (walk_valid) {
                pp->system->physProxy.readBlob(
                    walked_paddr, walked_words.data(),
                    sizeof(walked_words));
            }
            std::array<uint8_t, 24> clone_return_bytes = {};
            SETranslatingPortProxy(ptc).readBlob(
                ptc->getReg(X86ISA::int_reg::Rcx),
                clone_return_bytes.data(), clone_return_bytes.size());

            std::ostringstream trace;
            trace << std::hex << std::showbase
                  << "KVM SE hardware page walk: cr3="
                  << ctc->readMiscRegNoEffect(X86ISA::misc_reg::Cr3)
                  << " stack=" << stack
                  << " parent_pc=" << ptc->pcState().instAddr()
                  << " parent_rcx="
                  << ptc->getReg(X86ISA::int_reg::Rcx)
                  << " parent_rax="
                  << ptc->getReg(X86ISA::int_reg::Rax)
                  << " parent_rdi="
                  << ptc->getReg(X86ISA::int_reg::Rdi)
                  << " child_pc=" << ctc->pcState().instAddr()
                  << " child_rcx="
                  << ctc->getReg(X86ISA::int_reg::Rcx)
                  << " child_rax="
                  << ctc->getReg(X86ISA::int_reg::Rax)
                  << " child_rdi="
                  << ctc->getReg(X86ISA::int_reg::Rdi)
                  << " valid=" << std::dec << walk_valid
                  << std::hex;
            for (size_t level = 0; level < entries.size(); ++level) {
                trace << " l" << (4 - level) << "_addr="
                      << entry_addrs[level]
                      << " l" << (4 - level) << "_entry="
                      << entries[level];
            }
            trace << " walked_paddr=" << walked_paddr
                  << " walked_function=" << walked_words[0]
                  << " walked_argument=" << walked_words[1]
                  << " clone_return_bytes=";
            trace << std::noshowbase << std::setfill('0');
            for (const auto byte : clone_return_bytes) {
                trace << std::setw(2)
                      << static_cast<unsigned int>(byte);
            }
            trace << "\n";
            inform("%s", trace.str());
        }
    }

    class SyscallABI {};
};

namespace guest_abi
{

template <typename ABI>
struct Result<ABI, SyscallReturn,
    typename std::enable_if_t<std::is_base_of_v<X86Linux::SyscallABI, ABI>>>
{
    static void
    store(ThreadContext *tc, const SyscallReturn &ret)
    {
        tc->setReg(X86ISA::int_reg::Rax, ret.encodedValue());
    }
};

} // namespace guest_abi

class X86Linux64 : public X86Linux, public OpenFlagTable<X86Linux64>
{
  public:

    struct tgt_stat64
    {
        uint64_t st_dev;
        uint64_t st_ino;
        uint64_t st_nlink;
        uint32_t st_mode;
        uint32_t st_uid;
        uint32_t st_gid;
        uint32_t __pad0;
        uint64_t st_rdev;
        int64_t st_size;
        int64_t st_blksize;
        int64_t st_blocks;
        uint64_t st_atimeX;
        uint64_t st_atime_nsec;
        uint64_t st_mtimeX;
        uint64_t st_mtime_nsec;
        uint64_t st_ctimeX;
        uint64_t st_ctime_nsec;
        int64_t unused0[3];
    };

    struct tgt_statx
    {
        /* 0x00 */
        uint32_t stx_mask;
        uint32_t stx_blksize;
        uint64_t stx_attributes;
        /* 0x10 */
        uint32_t stx_nlink;
        uint32_t stx_uid;
        uint32_t stx_gid;
        uint16_t stx_mode;
        uint16_t stx_spare0;
        /* 0x20 */
        uint64_t stx_ino;
        uint64_t stx_size;
        uint64_t stx_blocks;
        uint64_t stx_attributes_mask;
        /* 0x40 */
        uint64_t stx_atimeX;
        uint32_t stx_atime_nsec;
        int32_t  stx_atime_reserved;
        uint64_t stx_btimeX;
        uint32_t stx_btime_nsec;
        int32_t  stx_btime_reserved;
        uint64_t stx_ctimeX;
        uint32_t stx_ctime_nsec;
        int32_t  stx_ctime_reserved;
        uint64_t stx_mtimeX;
        uint32_t stx_mtime_nsec;
        int32_t  stx_mtime_reserved;
        /* 0x80 */
        uint32_t stx_rdev_major;
        uint32_t stx_rdev_minor;
        uint32_t stx_dev_major;
        uint32_t stx_dev_minor;
        /* 0x90 */
        uint64_t stx_mnt_id;
        uint64_t stx_spare2;
        /* 0xa0 */
        uint64_t stx_spare3[12];
        /* 0x100 */
    };

    struct tgt_fsid
    {
        long val[2];
    };

    struct tgt_statfs
    {
        long f_type;
        long f_bsize;
        long f_blocks;
        long f_bfree;
        long f_bavail;
        long f_files;
        long f_ffree;
        tgt_fsid f_fsid;
        long f_namelen;
        long f_frsize;
        long f_spare[5];
    };

    static const int TGT_SIGHUP         = 0x000001;
    static const int TGT_SIGINT         = 0x000002;
    static const int TGT_SIGQUIT        = 0x000003;
    static const int TGT_SIGILL         = 0x000004;
    static const int TGT_SIGTRAP        = 0x000005;
    static const int TGT_SIGABRT        = 0x000006;
    static const int TGT_SIGIOT         = 0x000006;
    static const int TGT_SIGBUS         = 0x000007;
    static const int TGT_SIGFPE         = 0x000008;
    static const int TGT_SIGKILL        = 0x000009;
    static const int TGT_SIGUSR1        = 0x00000a;
    static const int TGT_SIGSEGV        = 0x00000b;
    static const int TGT_SIGUSR2        = 0x00000c;
    static const int TGT_SIGPIPE        = 0x00000d;
    static const int TGT_SIGALRM        = 0x00000e;
    static const int TGT_SIGTERM        = 0x00000f;
    static const int TGT_SIGSTKFLT      = 0x000010;
    static const int TGT_SIGCHLD        = 0x000011;
    static const int TGT_SIGCONT        = 0x000012;
    static const int TGT_SIGSTOP        = 0x000013;
    static const int TGT_SIGTSTP        = 0x000014;
    static const int TGT_SIGTTIN        = 0x000015;
    static const int TGT_SIGTTOU        = 0x000016;
    static const int TGT_SIGURG         = 0x000017;
    static const int TGT_SIGXCPU        = 0x000018;
    static const int TGT_SIGXFSZ        = 0x000019;
    static const int TGT_SIGVTALRM      = 0x00001a;
    static const int TGT_SIGPROF        = 0x00001b;
    static const int TGT_SIGWINCH       = 0x00001c;
    static const int TGT_SIGIO          = 0x00001d;
    static const int TGT_SIGPOLL        = 0x00001d;
    static const int TGT_SIGPWR         = 0x00001e;
    static const int TGT_SIGSYS         = 0x00001f;
    static const int TGT_SIGUNUSED      = 0x00001f;

    static constexpr int TGT_O_RDONLY       = 000000000;     //!< O_RDONLY
    static constexpr int TGT_O_WRONLY       = 000000001;     //!< O_WRONLY
    static constexpr int TGT_O_RDWR         = 000000002;     //!< O_RDWR
    static constexpr int TGT_O_CREAT        = 000000100;     //!< O_CREAT
    static constexpr int TGT_O_EXCL         = 000000200;     //!< O_EXCL
    static constexpr int TGT_O_NOCTTY       = 000000400;     //!< O_NOCTTY
    static constexpr int TGT_O_TRUNC        = 000001000;     //!< O_TRUNC
    static constexpr int TGT_O_APPEND       = 000002000;     //!< O_APPEND
    static constexpr int TGT_O_NONBLOCK     = 000004000;     //!< O_NONBLOCK
    static constexpr int TGT_O_DSYNC        = 000010000;
    static constexpr int TGT_FASYNC         = 000020000;
    static constexpr int TGT_O_DIRECT       = 000040000;     //!< O_DIRECTIO
    static constexpr int TGT_O_LARGEFILE    = 000100000;
    static constexpr int TGT_O_DIRECTORY    = 000200000;
    static constexpr int TGT_O_NOFOLLOW     = 000400000;
    static constexpr int TGT_O_NOATIME      = 001000000;
    static constexpr int TGT_O_CLOEXEC      = 002000000;
    static constexpr int TGT_O_SYNC         = 004010000;     //!< O_SYNC
    static constexpr int TGT_O_PATH         = 010000000;

    //@{
    /// Basic X86_64 Linux types
    typedef uint64_t size_t;
    typedef int64_t off_t;
    typedef int64_t time_t;
    typedef int64_t clock_t;
    //@}

    static constexpr unsigned TGT_MAP_SHARED        = 0x00001;
    static constexpr unsigned TGT_MAP_PRIVATE       = 0x00002;
    static constexpr unsigned TGT_MAP_32BIT         = 0x00040;
    static constexpr unsigned TGT_MAP_ANON          = 0x00020;
    static constexpr unsigned TGT_MAP_DENYWRITE     = 0x00800;
    static constexpr unsigned TGT_MAP_EXECUTABLE    = 0x01000;
    static constexpr unsigned TGT_MAP_FILE          = 0x00000;
    static constexpr unsigned TGT_MAP_GROWSDOWN     = 0x00100;
    static constexpr unsigned TGT_MAP_HUGETLB       = 0x40000;
    static constexpr unsigned TGT_MAP_LOCKED        = 0x02000;
    static constexpr unsigned TGT_MAP_NONBLOCK      = 0x10000;
    static constexpr unsigned TGT_MAP_NORESERVE     = 0x04000;
    static constexpr unsigned TGT_MAP_POPULATE      = 0x08000;
    static constexpr unsigned TGT_MAP_STACK         = 0x20000;
    static constexpr unsigned TGT_MAP_ANONYMOUS     = 0x00020;
    static constexpr unsigned TGT_MAP_FIXED         = 0x00010;

    struct tgt_iovec
    {
        uint64_t iov_base; // void *
        uint64_t iov_len;  // size_t
    };

    struct tgt_sysinfo
    {
        int64_t  uptime;    /* Seconds since boot */
        uint64_t loads[3];  /* 1, 5, and 15 minute load averages */
        uint64_t totalram;  /* Total usable main memory size */
        uint64_t freeram;   /* Available memory size */
        uint64_t sharedram; /* Amount of shared memory */
        uint64_t bufferram; /* Memory used by buffers */
        uint64_t totalswap; /* Total swap space size */
        uint64_t freeswap;  /* swap space still available */
        uint16_t procs;     /* Number of current processes */
        uint64_t totalhigh; /* Total high memory size */
        uint64_t freehigh;  /* Available high memory size */
        uint64_t mem_unit;  /* Memory unit size in bytes */
    };

    struct tgt_clone_args
    {
        uint64_t flags;
        uint64_t pidfd;
        uint64_t child_tid;
        uint64_t parent_tid;
        uint64_t exit_signal;
        uint64_t stack;
        uint64_t stack_size;
        uint64_t tls;
        uint64_t set_tid;
        uint64_t set_tid_size;
        uint64_t cgroup;
    };

};

class X86Linux32 : public X86Linux, public OpenFlagTable<X86Linux32>
{
  public:
    struct GEM5_PACKED tgt_stat64
    {
        uint64_t st_dev;
        uint8_t __pad0[4];
        uint32_t __st_ino;
        uint32_t st_mode;
        uint32_t st_nlink;
        uint32_t st_uid;
        uint32_t st_gid;
        uint64_t st_rdev;
        uint8_t __pad3[4];
        int64_t st_size;
        uint32_t st_blksize;
        uint64_t st_blocks;
        uint32_t st_atimeX;
        uint32_t st_atime_nsec;
        uint32_t st_mtimeX;
        uint32_t st_mtime_nsec;
        uint32_t st_ctimeX;
        uint32_t st_ctime_nsec;
        uint64_t st_ino;
    };

    static const int TGT_SIGHUP         = 0x000001;
    static const int TGT_SIGINT         = 0x000002;
    static const int TGT_SIGQUIT        = 0x000003;
    static const int TGT_SIGILL         = 0x000004;
    static const int TGT_SIGTRAP        = 0x000005;
    static const int TGT_SIGABRT        = 0x000006;
    static const int TGT_SIGIOT         = 0x000006;
    static const int TGT_SIGBUS         = 0x000007;
    static const int TGT_SIGFPE         = 0x000008;
    static const int TGT_SIGKILL        = 0x000009;
    static const int TGT_SIGUSR1        = 0x00000a;
    static const int TGT_SIGSEGV        = 0x00000b;
    static const int TGT_SIGUSR2        = 0x00000c;
    static const int TGT_SIGPIPE        = 0x00000d;
    static const int TGT_SIGALRM        = 0x00000e;
    static const int TGT_SIGTERM        = 0x00000f;
    static const int TGT_SIGSTKFLT      = 0x000010;
    static const int TGT_SIGCHLD        = 0x000011;
    static const int TGT_SIGCONT        = 0x000012;
    static const int TGT_SIGSTOP        = 0x000013;
    static const int TGT_SIGTSTP        = 0x000014;
    static const int TGT_SIGTTIN        = 0x000015;
    static const int TGT_SIGTTOU        = 0x000016;
    static const int TGT_SIGURG         = 0x000017;
    static const int TGT_SIGXCPU        = 0x000018;
    static const int TGT_SIGXFSZ        = 0x000019;
    static const int TGT_SIGVTALRM      = 0x00001a;
    static const int TGT_SIGPROF        = 0x00001b;
    static const int TGT_SIGWINCH       = 0x00001c;
    static const int TGT_SIGIO          = 0x00001d;
    static const int TGT_SIGPOLL        = 0x00001d;
    static const int TGT_SIGPWR         = 0x00001e;
    static const int TGT_SIGSYS         = 0x00001f;
    static const int TGT_SIGUNUSED      = 0x00001f;

    static constexpr int TGT_O_RDONLY       = 000000000;     //!< O_RDONLY
    static constexpr int TGT_O_WRONLY       = 000000001;     //!< O_WRONLY
    static constexpr int TGT_O_RDWR         = 000000002;     //!< O_RDWR
    static constexpr int TGT_O_CREAT        = 000000100;     //!< O_CREAT
    static constexpr int TGT_O_EXCL         = 000000200;     //!< O_EXCL
    static constexpr int TGT_O_NOCTTY       = 000000400;     //!< O_NOCTTY
    static constexpr int TGT_O_TRUNC        = 000001000;     //!< O_TRUNC
    static constexpr int TGT_O_APPEND       = 000002000;     //!< O_APPEND
    static constexpr int TGT_O_NONBLOCK     = 000004000;     //!< O_NONBLOCK
    static constexpr int TGT_O_DSYNC        = 000010000;     //!< O_DSYNC
    static constexpr int TGT_FASYNC         = 000020000;
    static constexpr int TGT_O_DIRECT       = 000040000;     //!< O_DIRECTIO
    static constexpr int TGT_O_LARGEFILE    = 000100000;
    static constexpr int TGT_O_DIRECTORY    = 000200000;
    static constexpr int TGT_O_NOFOLLOW     = 000400000;
    static constexpr int TGT_O_NOATIME      = 001000000;
    static constexpr int TGT_O_CLOEXEC      = 002000000;
    static constexpr int TGT_O_SYNC         = 004010000;     //!< O_SYNC
    static constexpr int TGT_O_PATH         = 010000000;

    static const std::map<int, int> mmapFlagTable;

    //@{
    /// Basic X86 Linux types
    typedef uint32_t size_t;
    typedef int32_t off_t;
    typedef int32_t time_t;
    typedef int32_t clock_t;
    //@}

    static constexpr unsigned TGT_MAP_SHARED        = 0x00001;
    static constexpr unsigned TGT_MAP_PRIVATE       = 0x00002;
    static constexpr unsigned TGT_MAP_32BIT         = 0x00040;
    static constexpr unsigned TGT_MAP_ANON          = 0x00020;
    static constexpr unsigned TGT_MAP_DENYWRITE     = 0x00800;
    static constexpr unsigned TGT_MAP_EXECUTABLE    = 0x01000;
    static constexpr unsigned TGT_MAP_FILE          = 0x00000;
    static constexpr unsigned TGT_MAP_GROWSDOWN     = 0x00100;
    static constexpr unsigned TGT_MAP_HUGETLB       = 0x40000;
    static constexpr unsigned TGT_MAP_LOCKED        = 0x02000;
    static constexpr unsigned TGT_MAP_NONBLOCK      = 0x10000;
    static constexpr unsigned TGT_MAP_NORESERVE     = 0x04000;
    static constexpr unsigned TGT_MAP_POPULATE      = 0x08000;
    static constexpr unsigned TGT_MAP_STACK         = 0x20000;
    static constexpr unsigned TGT_MAP_ANONYMOUS     = 0x00020;
    static constexpr unsigned TGT_MAP_FIXED         = 0x00010;

    struct tgt_sysinfo
    {
       int32_t  uptime;    /* Seconds since boot */
       uint32_t loads[3];  /* 1, 5, and 15 minute load averages */
       uint32_t totalram;  /* Total usable main memory size */
       uint32_t freeram;   /* Available memory size */
       uint32_t sharedram; /* Amount of shared memory */
       uint32_t bufferram; /* Memory used by buffers */
       uint32_t totalswap; /* Total swap space size */
       uint32_t freeswap;  /* swap space still available */
       uint16_t procs;     /* Number of current processes */
       uint32_t totalhigh; /* Total high memory size */
       uint32_t freehigh;  /* Available high memory size */
       uint32_t mem_unit;  /* Memory unit size in bytes */
    };
};

} // namespace gem5

#endif
