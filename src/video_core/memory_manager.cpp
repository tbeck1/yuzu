// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra {

MemoryManager::MemoryManager(Core::System& system, VideoCore::RasterizerInterface& rasterizer)
    : rasterizer{rasterizer}, system{system} {
    std::fill(page_table.pointers.begin(), page_table.pointers.end(), nullptr);
    std::fill(page_table.attributes.begin(), page_table.attributes.end(),
              Common::PageType::Unmapped);
    page_table.Resize(address_space_width);

    // Initialize the map with a single free region covering the entire managed space.
    VirtualMemoryArea initial_vma;
    initial_vma.size = address_space_end;
    vma_map.emplace(initial_vma.base, initial_vma);

    UpdatePageTableForVMA(initial_vma);
}

MemoryManager::~MemoryManager() = default;

GPUVAddr MemoryManager::AllocateSpace(u64 size, u64 align) {
    const u64 aligned_size{Common::AlignUp(size, page_size)};
    const GPUVAddr gpu_addr{FindFreeRegion(address_space_base, aligned_size)};

    AllocateMemory(gpu_addr, 0, aligned_size);

    return gpu_addr;
}

GPUVAddr MemoryManager::AllocateSpace(GPUVAddr gpu_addr, u64 size, u64 align) {
    const u64 aligned_size{Common::AlignUp(size, page_size)};

    AllocateMemory(gpu_addr, 0, aligned_size);

    return gpu_addr;
}

GPUVAddr MemoryManager::MapBufferEx(VAddr cpu_addr, u64 size) {
    const u64 aligned_size{Common::AlignUp(size, page_size)};
    const GPUVAddr gpu_addr{FindFreeRegion(address_space_base, aligned_size)};

    MapBackingMemory(gpu_addr, system.Memory().GetPointer(cpu_addr), aligned_size, cpu_addr);
    ASSERT(system.CurrentProcess()
               ->VMManager()
               .SetMemoryAttribute(cpu_addr, size, Kernel::MemoryAttribute::DeviceMapped,
                                   Kernel::MemoryAttribute::DeviceMapped)
               .IsSuccess());

    return gpu_addr;
}

GPUVAddr MemoryManager::MapBufferEx(VAddr cpu_addr, GPUVAddr gpu_addr, u64 size) {
    ASSERT((gpu_addr & page_mask) == 0);

    const u64 aligned_size{Common::AlignUp(size, page_size)};

    MapBackingMemory(gpu_addr, system.Memory().GetPointer(cpu_addr), aligned_size, cpu_addr);
    ASSERT(system.CurrentProcess()
               ->VMManager()
               .SetMemoryAttribute(cpu_addr, size, Kernel::MemoryAttribute::DeviceMapped,
                                   Kernel::MemoryAttribute::DeviceMapped)
               .IsSuccess());
    return gpu_addr;
}

GPUVAddr MemoryManager::UnmapBuffer(GPUVAddr gpu_addr, u64 size) {
    ASSERT((gpu_addr & page_mask) == 0);

    const u64 aligned_size{Common::AlignUp(size, page_size)};
    const CacheAddr cache_addr{ToCacheAddr(GetPointer(gpu_addr))};
    const auto cpu_addr = GpuToCpuAddress(gpu_addr);
    ASSERT(cpu_addr);

    // Flush and invalidate through the GPU interface, to be asynchronous if possible.
    system.GPU().FlushAndInvalidateRegion(cache_addr, aligned_size);

    UnmapRange(gpu_addr, aligned_size);
    ASSERT(system.CurrentProcess()
               ->VMManager()
               .SetMemoryAttribute(cpu_addr.value(), size, Kernel::MemoryAttribute::DeviceMapped,
                                   Kernel::MemoryAttribute::None)
               .IsSuccess());

    return gpu_addr;
}

GPUVAddr MemoryManager::FindFreeRegion(GPUVAddr region_start, u64 size) const {
    // Find the first Free VMA.
    const VMAHandle vma_handle{
        std::find_if(vma_map.begin(), vma_map.end(), [region_start, size](const auto& vma) {
            if (vma.second.type != VirtualMemoryArea::Type::Unmapped) {
                return false;
            }

            const VAddr vma_end{vma.second.base + vma.second.size};
            return vma_end > region_start && vma_end >= region_start + size;
        })};

    if (vma_handle == vma_map.end()) {
        return {};
    }

    return std::max(region_start, vma_handle->second.base);
}

bool MemoryManager::IsAddressValid(GPUVAddr addr) const {
    return (addr >> page_bits) < page_table.pointers.size();
}

std::optional<VAddr> MemoryManager::GpuToCpuAddress(GPUVAddr addr) const {
    if (!IsAddressValid(addr)) {
        return {};
    }

    const VAddr cpu_addr{page_table.backing_addr[addr >> page_bits]};
    if (cpu_addr) {
        return cpu_addr + (addr & page_mask);
    }

    return {};
}

template <typename T>
T MemoryManager::Read(GPUVAddr addr) const {
    if (!IsAddressValid(addr)) {
        return {};
    }

    const u8* page_pointer{page_table.pointers[addr >> page_bits]};
    if (page_pointer) {
        // NOTE: Avoid adding any extra logic to this fast-path block
        T value;
        std::memcpy(&value, &page_pointer[addr & page_mask], sizeof(T));
        return value;
    }

    switch (page_table.attributes[addr >> page_bits]) {
    case Common::PageType::Unmapped:
        LOG_ERROR(HW_GPU, "Unmapped Read{} @ 0x{:08X}", sizeof(T) * 8, addr);
        return 0;
    case Common::PageType::Memory:
        ASSERT_MSG(false, "Mapped memory page without a pointer @ {:016X}", addr);
        break;
    default:
        UNREACHABLE();
    }
    return {};
}

template <typename T>
void MemoryManager::Write(GPUVAddr addr, T data) {
    if (!IsAddressValid(addr)) {
        return;
    }

    u8* page_pointer{page_table.pointers[addr >> page_bits]};
    if (page_pointer) {
        // NOTE: Avoid adding any extra logic to this fast-path block
        std::memcpy(&page_pointer[addr & page_mask], &data, sizeof(T));
        return;
    }

    switch (page_table.attributes[addr >> page_bits]) {
    case Common::PageType::Unmapped:
        LOG_ERROR(HW_GPU, "Unmapped Write{} 0x{:08X} @ 0x{:016X}", sizeof(data) * 8,
                  static_cast<u32>(data), addr);
        return;
    case Common::PageType::Memory:
        ASSERT_MSG(false, "Mapped memory page without a pointer @ {:016X}", addr);
        break;
    default:
        UNREACHABLE();
    }
}

template u8 MemoryManager::Read<u8>(GPUVAddr addr) const;
template u16 MemoryManager::Read<u16>(GPUVAddr addr) const;
template u32 MemoryManager::Read<u32>(GPUVAddr addr) const;
template u64 MemoryManager::Read<u64>(GPUVAddr addr) const;
template void MemoryManager::Write<u8>(GPUVAddr addr, u8 data);
template void MemoryManager::Write<u16>(GPUVAddr addr, u16 data);
template void MemoryManager::Write<u32>(GPUVAddr addr, u32 data);
template void MemoryManager::Write<u64>(GPUVAddr addr, u64 data);

u8* MemoryManager::GetPointer(GPUVAddr addr) {
    if (!IsAddressValid(addr)) {
        return {};
    }

    u8* const page_pointer{page_table.pointers[addr >> page_bits]};
    if (page_pointer != nullptr) {
        return page_pointer + (addr & page_mask);
    }

    LOG_ERROR(HW_GPU, "Unknown GetPointer @ 0x{:016X}", addr);
    return {};
}

const u8* MemoryManager::GetPointer(GPUVAddr addr) const {
    if (!IsAddressValid(addr)) {
        return {};
    }

    const u8* const page_pointer{page_table.pointers[addr >> page_bits]};
    if (page_pointer != nullptr) {
        return page_pointer + (addr & page_mask);
    }

    LOG_ERROR(HW_GPU, "Unknown GetPointer @ 0x{:016X}", addr);
    return {};
}

bool MemoryManager::IsBlockContinuous(const GPUVAddr start, const std::size_t size) const {
    const std::size_t inner_size = size - 1;
    const GPUVAddr end = start + inner_size;
    const auto host_ptr_start = reinterpret_cast<std::uintptr_t>(GetPointer(start));
    const auto host_ptr_end = reinterpret_cast<std::uintptr_t>(GetPointer(end));
    const auto range = static_cast<std::size_t>(host_ptr_end - host_ptr_start);
    return range == inner_size;
}

void MemoryManager::ReadBlock(GPUVAddr src_addr, void* dest_buffer, const std::size_t size) const {
    std::size_t remaining_size{size};
    std::size_t page_index{src_addr >> page_bits};
    std::size_t page_offset{src_addr & page_mask};

    while (remaining_size > 0) {
        const std::size_t copy_amount{
            std::min(static_cast<std::size_t>(page_size) - page_offset, remaining_size)};

        switch (page_table.attributes[page_index]) {
        case Common::PageType::Memory: {
            const u8* src_ptr{page_table.pointers[page_index] + page_offset};
            // Flush must happen on the rasterizer interface, such that memory is always synchronous
            // when it is read (even when in asynchronous GPU mode). Fixes Dead Cells title menu.
            rasterizer.FlushRegion(ToCacheAddr(src_ptr), copy_amount);
            std::memcpy(dest_buffer, src_ptr, copy_amount);
            break;
        }
        default:
            UNREACHABLE();
        }

        page_index++;
        page_offset = 0;
        dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
        remaining_size -= copy_amount;
    }
}

void MemoryManager::ReadBlockUnsafe(GPUVAddr src_addr, void* dest_buffer,
                                    const std::size_t size) const {
    std::size_t remaining_size{size};
    std::size_t page_index{src_addr >> page_bits};
    std::size_t page_offset{src_addr & page_mask};

    while (remaining_size > 0) {
        const std::size_t copy_amount{
            std::min(static_cast<std::size_t>(page_size) - page_offset, remaining_size)};
        const u8* page_pointer = page_table.pointers[page_index];
        if (page_pointer) {
            const u8* src_ptr{page_pointer + page_offset};
            std::memcpy(dest_buffer, src_ptr, copy_amount);
        } else {
            std::memset(dest_buffer, 0, copy_amount);
        }
        page_index++;
        page_offset = 0;
        dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
        remaining_size -= copy_amount;
    }
}

void MemoryManager::WriteBlock(GPUVAddr dest_addr, const void* src_buffer, const std::size_t size) {
    std::size_t remaining_size{size};
    std::size_t page_index{dest_addr >> page_bits};
    std::size_t page_offset{dest_addr & page_mask};

    while (remaining_size > 0) {
        const std::size_t copy_amount{
            std::min(static_cast<std::size_t>(page_size) - page_offset, remaining_size)};

        switch (page_table.attributes[page_index]) {
        case Common::PageType::Memory: {
            u8* dest_ptr{page_table.pointers[page_index] + page_offset};
            // Invalidate must happen on the rasterizer interface, such that memory is always
            // synchronous when it is written (even when in asynchronous GPU mode).
            rasterizer.InvalidateRegion(ToCacheAddr(dest_ptr), copy_amount);
            std::memcpy(dest_ptr, src_buffer, copy_amount);
            break;
        }
        default:
            UNREACHABLE();
        }

        page_index++;
        page_offset = 0;
        src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
        remaining_size -= copy_amount;
    }
}

void MemoryManager::WriteBlockUnsafe(GPUVAddr dest_addr, const void* src_buffer,
                                     const std::size_t size) {
    std::size_t remaining_size{size};
    std::size_t page_index{dest_addr >> page_bits};
    std::size_t page_offset{dest_addr & page_mask};

    while (remaining_size > 0) {
        const std::size_t copy_amount{
            std::min(static_cast<std::size_t>(page_size) - page_offset, remaining_size)};
        u8* page_pointer = page_table.pointers[page_index];
        if (page_pointer) {
            u8* dest_ptr{page_pointer + page_offset};
            std::memcpy(dest_ptr, src_buffer, copy_amount);
        }
        page_index++;
        page_offset = 0;
        src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
        remaining_size -= copy_amount;
    }
}

void MemoryManager::CopyBlock(GPUVAddr dest_addr, GPUVAddr src_addr, const std::size_t size) {
    std::size_t remaining_size{size};
    std::size_t page_index{src_addr >> page_bits};
    std::size_t page_offset{src_addr & page_mask};

    while (remaining_size > 0) {
        const std::size_t copy_amount{
            std::min(static_cast<std::size_t>(page_size) - page_offset, remaining_size)};

        switch (page_table.attributes[page_index]) {
        case Common::PageType::Memory: {
            // Flush must happen on the rasterizer interface, such that memory is always synchronous
            // when it is copied (even when in asynchronous GPU mode).
            const u8* src_ptr{page_table.pointers[page_index] + page_offset};
            rasterizer.FlushRegion(ToCacheAddr(src_ptr), copy_amount);
            WriteBlock(dest_addr, src_ptr, copy_amount);
            break;
        }
        default:
            UNREACHABLE();
        }

        page_index++;
        page_offset = 0;
        dest_addr += static_cast<VAddr>(copy_amount);
        src_addr += static_cast<VAddr>(copy_amount);
        remaining_size -= copy_amount;
    }
}

void MemoryManager::CopyBlockUnsafe(GPUVAddr dest_addr, GPUVAddr src_addr, const std::size_t size) {
    std::vector<u8> tmp_buffer(size);
    ReadBlockUnsafe(src_addr, tmp_buffer.data(), size);
    WriteBlockUnsafe(dest_addr, tmp_buffer.data(), size);
}

void MemoryManager::MapPages(GPUVAddr base, u64 size, u8* memory, Common::PageType type,
                             VAddr backing_addr) {
    LOG_DEBUG(HW_GPU, "Mapping {} onto {:016X}-{:016X}", fmt::ptr(memory), base * page_size,
              (base + size) * page_size);

    const VAddr end{base + size};
    ASSERT_MSG(end <= page_table.pointers.size(), "out of range mapping at {:016X}",
               base + page_table.pointers.size());

    std::fill(page_table.attributes.begin() + base, page_table.attributes.begin() + end, type);

    if (memory == nullptr) {
        std::fill(page_table.pointers.begin() + base, page_table.pointers.begin() + end, memory);
        std::fill(page_table.backing_addr.begin() + base, page_table.backing_addr.begin() + end,
                  backing_addr);
    } else {
        while (base != end) {
            page_table.pointers[base] = memory;
            page_table.backing_addr[base] = backing_addr;

            base += 1;
            memory += page_size;
            backing_addr += page_size;
        }
    }
}

void MemoryManager::MapMemoryRegion(GPUVAddr base, u64 size, u8* target, VAddr backing_addr) {
    ASSERT_MSG((size & page_mask) == 0, "non-page aligned size: {:016X}", size);
    ASSERT_MSG((base & page_mask) == 0, "non-page aligned base: {:016X}", base);
    MapPages(base / page_size, size / page_size, target, Common::PageType::Memory, backing_addr);
}

void MemoryManager::UnmapRegion(GPUVAddr base, u64 size) {
    ASSERT_MSG((size & page_mask) == 0, "non-page aligned size: {:016X}", size);
    ASSERT_MSG((base & page_mask) == 0, "non-page aligned base: {:016X}", base);
    MapPages(base / page_size, size / page_size, nullptr, Common::PageType::Unmapped);
}

bool VirtualMemoryArea::CanBeMergedWith(const VirtualMemoryArea& next) const {
    ASSERT(base + size == next.base);
    if (type != next.type) {
        return {};
    }
    if (type == VirtualMemoryArea::Type::Allocated && (offset + size != next.offset)) {
        return {};
    }
    if (type == VirtualMemoryArea::Type::Mapped && backing_memory + size != next.backing_memory) {
        return {};
    }
    return true;
}

MemoryManager::VMAHandle MemoryManager::FindVMA(GPUVAddr target) const {
    if (target >= address_space_end) {
        return vma_map.end();
    } else {
        return std::prev(vma_map.upper_bound(target));
    }
}

MemoryManager::VMAIter MemoryManager::Allocate(VMAIter vma_handle) {
    VirtualMemoryArea& vma{vma_handle->second};

    vma.type = VirtualMemoryArea::Type::Allocated;
    vma.backing_addr = 0;
    vma.backing_memory = {};
    UpdatePageTableForVMA(vma);

    return MergeAdjacent(vma_handle);
}

MemoryManager::VMAHandle MemoryManager::AllocateMemory(GPUVAddr target, std::size_t offset,
                                                       u64 size) {

    // This is the appropriately sized VMA that will turn into our allocation.
    VMAIter vma_handle{CarveVMA(target, size)};
    VirtualMemoryArea& vma{vma_handle->second};

    ASSERT(vma.size == size);

    vma.offset = offset;

    return Allocate(vma_handle);
}

MemoryManager::VMAHandle MemoryManager::MapBackingMemory(GPUVAddr target, u8* memory, u64 size,
                                                         VAddr backing_addr) {
    // This is the appropriately sized VMA that will turn into our allocation.
    VMAIter vma_handle{CarveVMA(target, size)};
    VirtualMemoryArea& vma{vma_handle->second};

    ASSERT(vma.size == size);

    vma.type = VirtualMemoryArea::Type::Mapped;
    vma.backing_memory = memory;
    vma.backing_addr = backing_addr;
    UpdatePageTableForVMA(vma);

    return MergeAdjacent(vma_handle);
}

void MemoryManager::UnmapRange(GPUVAddr target, u64 size) {
    VMAIter vma{CarveVMARange(target, size)};
    const VAddr target_end{target + size};
    const VMAIter end{vma_map.end()};

    // The comparison against the end of the range must be done using addresses since VMAs can be
    // merged during this process, causing invalidation of the iterators.
    while (vma != end && vma->second.base < target_end) {
        // Unmapped ranges return to allocated state and can be reused
        // This behavior is used by Super Mario Odyssey, Sonic Forces, and likely other games
        vma = std::next(Allocate(vma));
    }

    ASSERT(FindVMA(target)->second.size >= size);
}

MemoryManager::VMAIter MemoryManager::StripIterConstness(const VMAHandle& iter) {
    // This uses a neat C++ trick to convert a const_iterator to a regular iterator, given
    // non-const access to its container.
    return vma_map.erase(iter, iter); // Erases an empty range of elements
}

MemoryManager::VMAIter MemoryManager::CarveVMA(GPUVAddr base, u64 size) {
    ASSERT_MSG((size & page_mask) == 0, "non-page aligned size: 0x{:016X}", size);
    ASSERT_MSG((base & page_mask) == 0, "non-page aligned base: 0x{:016X}", base);

    VMAIter vma_handle{StripIterConstness(FindVMA(base))};
    if (vma_handle == vma_map.end()) {
        // Target address is outside the managed range
        return {};
    }

    const VirtualMemoryArea& vma{vma_handle->second};
    if (vma.type == VirtualMemoryArea::Type::Mapped) {
        // Region is already allocated
        return vma_handle;
    }

    const VAddr start_in_vma{base - vma.base};
    const VAddr end_in_vma{start_in_vma + size};

    ASSERT_MSG(end_in_vma <= vma.size, "region size 0x{:016X} is less than required size 0x{:016X}",
               vma.size, end_in_vma);

    if (end_in_vma < vma.size) {
        // Split VMA at the end of the allocated region
        SplitVMA(vma_handle, end_in_vma);
    }
    if (start_in_vma != 0) {
        // Split VMA at the start of the allocated region
        vma_handle = SplitVMA(vma_handle, start_in_vma);
    }

    return vma_handle;
}

MemoryManager::VMAIter MemoryManager::CarveVMARange(GPUVAddr target, u64 size) {
    ASSERT_MSG((size & page_mask) == 0, "non-page aligned size: 0x{:016X}", size);
    ASSERT_MSG((target & page_mask) == 0, "non-page aligned base: 0x{:016X}", target);

    const VAddr target_end{target + size};
    ASSERT(target_end >= target);
    ASSERT(size > 0);

    VMAIter begin_vma{StripIterConstness(FindVMA(target))};
    const VMAIter i_end{vma_map.lower_bound(target_end)};
    if (std::any_of(begin_vma, i_end, [](const auto& entry) {
            return entry.second.type == VirtualMemoryArea::Type::Unmapped;
        })) {
        return {};
    }

    if (target != begin_vma->second.base) {
        begin_vma = SplitVMA(begin_vma, target - begin_vma->second.base);
    }

    VMAIter end_vma{StripIterConstness(FindVMA(target_end))};
    if (end_vma != vma_map.end() && target_end != end_vma->second.base) {
        end_vma = SplitVMA(end_vma, target_end - end_vma->second.base);
    }

    return begin_vma;
}

MemoryManager::VMAIter MemoryManager::SplitVMA(VMAIter vma_handle, u64 offset_in_vma) {
    VirtualMemoryArea& old_vma{vma_handle->second};
    VirtualMemoryArea new_vma{old_vma}; // Make a copy of the VMA

    // For now, don't allow no-op VMA splits (trying to split at a boundary) because it's probably
    // a bug. This restriction might be removed later.
    ASSERT(offset_in_vma < old_vma.size);
    ASSERT(offset_in_vma > 0);

    old_vma.size = offset_in_vma;
    new_vma.base += offset_in_vma;
    new_vma.size -= offset_in_vma;

    switch (new_vma.type) {
    case VirtualMemoryArea::Type::Unmapped:
        break;
    case VirtualMemoryArea::Type::Allocated:
        new_vma.offset += offset_in_vma;
        break;
    case VirtualMemoryArea::Type::Mapped:
        new_vma.backing_memory += offset_in_vma;
        break;
    }

    ASSERT(old_vma.CanBeMergedWith(new_vma));

    return vma_map.emplace_hint(std::next(vma_handle), new_vma.base, new_vma);
}

MemoryManager::VMAIter MemoryManager::MergeAdjacent(VMAIter iter) {
    const VMAIter next_vma{std::next(iter)};
    if (next_vma != vma_map.end() && iter->second.CanBeMergedWith(next_vma->second)) {
        iter->second.size += next_vma->second.size;
        vma_map.erase(next_vma);
    }

    if (iter != vma_map.begin()) {
        VMAIter prev_vma{std::prev(iter)};
        if (prev_vma->second.CanBeMergedWith(iter->second)) {
            prev_vma->second.size += iter->second.size;
            vma_map.erase(iter);
            iter = prev_vma;
        }
    }

    return iter;
}

void MemoryManager::UpdatePageTableForVMA(const VirtualMemoryArea& vma) {
    switch (vma.type) {
    case VirtualMemoryArea::Type::Unmapped:
        UnmapRegion(vma.base, vma.size);
        break;
    case VirtualMemoryArea::Type::Allocated:
        MapMemoryRegion(vma.base, vma.size, nullptr, vma.backing_addr);
        break;
    case VirtualMemoryArea::Type::Mapped:
        MapMemoryRegion(vma.base, vma.size, vma.backing_memory, vma.backing_addr);
        break;
    }
}

} // namespace Tegra
