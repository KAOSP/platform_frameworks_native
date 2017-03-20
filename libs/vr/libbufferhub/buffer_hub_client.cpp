#include <private/dvr/buffer_hub_client.h>

#include <log/log.h>
#include <poll.h>
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <utils/Trace.h>

#include <mutex>

#include <pdx/default_transport/client_channel.h>
#include <pdx/default_transport/client_channel_factory.h>
#include <private/dvr/platform_defines.h>

#include "include/private/dvr/bufferhub_rpc.h"

using android::pdx::LocalHandle;
using android::pdx::LocalChannelHandle;
using android::pdx::rpc::WrapBuffer;
using android::pdx::Status;

namespace {

constexpr int kUncachedBlobUsageFlags = GRALLOC_USAGE_SW_READ_RARELY |
                                        GRALLOC_USAGE_SW_WRITE_RARELY |
                                        GRALLOC_USAGE_PRIVATE_UNCACHED;

}  // anonymous namespace

namespace android {
namespace dvr {

BufferHubBuffer::BufferHubBuffer(LocalChannelHandle channel_handle)
    : Client{pdx::default_transport::ClientChannel::Create(
          std::move(channel_handle))},
      id_(-1) {}
BufferHubBuffer::BufferHubBuffer(const std::string& endpoint_path)
    : Client{pdx::default_transport::ClientChannelFactory::Create(
          endpoint_path)},
      id_(-1) {}

BufferHubBuffer::~BufferHubBuffer() {}

Status<LocalChannelHandle> BufferHubBuffer::CreateConsumer() {
  Status<LocalChannelHandle> status =
      InvokeRemoteMethod<BufferHubRPC::NewConsumer>();
  ALOGE_IF(!status,
           "BufferHub::CreateConsumer: Failed to create consumer channel: %s",
           status.GetErrorMessage().c_str());
  return status;
}

int BufferHubBuffer::ImportBuffer() {
  ATRACE_NAME("BufferHubBuffer::ImportBuffer");

  Status<std::vector<NativeBufferHandle<LocalHandle>>> status =
      InvokeRemoteMethod<BufferHubRPC::GetBuffers>();
  if (!status) {
    ALOGE("BufferHubBuffer::ImportBuffer: Failed to get buffers: %s",
          status.GetErrorMessage().c_str());
    return -status.error();
  } else if (status.get().empty()) {
    ALOGE(
        "BufferHubBuffer::ImportBuffer: Expected to receive at least one "
        "buffer handle but got zero!");
    return -EIO;
  }

  auto buffer_handles = status.take();

  // Stash the buffer id to replace the value in id_. All sub-buffers of a
  // buffer hub buffer have the same id.
  const int new_id = buffer_handles[0].id();

  // Import all of the buffers.
  std::vector<IonBuffer> ion_buffers;
  for (auto& handle : buffer_handles) {
    const size_t i = &handle - buffer_handles.data();
    ALOGD_IF(
        TRACE,
        "BufferHubBuffer::ImportBuffer: i=%zu id=%d FdCount=%zu IntCount=%zu",
        i, handle.id(), handle.FdCount(), handle.IntCount());

    IonBuffer buffer;
    const int ret = handle.Import(&buffer);
    if (ret < 0)
      return ret;

    ion_buffers.emplace_back(std::move(buffer));
  }

  // If all imports succeed, replace the previous buffers and id.
  slices_ = std::move(ion_buffers);
  id_ = new_id;
  return 0;
}

int BufferHubBuffer::Poll(int timeout_ms) {
  ATRACE_NAME("BufferHubBuffer::Poll");
  pollfd p = {event_fd(), POLLIN, 0};
  return poll(&p, 1, timeout_ms);
}

int BufferHubBuffer::Lock(int usage, int x, int y, int width, int height,
                          void** address, size_t index) {
  return slices_[index].Lock(usage, x, y, width, height, address);
}

int BufferHubBuffer::Unlock(size_t index) { return slices_[index].Unlock(); }

int BufferHubBuffer::GetBlobReadWritePointer(size_t size, void** addr) {
  int width = static_cast<int>(size);
  int height = 1;
  constexpr int usage = GRALLOC_USAGE_SW_READ_RARELY |
                        GRALLOC_USAGE_SW_WRITE_RARELY |
                        GRALLOC_USAGE_PRIVATE_UNCACHED;
  int ret = Lock(usage, 0, 0, width, height, addr);
  if (ret == 0)
    Unlock();
  return ret;
}

int BufferHubBuffer::GetBlobReadOnlyPointer(size_t size, void** addr) {
  int width = static_cast<int>(size);
  int height = 1;
  constexpr int usage =
      GRALLOC_USAGE_SW_READ_RARELY | GRALLOC_USAGE_PRIVATE_UNCACHED;
  int ret = Lock(usage, 0, 0, width, height, addr);
  if (ret == 0)
    Unlock();
  return ret;
}

BufferConsumer::BufferConsumer(LocalChannelHandle channel)
    : BASE(std::move(channel)) {
  const int ret = ImportBuffer();
  if (ret < 0) {
    ALOGE("BufferConsumer::BufferConsumer: Failed to import buffer: %s",
          strerror(-ret));
    Close(ret);
  }
}

std::unique_ptr<BufferConsumer> BufferConsumer::Import(
    LocalChannelHandle channel) {
  ATRACE_NAME("BufferConsumer::Import");
  ALOGD_IF(TRACE, "BufferConsumer::Import: channel=%d", channel.value());
  return BufferConsumer::Create(std::move(channel));
}

std::unique_ptr<BufferConsumer> BufferConsumer::Import(
    Status<LocalChannelHandle> status) {
  return Import(status ? status.take()
                       : LocalChannelHandle{nullptr, -status.error()});
}

int BufferConsumer::Acquire(LocalHandle* ready_fence) {
  return Acquire(ready_fence, nullptr, 0);
}

int BufferConsumer::Acquire(LocalHandle* ready_fence, void* meta,
                            size_t meta_size_bytes) {
  ATRACE_NAME("BufferConsumer::Acquire");
  LocalFence fence;
  auto return_value =
      std::make_pair(std::ref(fence), WrapBuffer(meta, meta_size_bytes));
  auto status = InvokeRemoteMethodInPlace<BufferHubRPC::ConsumerAcquire>(
      &return_value, meta_size_bytes);
  if (status && ready_fence)
    *ready_fence = fence.take();
  return status ? 0 : -status.error();
}

int BufferConsumer::Release(const LocalHandle& release_fence) {
  ATRACE_NAME("BufferConsumer::Release");
  return ReturnStatusOrError(InvokeRemoteMethod<BufferHubRPC::ConsumerRelease>(
      BorrowedFence(release_fence.Borrow())));
}

int BufferConsumer::ReleaseAsync() {
  ATRACE_NAME("BufferConsumer::ReleaseAsync");
  return ReturnStatusOrError(
      SendImpulse(BufferHubRPC::ConsumerRelease::Opcode));
}

int BufferConsumer::Discard() { return Release(LocalHandle()); }

int BufferConsumer::SetIgnore(bool ignore) {
  return ReturnStatusOrError(
      InvokeRemoteMethod<BufferHubRPC::ConsumerSetIgnore>(ignore));
}

BufferProducer::BufferProducer(int width, int height, int format, int usage,
                               size_t metadata_size, size_t slice_count)
    : BASE(BufferHubRPC::kClientPath) {
  ATRACE_NAME("BufferProducer::BufferProducer");
  ALOGD_IF(TRACE,
           "BufferProducer::BufferProducer: fd=%d width=%d height=%d format=%d "
           "usage=%d, metadata_size=%zu, slice_count=%zu",
           event_fd(), width, height, format, usage, metadata_size,
           slice_count);

  auto status = InvokeRemoteMethod<BufferHubRPC::CreateBuffer>(
      width, height, format, usage, metadata_size, slice_count);
  if (!status) {
    ALOGE(
        "BufferProducer::BufferProducer: Failed to create producer buffer: %s",
        status.GetErrorMessage().c_str());
    Close(-status.error());
    return;
  }

  const int ret = ImportBuffer();
  if (ret < 0) {
    ALOGE(
        "BufferProducer::BufferProducer: Failed to import producer buffer: %s",
        strerror(-ret));
    Close(ret);
  }
}

BufferProducer::BufferProducer(const std::string& name, int user_id,
                               int group_id, int width, int height, int format,
                               int usage, size_t meta_size_bytes,
                               size_t slice_count)
    : BASE(BufferHubRPC::kClientPath) {
  ATRACE_NAME("BufferProducer::BufferProducer");
  ALOGD_IF(TRACE,
           "BufferProducer::BufferProducer: fd=%d name=%s user_id=%d "
           "group_id=%d width=%d height=%d format=%d usage=%d, "
           "meta_size_bytes=%zu, slice_count=%zu",
           event_fd(), name.c_str(), user_id, group_id, width, height, format,
           usage, meta_size_bytes, slice_count);

  auto status = InvokeRemoteMethod<BufferHubRPC::CreatePersistentBuffer>(
      name, user_id, group_id, width, height, format, usage, meta_size_bytes,
      slice_count);
  if (!status) {
    ALOGE(
        "BufferProducer::BufferProducer: Failed to create/get persistent "
        "buffer \"%s\": %s",
        name.c_str(), status.GetErrorMessage().c_str());
    Close(-status.error());
    return;
  }

  const int ret = ImportBuffer();
  if (ret < 0) {
    ALOGE(
        "BufferProducer::BufferProducer: Failed to import producer buffer "
        "\"%s\": %s",
        name.c_str(), strerror(-ret));
    Close(ret);
  }
}

BufferProducer::BufferProducer(int usage, size_t size)
    : BASE(BufferHubRPC::kClientPath) {
  ATRACE_NAME("BufferProducer::BufferProducer");
  ALOGD_IF(TRACE, "BufferProducer::BufferProducer: usage=%d size=%zu", usage,
           size);
  const int width = static_cast<int>(size);
  const int height = 1;
  const int format = HAL_PIXEL_FORMAT_BLOB;
  const size_t meta_size_bytes = 0;
  const size_t slice_count = 1;
  auto status = InvokeRemoteMethod<BufferHubRPC::CreateBuffer>(
      width, height, format, usage, meta_size_bytes, slice_count);
  if (!status) {
    ALOGE("BufferProducer::BufferProducer: Failed to create blob: %s",
          status.GetErrorMessage().c_str());
    Close(-status.error());
    return;
  }

  const int ret = ImportBuffer();
  if (ret < 0) {
    ALOGE(
        "BufferProducer::BufferProducer: Failed to import producer buffer: %s",
        strerror(-ret));
    Close(ret);
  }
}

BufferProducer::BufferProducer(const std::string& name, int user_id,
                               int group_id, int usage, size_t size)
    : BASE(BufferHubRPC::kClientPath) {
  ATRACE_NAME("BufferProducer::BufferProducer");
  ALOGD_IF(TRACE,
           "BufferProducer::BufferProducer: name=%s user_id=%d group=%d "
           "usage=%d size=%zu",
           name.c_str(), user_id, group_id, usage, size);
  const int width = static_cast<int>(size);
  const int height = 1;
  const int format = HAL_PIXEL_FORMAT_BLOB;
  const size_t meta_size_bytes = 0;
  const size_t slice_count = 1;
  auto status = InvokeRemoteMethod<BufferHubRPC::CreatePersistentBuffer>(
      name, user_id, group_id, width, height, format, usage, meta_size_bytes,
      slice_count);
  if (!status) {
    ALOGE(
        "BufferProducer::BufferProducer: Failed to create persistent "
        "buffer \"%s\": %s",
        name.c_str(), status.GetErrorMessage().c_str());
    Close(-status.error());
    return;
  }

  const int ret = ImportBuffer();
  if (ret < 0) {
    ALOGE(
        "BufferProducer::BufferProducer: Failed to import producer buffer "
        "\"%s\": %s",
        name.c_str(), strerror(-ret));
    Close(ret);
  }
}

BufferProducer::BufferProducer(const std::string& name)
    : BASE(BufferHubRPC::kClientPath) {
  ATRACE_NAME("BufferProducer::BufferProducer");
  ALOGD_IF(TRACE, "BufferProducer::BufferProducer: name=%s", name.c_str());

  auto status = InvokeRemoteMethod<BufferHubRPC::GetPersistentBuffer>(name);
  if (!status) {
    ALOGE(
        "BufferProducer::BufferProducer: Failed to get producer buffer by name "
        "\"%s\": %s",
        name.c_str(), status.GetErrorMessage().c_str());
    Close(-status.error());
    return;
  }

  const int ret = ImportBuffer();
  if (ret < 0) {
    ALOGE(
        "BufferProducer::BufferProducer: Failed to import producer buffer "
        "\"%s\": %s",
        name.c_str(), strerror(-ret));
    Close(ret);
  }
}

BufferProducer::BufferProducer(LocalChannelHandle channel)
    : BASE(std::move(channel)) {
  const int ret = ImportBuffer();
  if (ret < 0) {
    ALOGE(
        "BufferProducer::BufferProducer: Failed to import producer buffer: %s",
        strerror(-ret));
    Close(ret);
  }
}

int BufferProducer::Post(const LocalHandle& ready_fence, const void* meta,
                         size_t meta_size_bytes) {
  ATRACE_NAME("BufferProducer::Post");
  return ReturnStatusOrError(InvokeRemoteMethod<BufferHubRPC::ProducerPost>(
      BorrowedFence(ready_fence.Borrow()), WrapBuffer(meta, meta_size_bytes)));
}

int BufferProducer::Gain(LocalHandle* release_fence) {
  ATRACE_NAME("BufferProducer::Gain");
  auto status = InvokeRemoteMethod<BufferHubRPC::ProducerGain>();
  if (!status)
    return -status.error();
  if (release_fence)
    *release_fence = status.take().take();
  return 0;
}

int BufferProducer::GainAsync() {
  ATRACE_NAME("BufferProducer::GainAsync");
  return ReturnStatusOrError(SendImpulse(BufferHubRPC::ProducerGain::Opcode));
}

std::unique_ptr<BufferProducer> BufferProducer::Import(
    LocalChannelHandle channel) {
  ALOGD_IF(TRACE, "BufferProducer::Import: channel=%d", channel.value());
  return BufferProducer::Create(std::move(channel));
}

std::unique_ptr<BufferProducer> BufferProducer::Import(
    Status<LocalChannelHandle> status) {
  return Import(status ? status.take()
                       : LocalChannelHandle{nullptr, -status.error()});
}

int BufferProducer::MakePersistent(const std::string& name, int user_id,
                                   int group_id) {
  ATRACE_NAME("BufferProducer::MakePersistent");
  return ReturnStatusOrError(
      InvokeRemoteMethod<BufferHubRPC::ProducerMakePersistent>(name, user_id,
                                                               group_id));
}

int BufferProducer::RemovePersistence() {
  ATRACE_NAME("BufferProducer::RemovePersistence");
  return ReturnStatusOrError(
      InvokeRemoteMethod<BufferHubRPC::ProducerRemovePersistence>());
}

std::unique_ptr<BufferProducer> BufferProducer::CreateUncachedBlob(
    size_t size) {
  return BufferProducer::Create(kUncachedBlobUsageFlags, size);
}

std::unique_ptr<BufferProducer> BufferProducer::CreatePersistentUncachedBlob(
    const std::string& name, int user_id, int group_id, size_t size) {
  return BufferProducer::Create(name, user_id, group_id,
                                kUncachedBlobUsageFlags, size);
}

}  // namespace dvr
}  // namespace android
