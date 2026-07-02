/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#include "dev/hsa/se_sdma_engine.hh"

#include <algorithm>
#include <cstring>
#include <memory>

#include "base/amo.hh"
#include "base/bitfield.hh"
#include "debug/SESDMAEngine.hh"

#include "base/logging.hh"
#include "dev/amdgpu/sdma_commands.hh"
#include "dev/amdgpu/sdma_packets.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/system.hh"

namespace gem5
{

SESDMAEngine::SESDMAEngine(const Params &p)
    : DmaVirtDevice(p), _gpuId(p.gpuId)
{
    DPRINTF(SESDMAEngine, "Created SE SDMA engine for gpu_id %u\n", _gpuId);
}

void
SESDMAEngine::registerQueue(uint64_t queue_id, Addr read_ptr, Addr write_ptr,
                            Addr ring_base, uint32_t ring_size,
                            int doorbell_size)
{
    DPRINTF(SESDMAEngine,
            "Register SDMA queue %lu gpu_id %u ring %#lx size %#x "
            "read_ptr %#lx write_ptr %#lx doorbell %d\n",
            queue_id, _gpuId, ring_base, ring_size, read_ptr, write_ptr,
            doorbell_size);

    QueueDesc desc;
    desc.readPtr = read_ptr;
    desc.writePtr = write_ptr;
    desc.ringBase = ring_base;
    desc.ringSize = ring_size;
    desc.doorbellSize = doorbell_size;
    queues[queue_id] = desc;

    auto *cb = new DmaVirtCallback<uint32_t>(
        [ = ] (const uint32_t &first_dword)
        {
            DPRINTF(SESDMAEngine,
                    "Register SDMA queue %lu gpu_id %u first_ring_dword "
                    "%#x ring %#lx\n",
                    queue_id, _gpuId, first_dword,
                    static_cast<uint64_t>(ring_base));
        });
    dmaReadVirt(ring_base, sizeof(uint32_t), cb, &cb->dmaBuffer);
}

void
SESDMAEngine::unregisterQueue(uint64_t queue_id)
{
    DPRINTF(SESDMAEngine, "Unregister SDMA queue %lu gpu_id %u\n",
            queue_id, _gpuId);
    queues.erase(queue_id);
}

void
SESDMAEngine::writeDoorbell(uint64_t queue_id, uint64_t write_index)
{
    auto it = queues.find(queue_id);
    fatal_if(it == queues.end(),
             "Doorbell for unregistered SE SDMA queue %lu on gpu_id %u\n",
             queue_id, _gpuId);

    DPRINTF(SESDMAEngine,
            "SDMA doorbell queue %lu gpu_id %u raw_write_index %lu\n",
            queue_id, _gpuId, write_index);

    auto &queue = it->second;
    queue.lastDoorbellIndex = write_index;
    auto *cb = new DmaVirtCallback<uint64_t>(
        [ = ] (const uint64_t &queue_write_index)
        { updateWriteIndex(queue_id, queue_write_index); });
    dmaReadVirt(queue.writePtr, sizeof(uint64_t), cb, &cb->dmaBuffer);
}

TranslationGenPtr
SESDMAEngine::translate(Addr vaddr, Addr size)
{
    fatal_if(FullSystem, "SESDMAEngine is only valid in SE mode\n");
    auto process = sys->threads[0]->getProcessPtr();
    return process->pTable->translateRange(vaddr, size);
}

AddrRangeList
SESDMAEngine::getAddrRanges() const
{
    return {};
}

Tick
SESDMAEngine::read(PacketPtr pkt)
{
    pkt->makeAtomicResponse();
    pkt->setBadAddress();
    return 0;
}

Tick
SESDMAEngine::write(PacketPtr pkt)
{
    pkt->makeAtomicResponse();
    pkt->setBadAddress();
    return 0;
}

Addr
SESDMAEngine::ringAddr(const QueueDesc &queue) const
{
    return queue.ringBase + queue.readIndex;
}

void
SESDMAEngine::dmaReadAddr(Addr addr, unsigned size, DmaCallback *cb,
                          void *data)
{
    if (size == 0) {
        if (cb) {
            schedule(cb->getChunkEvent(), curTick());
        }
        return;
    }

    TranslationGenPtr gen = translate(addr, size);
    bool translated = true;
    for (const auto &range: *gen) {
        if (range.fault) {
            translated = false;
            break;
        }
    }

    if (translated) {
        dmaReadVirt(addr, size, cb, data);
    } else {
        DPRINTF(SESDMAEngine,
                "SE SDMA direct physical read addr %#lx size %u\n",
                addr, size);
        dmaRead(addr, size, cb ? cb->getChunkEvent() : nullptr,
                static_cast<uint8_t *>(data));
    }
}

void
SESDMAEngine::dmaWriteAddr(Addr addr, unsigned size, DmaCallback *cb,
                           void *data)
{
    if (size == 0) {
        if (cb) {
            schedule(cb->getChunkEvent(), curTick());
        }
        return;
    }

    TranslationGenPtr gen = translate(addr, size);
    bool translated = true;
    for (const auto &range: *gen) {
        if (range.fault) {
            translated = false;
            break;
        }
    }

    if (translated) {
        dmaWriteVirt(addr, size, cb, data);
    } else {
        DPRINTF(SESDMAEngine,
                "SE SDMA direct physical write addr %#lx size %u\n",
                addr, size);
        dmaWrite(addr, size, cb ? cb->getChunkEvent() : nullptr,
                 static_cast<uint8_t *>(data));
    }
}

void
SESDMAEngine::dmaAtomicAddr(Addr addr, unsigned size, DmaCallback *cb,
                            void *data, AtomicOpFunctorPtr atomic_op)
{
    fatal_if(size == 0, "SE SDMA atomic with zero size\n");

    TranslationGenPtr gen = translate(addr, size);
    bool translated = true;
    Addr translated_addr = 0;
    unsigned translated_size = 0;
    int ranges = 0;
    for (const auto &range: *gen) {
        if (range.fault) {
            translated = false;
            break;
        }
        translated_addr = range.paddr;
        translated_size = range.size;
        ranges++;
    }

    if (translated) {
        fatal_if(ranges != 1 || translated_size != size,
                 "SE SDMA atomic crosses translation ranges: addr %#lx "
                 "size %u ranges %d translated_size %u\n",
                 addr, size, ranges, translated_size);
        dmaAtomic(translated_addr, size, cb ? cb->getChunkEvent() : nullptr,
                  static_cast<uint8_t *>(data), std::move(atomic_op));
    } else {
        DPRINTF(SESDMAEngine,
                "SE SDMA direct physical atomic addr %#lx size %u\n",
                addr, size);
        dmaAtomic(addr, size, cb ? cb->getChunkEvent() : nullptr,
                  static_cast<uint8_t *>(data), std::move(atomic_op));
    }
}

void
SESDMAEngine::advanceReadIndex(QueueDesc &queue, uint64_t bytes)
{
    queue.readIndex = (queue.readIndex + bytes) % queue.ringSize;
}

void
SESDMAEngine::updateWriteIndex(uint64_t queue_id, uint64_t write_index)
{
    auto it = queues.find(queue_id);
    fatal_if(it == queues.end(),
             "Updating write index for unknown SE SDMA queue %lu\n",
             queue_id);
    auto &queue = it->second;

    queue.writeIndex = write_index % queue.ringSize;
    DPRINTF(SESDMAEngine,
            "SDMA queue %lu gpu_id %u queue_write_index %lu wrapped %lu "
            "last_doorbell %lu read_index %lu ring_size %#x\n",
            queue_id, _gpuId, write_index, queue.writeIndex,
            queue.lastDoorbellIndex, queue.readIndex, queue.ringSize);

    if (!queue.dumpedDoorbellRing) {
        queue.dumpedDoorbellRing = true;
        dumpRingDwords(queue_id, "doorbell ring base", 0, 96);
    }

    if (!queue.processing) {
        queue.processing = true;
        processQueue(queue_id);
    }
}

void
SESDMAEngine::processQueue(uint64_t queue_id)
{
    auto it = queues.find(queue_id);
    fatal_if(it == queues.end(), "Processing unknown SE SDMA queue %lu\n",
             queue_id);
    auto &queue = it->second;

    if (queue.readIndex == queue.writeIndex) {
        queue.processing = false;
        DPRINTF(SESDMAEngine, "SDMA queue %lu drained at rptr %lu\n",
                queue_id, queue.readIndex);
        return;
    }

    auto *cb = new DmaVirtCallback<uint32_t>(
        [ = ] (const uint32_t &header) { decodeHeader(queue_id, header); });
    dmaReadVirt(ringAddr(queue), sizeof(uint32_t), cb, &cb->dmaBuffer);
}

void
SESDMAEngine::decodeHeader(uint64_t queue_id, uint32_t header)
{
    auto it = queues.find(queue_id);
    fatal_if(it == queues.end(), "Decoding unknown SE SDMA queue %lu\n",
             queue_id);
    auto &queue = it->second;

    if (header == 0) {
        if (!queue.dumpedZeroHeaderRing) {
            queue.dumpedZeroHeaderRing = true;
            const uint64_t dump_offset =
                queue.readIndex >= 64 ? queue.readIndex - 64 : 0;
            dumpRingDwords(queue_id, "zero header neighborhood",
                           dump_offset, 64);
        }
        DPRINTF(SESDMAEngine,
                "SDMA queue %lu saw zero header at rptr %lu; treating as "
                "empty ring space and draining to wptr %lu\n",
                queue_id, queue.readIndex, queue.writeIndex);
        queue.readIndex = queue.writeIndex;
        finishPacket(queue_id);
        return;
    }

    advanceReadIndex(queue, sizeof(uint32_t));

    const int opcode = bits(header, 7, 0);
    const int sub_opcode = bits(header, 15, 8);

    DPRINTF(SESDMAEngine,
            "SDMA queue %lu header %#x opcode %#x sub_opcode %#x\n",
            queue_id, header, opcode, sub_opcode);

    switch (opcode) {
      case SDMA_OP_NOP:
        advanceReadIndex(queue, ((header >> 16) & 0x3fff) * sizeof(uint32_t));
        finishPacket(queue_id);
        break;
      case SDMA_OP_FENCE: {
        auto *cb = new DmaVirtCallback<sdmaFence>(
            [ = ] (const sdmaFence &pkt) { executeFence(queue_id, pkt); },
            sdmaFence{});
        dmaReadVirt(ringAddr(queue), sizeof(sdmaFence), cb, &cb->dmaBuffer);
        break;
      }
      case SDMA_OP_TRAP: {
        auto *cb = new DmaVirtCallback<sdmaTrap>(
            [ = ] (const sdmaTrap &pkt) { executeTrap(queue_id, pkt); },
            sdmaTrap{});
        dmaReadVirt(ringAddr(queue), sizeof(sdmaTrap), cb, &cb->dmaBuffer);
        break;
      }
      case SDMA_OP_POLL_REGMEM: {
        auto *cb = new DmaVirtCallback<sdmaPollRegMem>(
            [ = ] (const sdmaPollRegMem &pkt)
            { executePollRegMem(queue_id, header, pkt); },
            sdmaPollRegMem{});
        dmaReadVirt(ringAddr(queue), sizeof(sdmaPollRegMem),
                    cb, &cb->dmaBuffer);
        break;
      }
      case SDMA_OP_WRITE:
        if (sub_opcode != SDMA_SUBOP_WRITE_LINEAR) {
            if (sub_opcode == 0x14) {
                constexpr uint64_t observedWriteVariantBodyDwords = 5;
                DPRINTF(SESDMAEngine,
                        "SDMA WRITE queue %lu unsupported WRITE sub-opcode "
                        "%#x header %#x; diagnostic skip %lu dwords\n",
                        queue_id, sub_opcode, header,
                        observedWriteVariantBodyDwords);
                advanceReadIndex(queue, observedWriteVariantBodyDwords *
                                        sizeof(uint32_t));
                dumpRingDwords(queue_id, "after unsupported WRITE variant",
                               queue.readIndex, 64);
                finishPacket(queue_id);
                break;
            }
            fatal("SE SDMA unsupported WRITE sub-opcode %#x header %#x\n",
                  sub_opcode, header);
        }
        DPRINTF(SESDMAEngine,
                "SDMA WRITE queue %lu sub_opcode %#x, decoding as linear "
                "untiled WRITE packet\n",
                queue_id, sub_opcode);
        {
            auto *cb = new DmaVirtCallback<sdmaWrite>(
                [ = ] (const sdmaWrite &pkt) { executeWrite(queue_id, pkt); },
                sdmaWrite{});
            dmaReadVirt(ringAddr(queue), sizeof(sdmaWrite), cb,
                        &cb->dmaBuffer);
        }
        break;
      case SDMA_OP_ATOMIC: {
        auto *cb = new DmaVirtCallback<sdmaAtomic>(
            [ = ] (const sdmaAtomic &pkt)
            { executeAtomic(queue_id, header, pkt); },
            sdmaAtomic{});
        dmaReadVirt(ringAddr(queue), sizeof(sdmaAtomic),
                    cb, &cb->dmaBuffer);
        break;
      }
      case SDMA_OP_CONST_FILL: {
        auto *cb = new DmaVirtCallback<sdmaConstFill>(
            [ = ] (const sdmaConstFill &pkt)
            { executeConstFill(queue_id, header, pkt); },
            sdmaConstFill{});
        dmaReadVirt(ringAddr(queue), sizeof(sdmaConstFill),
                    cb, &cb->dmaBuffer);
        break;
      }
      case SDMA_OP_COPY:
        if (sub_opcode != SDMA_SUBOP_COPY_LINEAR) {
            fatal("SE SDMA unsupported COPY sub-opcode %#x\n", sub_opcode);
        }
        {
            auto *cb = new DmaVirtCallback<sdmaCopy>(
                [ = ] (const sdmaCopy &pkt) { executeCopy(queue_id, pkt); },
                sdmaCopy{});
            dmaReadVirt(ringAddr(queue), sizeof(sdmaCopy), cb, &cb->dmaBuffer);
        }
        break;
      default:
        fatal("SE SDMA unsupported opcode %#x sub-opcode %#x header %#x\n",
              opcode, sub_opcode, header);
    }
}

void
SESDMAEngine::finishPacket(uint64_t queue_id)
{
    auto it = queues.find(queue_id);
    fatal_if(it == queues.end(), "Finishing unknown SE SDMA queue %lu\n",
             queue_id);
    auto &queue = it->second;

    auto *cb = new DmaVirtCallback<uint64_t>(
        [ = ] (const uint64_t &) { processQueue(queue_id); },
        queue.readIndex);
    dmaWriteVirt(queue.readPtr, sizeof(uint64_t), cb, &cb->dmaBuffer);
}

void
SESDMAEngine::executeFence(uint64_t queue_id, const sdmaFence &pkt)
{
    auto &queue = queues.at(queue_id);
    advanceReadIndex(queue, sizeof(sdmaFence));

    auto *cb = new DmaVirtCallback<uint32_t>(
        [ = ] (const uint32_t &) { finishPacket(queue_id); }, pkt.data);
    dmaWriteAddr(pkt.dest, sizeof(pkt.data), cb, &cb->dmaBuffer);
}

void
SESDMAEngine::executeTrap(uint64_t queue_id, const sdmaTrap &pkt)
{
    auto &queue = queues.at(queue_id);
    advanceReadIndex(queue, sizeof(sdmaTrap));
    DPRINTF(SESDMAEngine, "SDMA trap queue %lu context %#x\n",
            queue_id, pkt.intrContext);
    finishPacket(queue_id);
}

void
SESDMAEngine::executePollRegMem(uint64_t queue_id, uint32_t header,
                                const sdmaPollRegMem &pkt)
{
    auto &queue = queues.at(queue_id);
    advanceReadIndex(queue, sizeof(sdmaPollRegMem));

    sdmaPollRegMemHeader poll_header;
    poll_header.ordinal = header;

    DPRINTF(SESDMAEngine,
            "SDMA POLL_REGMEM queue %lu mode %u func %u op %u addr %#lx "
            "ref %#x mask %#x retry %u poll_int %u\n",
            queue_id, poll_header.mode, poll_header.func, poll_header.op,
            pkt.address, pkt.ref, pkt.mask, pkt.retryCount, pkt.pollInt);

    if (poll_header.mode == 0) {
        DPRINTF(SESDMAEngine,
                "SDMA POLL_REGMEM queue %lu register/HDP-flush poll has no "
                "SE register model; consuming packet\n",
                queue_id);
        finishPacket(queue_id);
        return;
    }

    if (poll_header.op != 0) {
        fatal("SE SDMA unsupported POLL_REGMEM operation %#x header %#x\n",
              poll_header.op, header);
    }

    auto *data = new uint32_t;
    auto *cb = new DmaVirtCallback<uint32_t>(
        [ = ] (const uint32_t &)
        { executePollRegMemData(queue_id, header, pkt, data, 0); });
    dmaReadAddr(pkt.address, sizeof(*data), cb, data);
}

void
SESDMAEngine::executePollRegMemData(uint64_t queue_id, uint32_t header,
                                    sdmaPollRegMem pkt, uint32_t *data,
                                    uint32_t count)
{
    sdmaPollRegMemHeader poll_header;
    poll_header.ordinal = header;

    const uint32_t value = *data & pkt.mask;
    const uint32_t reference = pkt.ref & pkt.mask;
    const bool matched = pollRegMemFunc(value, reference, poll_header.func);
    const bool retry_forever = pkt.retryCount == 0xfff;
    const bool retry_limited = pkt.retryCount != 0xfff &&
        count < static_cast<uint32_t>(pkt.retryCount + 1);

    if (!matched && (retry_forever || retry_limited)) {
        DPRINTF(SESDMAEngine,
                "SDMA POLL_REGMEM queue %lu retry addr %#lx value %#x "
                "masked %#x ref %#x masked_ref %#x func %u count %u "
                "retry %u\n",
                queue_id, pkt.address, *data, value, pkt.ref, reference,
                poll_header.func, count, pkt.retryCount);

        auto *cb = new DmaVirtCallback<uint32_t>(
            [ = ] (const uint32_t &)
            { executePollRegMemData(queue_id, header, pkt, data, count + 1); });
        dmaReadAddr(pkt.address, sizeof(*data), cb, data);
        return;
    }

    DPRINTF(SESDMAEngine,
            "SDMA POLL_REGMEM queue %lu done addr %#lx value %#x masked %#x "
            "ref %#x masked_ref %#x func %u matched %d count %u retry %u\n",
            queue_id, pkt.address, *data, value, pkt.ref, reference,
            poll_header.func, matched, count, pkt.retryCount);

    delete data;
    finishPacket(queue_id);
}

bool
SESDMAEngine::pollRegMemFunc(uint32_t value, uint32_t reference,
                             uint32_t func) const
{
    switch (func) {
      case 0:
        return true;
      case 1:
        return value < reference;
      case 2:
        return value <= reference;
      case 3:
        return value == reference;
      case 4:
        return value != reference;
      case 5:
        return value >= reference;
      case 6:
        return value > reference;
      default:
        fatal("SE SDMA unsupported POLL_REGMEM comparison function %#x\n",
              func);
    }
}

void
SESDMAEngine::executeWrite(uint64_t queue_id, const sdmaWrite &pkt)
{
    auto &queue = queues.at(queue_id);
    advanceReadIndex(queue, sizeof(sdmaWrite));

    const uint64_t dwords = static_cast<uint64_t>(pkt.count) + 1;
    fatal_if(dwords > (128ULL << 20) / sizeof(uint32_t),
             "SE SDMA WRITE too large for MVP buffer: %lu dwords\n", dwords);

    DPRINTF(SESDMAEngine, "SDMA write queue %lu %lu dwords to %#lx\n",
            queue_id, dwords, pkt.dest);

    auto *data = new std::vector<uint32_t>(dwords);
    auto *cb = new DmaVirtCallback<uint64_t>(
        [ = ] (const uint64_t &)
        { executeWriteData(queue_id, pkt, data); });
    dmaReadVirt(ringAddr(queue), static_cast<unsigned>(
                    dwords * sizeof(uint32_t)), cb, data->data());
}

void
SESDMAEngine::executeWriteData(uint64_t queue_id, sdmaWrite pkt,
                               std::vector<uint32_t> *data)
{
    auto &queue = queues.at(queue_id);
    advanceReadIndex(queue, data->size() * sizeof(uint32_t));

    auto *cb = new DmaVirtCallback<uint64_t>(
        [ = ] (const uint64_t &) {
            delete data;
            finishPacket(queue_id);
        });
    dmaWriteAddr(pkt.dest, static_cast<unsigned>(
                     data->size() * sizeof(uint32_t)), cb, data->data());
}

void
SESDMAEngine::executeAtomic(uint64_t queue_id, uint32_t header,
                            const sdmaAtomic &pkt)
{
    auto &queue = queues.at(queue_id);
    advanceReadIndex(queue, sizeof(sdmaAtomic));

    sdmaAtomicHeader atomic_header;
    atomic_header.ordinal = header;

    DPRINTF(SESDMAEngine,
            "SDMA atomic queue %lu op %#x addr %#lx src %#lx cmp %#lx "
            "loop %d loop_int %u\n",
            queue_id, atomic_header.opcode, pkt.addr, pkt.srcData,
            pkt.cmpData, atomic_header.loop, pkt.loopInt);

    if (atomic_header.opcode != SDMA_ATOMIC_ADD64) {
        fatal("SE SDMA unsupported ATOMIC opcode %#x\n",
              atomic_header.opcode);
    }

    const auto addend = static_cast<int64_t>(pkt.srcData);

    DPRINTF(SESDMAEngine,
            "SDMA atomic ADD64 queue %lu addr %#lx src %#lx via Ruby atomic\n",
            queue_id, pkt.addr, pkt.srcData);

    auto *data = new uint64_t;
    auto *cb = new DmaVirtCallback<uint64_t>(
        [ = ] (const uint64_t &) {
            delete data;
            finishPacket(queue_id);
        });
    dmaAtomicAddr(pkt.addr, sizeof(*data), cb, data,
                  std::make_unique<AtomicOpAdd<uint64_t>>(
                      static_cast<uint64_t>(addend)));
}

void
SESDMAEngine::dumpRingDwords(uint64_t queue_id, const std::string &reason,
                             uint64_t byte_offset, uint32_t dwords)
{
    auto it = queues.find(queue_id);
    fatal_if(it == queues.end(), "Dumping unknown SE SDMA queue %lu\n",
             queue_id);
    auto &queue = it->second;

    if (queue.ringSize == 0 || dwords == 0) {
        return;
    }

    byte_offset %= queue.ringSize;
    byte_offset &= ~0x3ULL;
    const uint64_t bytes_left = queue.ringSize - byte_offset;
    const uint32_t dump_dwords = std::min<uint32_t>(
        dwords, static_cast<uint32_t>(bytes_left / sizeof(uint32_t)));
    if (dump_dwords == 0) {
        return;
    }

    DPRINTF(SESDMAEngine,
            "SDMA ring dump request queue %lu reason '%s' ring %#lx "
            "offset %lu dwords %u read_index %lu write_index %lu "
            "read_ptr %#lx write_ptr %#lx\n",
            queue_id, reason.c_str(), queue.ringBase, byte_offset, dump_dwords,
            queue.readIndex, queue.writeIndex, queue.readPtr,
            queue.writePtr);

    auto *data = new std::vector<uint32_t>(dump_dwords);
    auto *cb = new DmaVirtCallback<uint64_t>(
        [ = ] (const uint64_t &)
        { dumpRingDwordsData(queue_id, reason, byte_offset, data); });
    dmaReadVirt(queue.ringBase + byte_offset,
                dump_dwords * sizeof(uint32_t), cb, data->data());
}

void
SESDMAEngine::dumpRingDwordsData(uint64_t queue_id, std::string reason,
                                 uint64_t byte_offset,
                                 std::vector<uint32_t> *data)
{
    auto it = queues.find(queue_id);
    fatal_if(it == queues.end(), "Dumping unknown SE SDMA queue %lu\n",
             queue_id);
    auto &queue = it->second;

    for (uint32_t i = 0; i < data->size(); ++i) {
        const uint64_t offset = byte_offset + i * sizeof(uint32_t);
        DPRINTF(SESDMAEngine,
                "SDMA ring dump queue %lu reason '%s' offset %lu "
                "addr %#lx dword[%u] %#010x read_index %lu write_index %lu\n",
                queue_id, reason.c_str(), offset, queue.ringBase + offset, i,
                (*data)[i], queue.readIndex, queue.writeIndex);
    }

    delete data;
}

void
SESDMAEngine::executeConstFill(uint64_t queue_id, uint32_t header,
                               const sdmaConstFill &pkt)
{
    auto &queue = queues.at(queue_id);
    advanceReadIndex(queue, sizeof(sdmaConstFill));

    sdmaConstFillHeader fill_header;
    fill_header.ordinal = header;

    const uint64_t bytes =
        static_cast<uint64_t>(pkt.count + 1) << fill_header.fillsize;
    fatal_if(bytes > (128ULL << 20),
             "SE SDMA CONST_FILL too large for MVP buffer: %lu bytes\n",
             bytes);

    auto *fill_data = new std::vector<uint8_t>(bytes);
    std::memset(fill_data->data(), pkt.srcData, bytes);

    auto *cb = new DmaVirtCallback<uint64_t>(
        [ = ] (const uint64_t &) {
            delete fill_data;
            finishPacket(queue_id);
        });
    dmaWriteAddr(pkt.addr, static_cast<unsigned>(bytes), cb,
                 fill_data->data());
}

void
SESDMAEngine::executeCopy(uint64_t queue_id, const sdmaCopy &pkt)
{
    auto &queue = queues.at(queue_id);
    advanceReadIndex(queue, sizeof(sdmaCopy));

    const uint64_t bytes = static_cast<uint64_t>(pkt.count) + 1;
    fatal_if(bytes > (128ULL << 20),
             "SE SDMA COPY too large for MVP buffer: %lu bytes\n", bytes);

    auto *copy_data = new std::vector<uint8_t>(bytes);
    auto *read_cb = new DmaVirtCallback<uint64_t>(
        [ = ] (const uint64_t &) {
            auto *write_cb = new DmaVirtCallback<uint64_t>(
                [ = ] (const uint64_t &) {
                    delete copy_data;
                    finishPacket(queue_id);
                });
            dmaWriteAddr(pkt.dest, static_cast<unsigned>(bytes), write_cb,
                         copy_data->data());
        });
    dmaReadAddr(pkt.source, static_cast<unsigned>(bytes), read_cb,
                copy_data->data());
}

} // namespace gem5
