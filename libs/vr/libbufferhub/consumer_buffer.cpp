#include <private/dvr/consumer_buffer.h>

using android::pdx::LocalChannelHandle;
using android::pdx::LocalHandle;
using android::pdx::Status;

namespace android {
namespace dvr {

ConsumerBuffer::ConsumerBuffer(LocalChannelHandle channel)
    : BASE(std::move(channel)) {
  const int ret = ImportBuffer();
  if (ret < 0) {
    ALOGE("ConsumerBuffer::ConsumerBuffer: Failed to import buffer: %s",
          strerror(-ret));
    Close(ret);
  }
}

std::unique_ptr<ConsumerBuffer> ConsumerBuffer::Import(
    LocalChannelHandle channel) {
  ATRACE_NAME("ConsumerBuffer::Import");
  ALOGD_IF(TRACE, "ConsumerBuffer::Import: channel=%d", channel.value());
  return ConsumerBuffer::Create(std::move(channel));
}

std::unique_ptr<ConsumerBuffer> ConsumerBuffer::Import(
    Status<LocalChannelHandle> status) {
  return Import(status ? status.take()
                       : LocalChannelHandle{nullptr, -status.error()});
}

int ConsumerBuffer::LocalAcquire(DvrNativeBufferMetadata* out_meta,
                                 LocalHandle* out_fence) {
  if (!out_meta)
    return -EINVAL;

  // Only check producer bit and this consumer buffer's particular consumer bit.
  // The buffer is can be acquired iff: 1) producer bit is set; 2) consumer bit
  // is not set.
  uint64_t buffer_state = buffer_state_->load();
  if (!BufferHubDefs::IsBufferPosted(buffer_state, buffer_state_bit())) {
    ALOGE("ConsumerBuffer::LocalAcquire: not posted, id=%d state=%" PRIx64
          " buffer_state_bit=%" PRIx64 ".",
          id(), buffer_state, buffer_state_bit());
    return -EBUSY;
  }

  // Copy the canonical metadata.
  void* metadata_ptr = reinterpret_cast<void*>(&metadata_header_->metadata);
  memcpy(out_meta, metadata_ptr, sizeof(DvrNativeBufferMetadata));
  // Fill in the user_metadata_ptr in address space of the local process.
  if (out_meta->user_metadata_size) {
    out_meta->user_metadata_ptr =
        reinterpret_cast<uint64_t>(user_metadata_ptr_);
  } else {
    out_meta->user_metadata_ptr = 0;
  }

  uint64_t fence_state = fence_state_->load();
  // If there is an acquire fence from producer, we need to return it.
  if (fence_state & BufferHubDefs::kProducerStateBit) {
    *out_fence = shared_acquire_fence_.Duplicate();
  }

  // Set the consumer bit unique to this consumer.
  BufferHubDefs::ModifyBufferState(buffer_state_, 0ULL, buffer_state_bit());
  return 0;
}

int ConsumerBuffer::Acquire(LocalHandle* ready_fence) {
  return Acquire(ready_fence, nullptr, 0);
}

int ConsumerBuffer::Acquire(LocalHandle* ready_fence, void* meta,
                            size_t user_metadata_size) {
  ATRACE_NAME("ConsumerBuffer::Acquire");

  if (const int error = CheckMetadata(user_metadata_size))
    return error;

  DvrNativeBufferMetadata canonical_meta;
  if (const int error = LocalAcquire(&canonical_meta, ready_fence))
    return error;

  if (meta && user_metadata_size) {
    void* metadata_src =
        reinterpret_cast<void*>(canonical_meta.user_metadata_ptr);
    if (metadata_src) {
      memcpy(meta, metadata_src, user_metadata_size);
    } else {
      ALOGW("ConsumerBuffer::Acquire: no user-defined metadata.");
    }
  }

  auto status = InvokeRemoteMethod<BufferHubRPC::ConsumerAcquire>();
  if (!status)
    return -status.error();
  return 0;
}

int ConsumerBuffer::AcquireAsync(DvrNativeBufferMetadata* out_meta,
                                 LocalHandle* out_fence) {
  ATRACE_NAME("ConsumerBuffer::AcquireAsync");

  if (const int error = LocalAcquire(out_meta, out_fence))
    return error;

  auto status = SendImpulse(BufferHubRPC::ConsumerAcquire::Opcode);
  if (!status)
    return -status.error();
  return 0;
}

int ConsumerBuffer::LocalRelease(const DvrNativeBufferMetadata* meta,
                                 const LocalHandle& release_fence) {
  if (const int error = CheckMetadata(meta->user_metadata_size))
    return error;

  // Check invalid state transition.
  uint64_t buffer_state = buffer_state_->load();
  if (!BufferHubDefs::IsBufferAcquired(buffer_state)) {
    ALOGE("ConsumerBuffer::LocalRelease: not acquired id=%d state=%" PRIx64 ".",
          id(), buffer_state);
    return -EBUSY;
  }

  // On release, only the user requested metadata is copied back into the shared
  // memory for metadata. Since there are multiple consumers, it doesn't make
  // sense to send the canonical metadata back to the producer. However, one of
  // the consumer can still choose to write up to user_metadata_size bytes of
  // data into user_metadata_ptr.
  if (meta->user_metadata_ptr && meta->user_metadata_size) {
    void* metadata_src = reinterpret_cast<void*>(meta->user_metadata_ptr);
    memcpy(user_metadata_ptr_, metadata_src, meta->user_metadata_size);
  }

  // Send out the release fence through the shared epoll fd. Note that during
  // releasing the producer is not expected to be polling on the fence.
  if (const int error = UpdateSharedFence(release_fence, shared_release_fence_))
    return error;

  // For release operation, the client don't need to change the state as it's
  // bufferhubd's job to flip the produer bit once all consumers are released.
  return 0;
}

int ConsumerBuffer::Release(const LocalHandle& release_fence) {
  ATRACE_NAME("ConsumerBuffer::Release");

  DvrNativeBufferMetadata meta;
  if (const int error = LocalRelease(&meta, release_fence))
    return error;

  return ReturnStatusOrError(InvokeRemoteMethod<BufferHubRPC::ConsumerRelease>(
      BorrowedFence(release_fence.Borrow())));
}

int ConsumerBuffer::ReleaseAsync() {
  DvrNativeBufferMetadata meta;
  return ReleaseAsync(&meta, LocalHandle());
}

int ConsumerBuffer::ReleaseAsync(const DvrNativeBufferMetadata* meta,
                                 const LocalHandle& release_fence) {
  ATRACE_NAME("ConsumerBuffer::ReleaseAsync");

  if (const int error = LocalRelease(meta, release_fence))
    return error;

  return ReturnStatusOrError(
      SendImpulse(BufferHubRPC::ConsumerRelease::Opcode));
}

int ConsumerBuffer::Discard() { return Release(LocalHandle()); }

}  // namespace dvr
}  // namespace android