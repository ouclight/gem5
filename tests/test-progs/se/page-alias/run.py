import sys
from pathlib import Path

import m5

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource


PAGE_SIZE = 4096
ALIAS0_ADDR = 0x600000000000
ALIAS1_ADDR = 0x600000001000
SHARED_PADDR = 0x1FFF0000

processor = SimpleProcessor(
    cpu_type=CPUTypes.ATOMIC,
    isa=ISA.X86,
    num_cores=1,
)
board = SimpleBoard(
    clk_freq="1GHz",
    processor=processor,
    memory=SingleChannelDDR3_1600(size="512MiB"),
    cache_hierarchy=NoCache(),
)

binary = Path(__file__).resolve().parent / "page_alias"
board.set_se_binary_workload(
    binary=BinaryResource(local_path=str(binary)),
    exit_on_work_items=True,
)

process = processor.get_cores()[0].core.workload[0]

root = board._pre_instantiate(full_system=False)
m5.instantiate()

aliases_installed = False
exit_event = m5.simulate()
while True:
    cause = exit_event.getCause()
    if "workbegin" in cause:
        if aliases_installed:
            raise RuntimeError("received more than one workbegin event")
        print(
            f"Mapping SE aliases {ALIAS0_ADDR:#x} and {ALIAS1_ADDR:#x} "
            f"to physical page {SHARED_PADDR:#x}"
        )
        process.map(ALIAS0_ADDR, SHARED_PADDR, PAGE_SIZE, True)
        process.map(ALIAS1_ADDR, SHARED_PADDR, PAGE_SIZE, True)
        aliases_installed = True
        exit_event = m5.simulate()
        continue
    break

if not aliases_installed:
    raise RuntimeError(f"workbegin event was not observed: {cause}")

print(f"Exiting @ tick {m5.curTick()} because {cause}.")
sys.exit(exit_event.getCode())
