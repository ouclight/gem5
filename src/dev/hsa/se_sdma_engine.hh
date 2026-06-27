/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#ifndef __DEV_HSA_SE_SDMA_ENGINE_HH__
#define __DEV_HSA_SE_SDMA_ENGINE_HH__

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/compiler.hh"
#include "base/types.hh"
#include "dev/amdgpu/sdma_packets.hh"
#include "dev/dma_virt_device.hh"
#include "mem/translation_gen.hh"
#include "params/SESDMAEngine.hh"

namespace gem5
{

class SESDMAEngine : public DmaVirtDevice
{
  public:
    typedef SESDMAEngineParams Params;

    explicit SESDMAEngine(const Params &p);

    uint32_t gpuId() const { return _gpuId; }

    void registerQueue(uint64_t queue_id, Addr read_ptr, Addr write_ptr,
                       Addr ring_base, uint32_t ring_size, int doorbell_size);
    void unregisterQueue(uint64_t queue_id);
    void writeDoorbell(uint64_t queue_id, uint64_t write_index);
    TranslationGenPtr translate(Addr vaddr, Addr size) override;

    AddrRangeList getAddrRanges() const override;
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;

  private:
    struct QueueDesc
    {
        Addr readPtr = 0;
        Addr writePtr = 0;
        Addr ringBase = 0;
        uint32_t ringSize = 0;
        int doorbellSize = 0;
        uint64_t readIndex = 0;
        uint64_t writeIndex = 0;
        uint64_t lastDoorbellIndex = 0;
        bool processing = false;
        bool dumpedDoorbellRing = false;
        bool dumpedZeroHeaderRing = false;
    };

    const uint32_t _gpuId;
    std::unordered_map<uint64_t, QueueDesc> queues;

    Addr ringAddr(const QueueDesc &queue) const;
    void dmaReadAddr(Addr addr, unsigned size, DmaCallback *cb, void *data);
    void dmaWriteAddr(Addr addr, unsigned size, DmaCallback *cb, void *data);
    void updateWriteIndex(uint64_t queue_id, uint64_t write_index);
    void processQueue(uint64_t queue_id);
    void decodeHeader(uint64_t queue_id, uint32_t header);
    void finishPacket(uint64_t queue_id);
    void advanceReadIndex(QueueDesc &queue, uint64_t bytes);
    void dumpRingDwords(uint64_t queue_id, const std::string &reason,
                        uint64_t byte_offset, uint32_t dwords);
    void dumpRingDwordsData(uint64_t queue_id, std::string reason,
                            uint64_t byte_offset,
                            std::vector<uint32_t> *data);

    void executeFence(uint64_t queue_id, const sdmaFence &pkt);
    void executeTrap(uint64_t queue_id, const sdmaTrap &pkt);
    void executePollRegMem(uint64_t queue_id, uint32_t header,
                           const sdmaPollRegMem &pkt);
    void executePollRegMemData(uint64_t queue_id, uint32_t header,
                               sdmaPollRegMem pkt, uint32_t *data,
                               uint32_t count);
    bool pollRegMemFunc(uint32_t value, uint32_t reference,
                        uint32_t func) const;
    void executeWrite(uint64_t queue_id, const sdmaWrite &pkt);
    void executeWriteData(uint64_t queue_id, sdmaWrite pkt,
                          std::vector<uint32_t> *data);
    void executeAtomic(uint64_t queue_id, uint32_t header,
                       const sdmaAtomic &pkt);
    void executeAtomicData(uint64_t queue_id, uint32_t header,
                           sdmaAtomic pkt, uint64_t *data);
    void executeConstFill(uint64_t queue_id, uint32_t header,
                          const sdmaConstFill &pkt);
    void executeCopy(uint64_t queue_id, const sdmaCopy &pkt);
};

} // namespace gem5

#endif // __DEV_HSA_SE_SDMA_ENGINE_HH__
