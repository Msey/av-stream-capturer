#pragma once

#include <cstdint>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace cfx {

inline constexpr wchar_t kSharedFrameName[] = L"Local\\ConferenceFx.Frames.v1";
inline constexpr uint32_t kSharedMagic = 0x46584346u;  // 'CFXF'
inline constexpr int kSharedMaxWidth = 1920;
inline constexpr int kSharedMaxHeight = 1080;
inline constexpr int kSharedSlots = 3;
inline constexpr int kSharedSlotBytes = kSharedMaxWidth * kSharedMaxHeight * 4;

// Cross-process frame mailbox. The writer fills a back slot and then publishes
// it, so a reader copying the published slot does not tear the pixels.
struct SharedHeader {
  uint32_t magic;
  uint32_t version;
  volatile LONG frameId;
  LONG width;
  LONG height;
  LONG stride;
  LONG slot;
  LONG reserved;
};

inline size_t sharedMappingBytes() {
  return sizeof(SharedHeader) + static_cast<size_t>(kSharedSlots) * kSharedSlotBytes;
}

struct SharedFrames {
  HANDLE mapping = nullptr;
  SharedHeader* header = nullptr;
  uint8_t* slots = nullptr;

  bool open() {
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                 static_cast<DWORD>(sharedMappingBytes()), kSharedFrameName);
    if (!mapping) return false;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sharedMappingBytes());
    if (!view) {
      CloseHandle(mapping);
      mapping = nullptr;
      return false;
    }
    header = static_cast<SharedHeader*>(view);
    slots = reinterpret_cast<uint8_t*>(header + 1);
    if (header->magic != kSharedMagic) {
      std::memset(header, 0, sizeof(SharedHeader));
      header->magic = kSharedMagic;
      header->version = 1;
      header->slot = 0;
      header->frameId = 0;
    }
    return true;
  }

  void close() {
    if (header) UnmapViewOfFile(header);
    if (mapping) CloseHandle(mapping);
    header = nullptr;
    slots = nullptr;
    mapping = nullptr;
  }

  uint8_t* slotPtr(int index) const { return slots + static_cast<size_t>(index) * kSharedSlotBytes; }

  void publish(const uint8_t* bgra, int width, int height, int stride) {
    if (!header || !bgra || width <= 0 || height <= 0 || width > kSharedMaxWidth || height > kSharedMaxHeight) return;
    const int next = (header->slot + 1) % kSharedSlots;
    uint8_t* dst = slotPtr(next);
    const int dstStride = width * 4;
    for (int y = 0; y < height; ++y) {
      std::memcpy(dst + static_cast<size_t>(y) * dstStride, bgra + static_cast<size_t>(y) * stride, dstStride);
    }
    header->width = width;
    header->height = height;
    header->stride = dstStride;
    MemoryBarrier();
    header->slot = next;
    InterlockedIncrement(&header->frameId);
  }

  // Copies the latest BGRA frame. Returns false when the producer has not published yet.
  bool copyLatest(uint8_t* dst, int dstStride, int maxWidth, int maxHeight, int& width, int& height) const {
    if (!header || header->magic != kSharedMagic || header->frameId == 0) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
      const LONG id = header->frameId;
      MemoryBarrier();
      width = header->width;
      height = header->height;
      const int srcStride = header->stride;
      const int slot = header->slot;
      if (width <= 0 || height <= 0 || width > maxWidth || height > maxHeight || srcStride < width * 4) return false;
      const uint8_t* src = slotPtr(slot);
      for (int y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * dstStride, src + static_cast<size_t>(y) * srcStride,
                    static_cast<size_t>(width) * 4);
      }
      MemoryBarrier();
      if (header->frameId == id) return true;
    }
    return false;
  }
};

}  // namespace cfx
